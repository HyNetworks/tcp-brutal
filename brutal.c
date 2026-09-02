#include <linux/module.h>
#include <linux/version.h>
#include <linux/hashtable.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/math64.h>
#include <net/tcp.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#error "TCP Brutal requires Linux 5.10 or later"
#endif

#if IS_ENABLED(CONFIG_IPV6)
#include <net/transp_v6.h>
#endif

#define INIT_PACING_RATE 125000 // 1 Mbps
#define INIT_CWND_GAIN 20

#define MIN_PACING_RATE 62500 // 500 Kbps
#define MIN_CWND_GAIN 5
#define MAX_CWND_GAIN 80
#define MIN_CWND 4

#define PKT_INFO_SLOTS 4
#define MIN_PKT_INFO_SAMPLES 50
#define MIN_ACK_RATE_PERCENT 80

// An unused reserved slot is returned to the group this long after its time
#define RESV_STALE_NS (20 * NSEC_PER_MSEC)
// Max lag of the group clock behind real time (token bucket depth)
#define GROUP_MAX_LAG_NS (2 * NSEC_PER_MSEC)

#define TCP_BRUTAL_PARAMS 23301  // setsockopt/getsockopt: struct brutal_params
#define TCP_BRUTAL_VERSION 23302 // getsockopt: u32 (major << 16 | minor << 8 | patch)

#define BRUTAL_VERSION_MAJOR 2
#define BRUTAL_VERSION_MINOR 0
#define BRUTAL_VERSION_PATCH 0
#define BRUTAL_VERSION ((BRUTAL_VERSION_MAJOR << 16) | (BRUTAL_VERSION_MINOR << 8) | BRUTAL_VERSION_PATCH)

struct brutal_pkt_info
{
    u32 sec;
    u32 acked;
    u32 losses;
};

// Sockets sharing one total rate.
// Before each transmit, a member reserves the next burst on the group's
// virtual clock (next_ns) and sets its own EDT (tcp_wstamp_ns) to that slot,
// so the group never exceeds rate while any single active member can use all of it.
struct brutal_group
{
    struct hlist_node node;
    refcount_t refcnt;
    spinlock_t lock; // protects next_ns
    u64 id;
    kuid_t uid;
    struct net *net;

    u64 rate;
    u32 cwnd_gain;
    u64 next_ns;
};

struct brutal
{
    u64 rate;
    u32 cwnd_gain;
    u32 ack_rate;               // percent, from the last rate update
    struct brutal_group *group; // NULL = per-socket rate (v1 behavior)

    u64 resv_start_ns;
    u64 resv_bytes;      // 0: no outstanding reservation
    u64 resv_bytes_sent; // tp->bytes_sent when reserved

    struct brutal_pkt_info slots[PKT_INFO_SLOTS];
};

struct brutal_params
{
    u64 rate;      // Send rate in bytes per second
    u32 cwnd_gain; // CWND gain in tenths (10=1.0)
    u64 group_id;  // 0 = per-socket rate; the 12-byte v1 struct is also accepted
} __packed;

#define BRUTAL_PARAMS_V1_SIZE offsetof(struct brutal_params, group_id)

static DEFINE_HASHTABLE(brutal_groups, 8);
static DEFINE_SPINLOCK(brutal_groups_lock);

static struct proto tcp_prot_override __ro_after_init;
#ifdef _TRANSP_V6_H
static struct proto tcpv6_prot_override __ro_after_init;
#endif

static struct brutal_group *brutal_group_get(struct sock *sk, u64 id)
{
    struct brutal_group *g, *ng = kzalloc(sizeof(*ng), GFP_KERNEL);

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
        refcount_set(&ng->refcnt, 1);
        spin_lock_init(&ng->lock);
        ng->id = id;
        ng->uid = sk->sk_uid;
        ng->net = sock_net(sk);
        ng->rate = INIT_PACING_RATE;
        ng->cwnd_gain = INIT_CWND_GAIN;
        hash_add(brutal_groups, &ng->node, id);
    }
    spin_unlock_bh(&brutal_groups_lock);
    return ng;
}

static void brutal_group_leave(struct brutal *brutal)
{
    struct brutal_group *g = brutal->group;

    if (!g)
        return;
    brutal->group = NULL;
    brutal->resv_bytes = 0;
    if (refcount_dec_and_test(&g->refcnt))
    {
        spin_lock_bh(&brutal_groups_lock);
        hash_del(&g->node);
        spin_unlock_bh(&brutal_groups_lock);
        kfree(g);
    }
}

// Configured rate compensated for this socket's loss
static u64 brutal_effective_rate(const struct brutal *brutal)
{
    u64 rate = brutal->group ? READ_ONCE(brutal->group->rate) : brutal->rate;

    return div_u64(rate * 100, brutal->ack_rate);
}

