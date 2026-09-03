// Groups and the application interface: TCP_BRUTAL_PARAMS / TCP_BRUTAL_VERSION
#include <linux/hashtable.h>
#include <linux/slab.h>
#include "brutal.h"

#if IS_ENABLED(CONFIG_IPV6)
#include <net/transp_v6.h>
#endif

static DEFINE_HASHTABLE(brutal_groups, 8);
static DEFINE_SPINLOCK(brutal_groups_lock);

static struct proto tcp_prot_override __ro_after_init;
#ifdef _TRANSP_V6_H
static struct proto tcpv6_prot_override __ro_after_init;
#endif

struct brutal_group *brutal_group_alloc(u64 id)
{
    struct brutal_group *g = kzalloc(sizeof(*g), GFP_KERNEL);

    if (!g)
        return NULL;
    refcount_set(&g->refcnt, 1);
    spin_lock_init(&g->lock);
    g->id = id;
    g->rate = INIT_PACING_RATE;
    g->cwnd_gain = INIT_CWND_GAIN;
    return g;
}

// Application group keyed by id, uid and netns; created if missing
static struct brutal_group *brutal_group_get(struct sock *sk, u64 id)
{
    struct brutal_group *g, *ng = brutal_group_alloc(id);

    spin_lock_bh(&brutal_groups_lock);
    hash_for_each_possible(brutal_groups, g, node, id)
    {
        if (g->id == id && uid_eq(g->uid, sk->sk_uid) && g->net == sock_net(sk) &&
            refcount_inc_not_zero(&g->refcnt))
        {
            spin_unlock_bh(&brutal_groups_lock);
            kfree(ng);
            return g;
        }
    }
    if (ng)
    {
        ng->uid = sk->sk_uid;
        ng->net = sock_net(sk);
        hash_add(brutal_groups, &ng->node, id);
    }
    spin_unlock_bh(&brutal_groups_lock);
    return ng;
}

void brutal_group_put(struct brutal_group *g)
{
    if (!refcount_dec_and_test(&g->refcnt))
        return;
    spin_lock_bh(&brutal_groups_lock);
    hash_del(&g->node); // no-op for a rule's group, which is never hashed
    spin_unlock_bh(&brutal_groups_lock);
    kfree(g);
}

// Takes over the caller's reference on g
void brutal_group_join(struct brutal *brutal, struct brutal_group *g)
{
    brutal->group = g;
    spin_lock_bh(&g->lock);
    g->members++;
    spin_unlock_bh(&g->lock);
}

void brutal_group_leave(struct brutal *brutal)
{
    struct brutal_group *g = brutal->group;

    if (!g)
        return;
    brutal->group = NULL;
    brutal->resv_bytes = 0;
    spin_lock_bh(&g->lock);
    g->members--;
    spin_unlock_bh(&g->lock);
    brutal_group_put(g);
}

static int brutal_set_params(struct sock *sk, sockptr_t optval, unsigned int optlen)
{
    struct brutal *brutal = inet_csk_ca(sk);
    struct brutal_params params = {};

    if (optlen < BRUTAL_PARAMS_V1_SIZE)
        return -EINVAL;
    if (copy_from_sockptr(&params, optval, min_t(unsigned int, optlen, sizeof(params))))
        return -EFAULT;
    if (optlen < sizeof(params))
        params.group_id = 0;

    // Sanity checks
    if (params.rate < MIN_PACING_RATE || params.rate > MAX_PACING_RATE)
        return -EINVAL;
    if (params.cwnd_gain < MIN_CWND_GAIN || params.cwnd_gain > MAX_CWND_GAIN)
        return -EINVAL;

    // The proto-level override runs before the kernel would take the socket
    // lock, and the group pointer must not change under the transmit hook
    lock_sock(sk);
    if (inet_csk(sk)->icsk_ca_ops != &tcp_brutal_ops)
    {
        release_sock(sk); // the socket has been switched to another algorithm
        return -ENOPROTOOPT;
    }
    if (brutal->group && READ_ONCE(brutal->group->locked))
    {
        release_sock(sk);
        return -EPERM; // governed by a locked destination rule
    }
    if (!params.group_id)
        brutal_group_leave(brutal);
    else if (!brutal->group || brutal->group->id != params.group_id)
    {
        struct brutal_group *g = brutal_group_get(sk, params.group_id);
        if (!g)
        {
            release_sock(sk);
            return -ENOMEM;
        }
        brutal_group_leave(brutal);
        brutal_group_join(brutal, g);
    }
    if (brutal->group)
    {
        WRITE_ONCE(brutal->group->rate, params.rate);
        WRITE_ONCE(brutal->group->cwnd_gain, params.cwnd_gain);
    }
    brutal->rate = params.rate;
    brutal->cwnd_gain = params.cwnd_gain;
    brutal_update_rate(sk);
    release_sock(sk);

    return 0;
}

