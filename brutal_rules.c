// Destination rules: /proc/net/tcp_brutal/rules
//
// Every connection to a rule's prefix joins the rule's group, without
// application support. The route must select brutal for the prefix
// ("ip route ... congctl lock brutal"); brutalctl in tools/ does both.
//
// Read:  one rule per line, "dst=<prefix>/<len> rate=<bytes/s> gain=<tenths>
//        lock=<0|1> id=<n> members=<n> sent=<bytes>"
// Write: "add <prefix>[/<len>] rate=<bytes/s> [gain=<tenths>] [nolock]"
//        (replaces an existing rule for the same prefix, keeping its group)
//        "del <prefix>[/<len>]"   "flush"
#include <linux/inet.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/rculist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include "brutal.h"

#define RULES_MAX_CMD_LEN 256

struct brutal_rule
{
    struct list_head list;
    u8 family;
    u8 plen;
    union
    {
        __be32 v4;
        struct in6_addr v6;
    };
    struct brutal_group *group;
};

static LIST_HEAD(brutal_rules); // readers use RCU
static DEFINE_MUTEX(brutal_rules_mutex);
static u32 brutal_rule_next_id;

static bool brutal_rule_match(const struct brutal_rule *r, const struct sock *sk)
{
#if IS_ENABLED(CONFIG_IPV6)
    if (sk->sk_family == AF_INET6 && !ipv6_addr_v4mapped(&sk->sk_v6_daddr))
        return r->family == AF_INET6 && ipv6_prefix_equal(&sk->sk_v6_daddr, &r->v6, r->plen);
#endif
    return r->family == AF_INET &&
           !((sk->sk_daddr ^ r->v4) & (r->plen ? htonl(~0u << (32 - r->plen)) : 0));
}

// Join the group of the longest matching rule, if any
void brutal_apply_rule(struct sock *sk, struct brutal *brutal)
{
    struct brutal_rule *r, *best = NULL;

    rcu_read_lock();
    list_for_each_entry_rcu(r, &brutal_rules, list)
    {
        if (brutal_rule_match(r, sk) && (!best || r->plen > best->plen))
            best = r;
    }
    if (best)
    {
        refcount_inc(&best->group->refcnt);
        brutal_group_join(brutal, best->group);
    }
    rcu_read_unlock();
}

static int brutal_parse_prefix(char *s, struct brutal_rule *r)
{
    char *slash = strchr(s, '/');
    int plen = -1;

    if (slash)
    {
        *slash++ = 0;
        if (kstrtoint(slash, 10, &plen) || plen < 0)
            return -EINVAL;
    }
    if (in4_pton(s, -1, (u8 *)&r->v4, -1, NULL))
    {
        r->family = AF_INET;
        r->plen = plen < 0 ? 32 : plen;
        return plen > 32 ? -EINVAL : 0;
    }
    if (in6_pton(s, -1, r->v6.s6_addr, -1, NULL))
    {
        r->family = AF_INET6;
        r->plen = plen < 0 ? 128 : plen;
        return plen > 128 ? -EINVAL : 0;
    }
    return -EINVAL;
}

// Caller holds brutal_rules_mutex
static struct brutal_rule *brutal_rule_find(const struct brutal_rule *key)
{
    struct brutal_rule *r;

    list_for_each_entry(r, &brutal_rules, list)
    {
        if (r->family == key->family && r->plen == key->plen &&
            (r->family == AF_INET ? r->v4 == key->v4 : ipv6_addr_equal(&r->v6, &key->v6)))
            return r;
    }
    return NULL;
}

// Rule is already unlinked; wait for sockets that may still be joining through it
static void brutal_rule_free(struct brutal_rule *r)
{
    synchronize_rcu();
    WRITE_ONCE(r->group->locked, 0);
    brutal_group_put(r->group);
    kfree(r);
}