static void brutal_update_rate(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);

    u32 sec = div_u64(tp->tcp_mstamp, USEC_PER_SEC);
    u32 min_sec = sec - PKT_INFO_SLOTS;
    u32 acked = 0, losses = 0;
    u32 ack_rate; // Scaled by 100 (100=1.00) as kernel doesn't support float
    u64 rate;
    u32 cwnd, cwnd_gain;

    u32 mss = tp->mss_cache;
    u32 rtt_ms = (tp->srtt_us >> 3) / USEC_PER_MSEC;
    if (!rtt_ms)
        rtt_ms = 1;

    for (int i = 0; i < PKT_INFO_SLOTS; i++)
    {
        if (brutal->slots[i].sec >= min_sec)
        {
            acked += brutal->slots[i].acked;
            losses += brutal->slots[i].losses;
        }
    }
    if (acked + losses < MIN_PKT_INFO_SAMPLES)
        ack_rate = 100;
    else
    {
        ack_rate = acked * 100 / (acked + losses);
        if (ack_rate < MIN_ACK_RATE_PERCENT)
            ack_rate = MIN_ACK_RATE_PERCENT;
    }
    brutal->ack_rate = ack_rate;

    rate = brutal_effective_rate(brutal);
    cwnd_gain = brutal->group ? READ_ONCE(brutal->group->cwnd_gain) : brutal->cwnd_gain;

    // The order here is chosen carefully to avoid overflow as much as possible
    cwnd = div_u64(rate, MSEC_PER_SEC);
    cwnd *= rtt_ms;
    cwnd /= mss;
    cwnd *= cwnd_gain;
    cwnd /= 10;
    cwnd = max_t(u32, cwnd, MIN_CWND);

    // In a group, cwnd and sk_pacing_rate are sized for the full group rate so
    // that a member can take all of it at any moment; the group clock decides
    // the actual share.
    WARN_ON_ONCE((int)cwnd <= 0);
    tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);

    WRITE_ONCE(sk->sk_pacing_rate, min_t(u64, rate, READ_ONCE(sk->sk_max_pacing_rate)));
}

// Bytes tcp_write_xmit is about to send in one go (mirrors tcp_tso_autosize)
static u32 brutal_burst_estimate(const struct sock *sk, u64 rate, u32 unsent)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    unsigned long bytes = rate >> READ_ONCE(sk->sk_pacing_shift);
    u32 segs;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
    u32 r = tcp_min_rtt(tp) >> READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_tso_rtt_log);
    if (r < BITS_PER_TYPE(sk->sk_gso_max_size))
        bytes += sk->sk_gso_max_size >> r;
#endif
    bytes = min_t(unsigned long, bytes, sk->sk_gso_max_size);
    segs = clamp_t(u32, bytes / tp->mss_cache, 2, sk->sk_gso_max_segs);
    return min_t(u32, segs * tp->mss_cache, unsent);
}

// Called once at the start of every tcp_write_xmit / tcp_xmit_retransmit_queue,
// under the socket lock, before the kernel checks pacing. This is where a
// group member claims its slot on the group clock.
static u32 brutal_min_tso_segs(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);
    struct brutal_group *g = brutal->group;
    u64 now = tp->tcp_clock_cache;
    u64 rate, start;
    u32 unsent, burst;

    if (!g)
        return 2;

    rate = brutal_effective_rate(brutal);

    // Settle the previous reservation against what was actually sent
    if (brutal->resv_bytes)
    {
        u64 sent = tp->bytes_sent - brutal->resv_bytes_sent;
        s64 delta;

        if (!sent && (s64)(now - brutal->resv_start_ns) < (s64)RESV_STALE_NS)
        {
            // Pacing timer wake-up (or a blocked send): the slot is still ours
            if (tp->tcp_wstamp_ns < brutal->resv_start_ns)
                tp->tcp_wstamp_ns = brutal->resv_start_ns;
            return 2;
        }
        delta = (s64)sent - (s64)brutal->resv_bytes; // < 0: give time back
        spin_lock_bh(&g->lock);
        if (delta >= 0)
            g->next_ns += div64_u64((u64)delta * NSEC_PER_SEC, rate);
        else
            g->next_ns -= div64_u64((u64)(-delta) * NSEC_PER_SEC, rate);
        spin_unlock_bh(&g->lock);
        brutal->resv_bytes = 0;
    }

    // Reserve the next burst, if this call can actually send one
    unsent = tp->write_seq - tp->snd_nxt;
    if (!unsent)
    {
        if (tp->lost_out <= tp->retrans_out)
            return 2;
        unsent = tp->mss_cache; // retransmission pending
    }
    if (tcp_packets_in_flight(tp) >= tp->snd_cwnd || !after(tcp_wnd_end(tp), tp->snd_nxt))
        return 2;

    burst = brutal_burst_estimate(sk, rate, unsent);

    spin_lock_bh(&g->lock);
    start = max(g->next_ns, now - GROUP_MAX_LAG_NS);
    g->next_ns = start + div64_u64((u64)burst * NSEC_PER_SEC, rate);
    spin_unlock_bh(&g->lock);

    brutal->resv_start_ns = start;
    brutal->resv_bytes = burst;
    brutal->resv_bytes_sent = tp->bytes_sent;
    if (tp->tcp_wstamp_ns < start)
        tp->tcp_wstamp_ns = start;
    return 2;
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
    if (params.rate < MIN_PACING_RATE)
        return -EINVAL;
    if (params.cwnd_gain < MIN_CWND_GAIN || params.cwnd_gain > MAX_CWND_GAIN)
        return -EINVAL;

    // The proto-level override runs before the kernel would take the socket
    // lock, and the group pointer must not change under the transmit hook
    lock_sock(sk);
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
        brutal->group = g;
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