// Returns the params in effect:
// For a group member, the group's rate and cwnd_gain.
// A 12-byte (v1) buffer gets the first two fields.
static int brutal_get_params(struct sock *sk, char __user *optval, int __user *optlen)
{
    struct brutal *brutal = inet_csk_ca(sk);
    struct brutal_params params;
    int len;

    if (get_user(len, optlen))
        return -EFAULT;
    if (len < BRUTAL_PARAMS_V1_SIZE)
        return -EINVAL;
    len = min_t(int, len, sizeof(params));

    lock_sock(sk);
    if (inet_csk(sk)->icsk_ca_ops != &tcp_brutal_ops)
    {
        release_sock(sk);
        return -ENOPROTOOPT;
    }
    if (brutal->group)
    {
        params.rate = READ_ONCE(brutal->group->rate);
        params.cwnd_gain = READ_ONCE(brutal->group->cwnd_gain);
        params.group_id = brutal->group->id;
    }
    else
    {
        params.rate = brutal->rate;
        params.cwnd_gain = brutal->cwnd_gain;
        params.group_id = 0;
    }
    release_sock(sk);

    if (put_user(len, optlen) || copy_to_user(optval, &params, len))
        return -EFAULT;
    return 0;
}

static int brutal_get_version(char __user *optval, int __user *optlen)
{
    u32 version = BRUTAL_VERSION;
    int len;

    if (get_user(len, optlen))
        return -EFAULT;
    if (len < sizeof(version))
        return -EINVAL;
    len = sizeof(version);
    if (put_user(len, optlen) || copy_to_user(optval, &version, len))
        return -EFAULT;
    return 0;
}

static int brutal_tcp_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return brutal_set_params(sk, optval, optlen);
    else
        return tcp_prot.setsockopt(sk, level, optname, optval, optlen);
}

static int brutal_tcp_getsockopt(struct sock *sk, int level, int optname, char __user *optval, int __user *optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return brutal_get_params(sk, optval, optlen);
    else if (level == IPPROTO_TCP && optname == TCP_BRUTAL_VERSION)
        return brutal_get_version(optval, optlen);
    else
        return tcp_prot.getsockopt(sk, level, optname, optval, optlen);
}

#ifdef _TRANSP_V6_H
static int brutal_tcpv6_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return brutal_set_params(sk, optval, optlen);
    else
        return tcpv6_prot.setsockopt(sk, level, optname, optval, optlen);
}

static int brutal_tcpv6_getsockopt(struct sock *sk, int level, int optname, char __user *optval, int __user *optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return brutal_get_params(sk, optval, optlen);
    else if (level == IPPROTO_TCP && optname == TCP_BRUTAL_VERSION)
        return brutal_get_version(optval, optlen);
    else
        return tcpv6_prot.getsockopt(sk, level, optname, optval, optlen);
}
#endif // _TRANSP_V6_H

// Prepare the proto tables that route our sockopts to us
void __init brutal_sockopt_init(void)
{
    tcp_prot_override = tcp_prot;
    tcp_prot_override.setsockopt = brutal_tcp_setsockopt;
    tcp_prot_override.getsockopt = brutal_tcp_getsockopt;

#ifdef _TRANSP_V6_H
    tcpv6_prot_override = tcpv6_prot;
    tcpv6_prot_override.setsockopt = brutal_tcpv6_setsockopt;
    tcpv6_prot_override.getsockopt = brutal_tcpv6_getsockopt;
#endif // _TRANSP_V6_H
}

void brutal_sockopt_install(struct sock *sk)
{
    if (sk->sk_prot == &tcp_prot)
        sk->sk_prot = &tcp_prot_override;
#ifdef _TRANSP_V6_H
    else if (sk->sk_prot == &tcpv6_prot)
        sk->sk_prot = &tcpv6_prot_override;
#endif // _TRANSP_V6_H
    else
        WARN_ON_ONCE(sk->sk_family != AF_INET && sk->sk_family != AF_INET6);
}

void brutal_sockopt_uninstall(struct sock *sk)
{
    if (sk->sk_prot == &tcp_prot_override)
        sk->sk_prot = &tcp_prot;
#ifdef _TRANSP_V6_H
    else if (sk->sk_prot == &tcpv6_prot_override)
        sk->sk_prot = &tcpv6_prot;
#endif // _TRANSP_V6_H
}