static int brutal_rule_add(char *args)
{
    struct brutal_rule key = {}, *r;
    struct brutal_group *g;
    u64 rate = 0;
    u32 gain = INIT_CWND_GAIN;
    bool lock = true;
    char *tok = strsep(&args, " ");
    int ret;

    if (!tok || (ret = brutal_parse_prefix(tok, &key)))
        return tok ? ret : -EINVAL;
    while ((tok = strsep(&args, " ")))
    {
        if (!*tok)
            continue;
        if (!strncmp(tok, "rate=", 5))
            ret = kstrtou64(tok + 5, 10, &rate);
        else if (!strncmp(tok, "gain=", 5))
            ret = kstrtou32(tok + 5, 10, &gain);
        else if (!strcmp(tok, "nolock"))
            lock = false;
        else if (!strcmp(tok, "lock"))
            lock = true;
        else
            ret = -EINVAL;
        if (ret)
            return -EINVAL;
    }
    if (rate < MIN_PACING_RATE || rate > MAX_PACING_RATE || gain < MIN_CWND_GAIN || gain > MAX_CWND_GAIN)
        return -EINVAL;

    mutex_lock(&brutal_rules_mutex);
    r = brutal_rule_find(&key);
    if (!r)
    {
        r = kmemdup(&key, sizeof(key), GFP_KERNEL);
        g = r ? brutal_group_alloc(++brutal_rule_next_id) : NULL;
        if (!g)
        {
            mutex_unlock(&brutal_rules_mutex);
            kfree(r);
            return -ENOMEM;
        }
        r->group = g;
        list_add_tail_rcu(&r->list, &brutal_rules);
    }
    g = r->group;
    WRITE_ONCE(g->rate, rate);
    WRITE_ONCE(g->cwnd_gain, gain);
    WRITE_ONCE(g->locked, lock);
    mutex_unlock(&brutal_rules_mutex);
    return 0;
}

static int brutal_rule_del(char *args)
{
    struct brutal_rule key = {}, *r;
    char *tok = strsep(&args, " ");
    int ret;

    if (!tok || (ret = brutal_parse_prefix(tok, &key)))
        return tok ? ret : -EINVAL;

    mutex_lock(&brutal_rules_mutex);
    r = brutal_rule_find(&key);
    if (r)
        list_del_rcu(&r->list);
    mutex_unlock(&brutal_rules_mutex);
    if (!r)
        return -ENOENT;
    brutal_rule_free(r);
    return 0;
}

static void brutal_rules_flush(void)
{
    struct brutal_rule *r;

    for (;;)
    {
        mutex_lock(&brutal_rules_mutex);
        r = list_first_entry_or_null(&brutal_rules, struct brutal_rule, list);
        if (r)
            list_del_rcu(&r->list);
        mutex_unlock(&brutal_rules_mutex);
        if (!r)
            return;
        brutal_rule_free(r);
    }
}

static int brutal_rules_show(struct seq_file *m, void *v)
{
    struct brutal_rule *r;

    rcu_read_lock();
    list_for_each_entry_rcu(r, &brutal_rules, list)
    {
        struct brutal_group *g = r->group;

        if (r->family == AF_INET)
            seq_printf(m, "dst=%pI4/%u", &r->v4, r->plen);
        else
            seq_printf(m, "dst=%pI6c/%u", &r->v6, r->plen);
        seq_printf(m, " rate=%llu gain=%u lock=%u id=%llu members=%u sent=%llu\n",
                   READ_ONCE(g->rate), READ_ONCE(g->cwnd_gain), READ_ONCE(g->locked),
                   g->id, READ_ONCE(g->members), READ_ONCE(g->sent_bytes));
    }
    rcu_read_unlock();
    return 0;
}

static int brutal_rules_open(struct inode *inode, struct file *file)
{
    return single_open(file, brutal_rules_show, NULL);
}

static ssize_t brutal_rules_write(struct file *file, const char __user *ubuf, size_t len, loff_t *off)
{
    char *buf, *args, *cmd;
    int ret;

    if (len > RULES_MAX_CMD_LEN)
        return -EINVAL;
    buf = memdup_user_nul(ubuf, len);
    if (IS_ERR(buf))
        return PTR_ERR(buf);

    args = strim(buf);
    cmd = strsep(&args, " ");
    if (!strcmp(cmd, "add"))
        ret = brutal_rule_add(args);
    else if (!strcmp(cmd, "del"))
        ret = brutal_rule_del(args);
    else if (!strcmp(cmd, "flush"))
        ret = (brutal_rules_flush(), 0);
    else
        ret = -EINVAL;

    kfree(buf);
    return ret ?: len;
}

static const struct proc_ops brutal_rules_proc_ops = {
    .proc_open = brutal_rules_open,
    .proc_read = seq_read,
    .proc_write = brutal_rules_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

int brutal_rules_init(void)
{
    struct proc_dir_entry *dir = proc_mkdir("tcp_brutal", init_net.proc_net);

    if (!dir || !proc_create("rules", 0644, dir, &brutal_rules_proc_ops))
    {
        remove_proc_subtree("tcp_brutal", init_net.proc_net);
        return -ENOMEM;
    }
    return 0;
}

void brutal_rules_exit(void)
{
    remove_proc_subtree("tcp_brutal", init_net.proc_net);
    brutal_rules_flush();
}