static int brutal_tcp_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return brutal_set_params(sk, optval, optlen);
    else
        return tcp_prot.setsockopt(sk, level, optname, optval, optlen);
}

#ifdef _TRANSP_V6_H
static int brutal_tcpv6_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return brutal_set_params(sk, optval, optlen);
    else
        return tcpv6_prot.setsockopt(sk, level, optname, optval, optlen);
}
#endif // _TRANSP_V6_H

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

static void brutal_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);

    if (sk->sk_family == AF_INET)
        sk->sk_prot = &tcp_prot_override;
#ifdef _TRANSP_V6_H
    else if (sk->sk_family == AF_INET6)
        sk->sk_prot = &tcpv6_prot_override;
#endif // _TRANSP_V6_H
    else
        BUG(); // WTF?

    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;

    memset(brutal, 0, sizeof(*brutal));
    brutal->rate = INIT_PACING_RATE;
    brutal->cwnd_gain = INIT_CWND_GAIN;
    brutal->ack_rate = 100;

    // Pacing is REQUIRED for Brutal to work
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void brutal_release(struct sock *sk)
{
    brutal_group_leave(inet_csk_ca(sk));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
static void brutal_main(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
#else
static void brutal_main(struct sock *sk, const struct rate_sample *rs)
#endif
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);

    u32 sec, slot;

    // Ignore invalid rate samples
    if (rs->delivered < 0 || rs->interval_us <= 0)
        return;

    sec = div_u64(tp->tcp_mstamp, USEC_PER_SEC);
    slot = sec % PKT_INFO_SLOTS;

    if (brutal->slots[slot].sec == sec)
    {
        // Current slot, update
        brutal->slots[slot].acked += rs->acked_sacked;
        brutal->slots[slot].losses += rs->losses;
    }
    else
    {
        // Uninitialized slot or slot expired
        brutal->slots[slot].sec = sec;
        brutal->slots[slot].acked = rs->acked_sacked;
        brutal->slots[slot].losses = rs->losses;
    }

    brutal_update_rate(sk);
}

static u32 brutal_undo_cwnd(struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

static u32 brutal_ssthresh(struct sock *sk)
{
    return tcp_sk(sk)->snd_ssthresh;
}

static struct tcp_congestion_ops tcp_brutal_ops = {
    .flags = TCP_CONG_NON_RESTRICTED,
    .name = "brutal",
    .owner = THIS_MODULE,
    .init = brutal_init,
    .release = brutal_release,
    .cong_control = brutal_main,
    .undo_cwnd = brutal_undo_cwnd,
    .ssthresh = brutal_ssthresh,
    .min_tso_segs = brutal_min_tso_segs,
};

static int __init brutal_register(void)
{
    BUILD_BUG_ON(sizeof(struct brutal) > ICSK_CA_PRIV_SIZE);
    BUILD_BUG_ON(sizeof(struct brutal_params) != 20);

    tcp_prot_override = tcp_prot;
    tcp_prot_override.setsockopt = brutal_tcp_setsockopt;
    tcp_prot_override.getsockopt = brutal_tcp_getsockopt;

#ifdef _TRANSP_V6_H
    tcpv6_prot_override = tcpv6_prot;
    tcpv6_prot_override.setsockopt = brutal_tcpv6_setsockopt;
    tcpv6_prot_override.getsockopt = brutal_tcpv6_getsockopt;
#endif // _TRANSP_V6_H

    return tcp_register_congestion_control(&tcp_brutal_ops);
}

static void __exit brutal_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_brutal_ops);
}

module_init(brutal_register);
module_exit(brutal_unregister);

MODULE_AUTHOR("The Hysteria Project");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Brutal");
MODULE_VERSION(__stringify(BRUTAL_VERSION_MAJOR) "." __stringify(BRUTAL_VERSION_MINOR) "." __stringify(BRUTAL_VERSION_PATCH));
