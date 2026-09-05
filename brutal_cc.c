// The congestion control: rate and cwnd, loss compensation, and the group clock
#include <linux/module.h>
#include <linux/math64.h>
#include "brutal.h"

#define MIN_PKT_INFO_SAMPLES 50
#define MIN_ACK_RATE_PERCENT 80

// An unused reserved slot is returned to the group this long after its time
#define RESV_STALE_NS (20 * NSEC_PER_MSEC)
// Max lag of the group clock behind real time (token bucket depth)
#define GROUP_MAX_LAG_NS (2 * NSEC_PER_MSEC)

// Configured rate compensated for this socket's loss
static u64 brutal_effective_rate(const struct brutal *brutal)
{
    u64 rate = brutal->group ? READ_ONCE(brutal->group->rate) : brutal->rate;

    return div_u64(rate * 100, brutal->ack_rate);
}

void brutal_update_rate(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);

    u32 sec = div_u64(tp->tcp_mstamp, USEC_PER_SEC);
    u32 min_sec = sec - PKT_INFO_SLOTS;
    u32 acked = 0, losses = 0;
    u32 ack_rate; // Scaled by 100 (100=1.00) as kernel doesn't support float
    u64 rate, bdp, cwnd;
    u32 cwnd_gain;

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

    // Packets in flight over one RTT at this rate, times the gain. Done in u64
    // with a microsecond RTT (floored at 1 ms) so short RTTs keep precision
    bdp = mul_u64_u64_div_u64(rate, max_t(u32, tp->srtt_us >> 3, USEC_PER_MSEC), USEC_PER_SEC);
    cwnd = div_u64(bdp * cwnd_gain, 10 * tp->mss_cache);

    // In a group, cwnd and sk_pacing_rate are sized for the full group rate so
    // that a member can take all of it at any moment; the group clock decides
    // the actual share.
    tp->snd_cwnd = clamp_t(u64, cwnd, MIN_CWND, min_t(u32, tp->snd_cwnd_clamp, INT_MAX));

    WRITE_ONCE(sk->sk_pacing_rate, min_t(u64, rate, READ_ONCE(sk->sk_max_pacing_rate)));
}

// Segments tcp_write_xmit puts in one skb at this rate (mirrors tcp_tso_autosize)
static u32 brutal_tso_segs_estimate(const struct sock *sk, u64 rate, u32 mss)
{
    unsigned long bytes = rate >> READ_ONCE(sk->sk_pacing_shift);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
    u32 r = tcp_min_rtt(tcp_sk(sk)) >> READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_tso_rtt_log);
    if (r < BITS_PER_TYPE(sk->sk_gso_max_size))
        bytes += sk->sk_gso_max_size >> r;
#endif
    bytes = min_t(unsigned long, bytes, sk->sk_gso_max_size);
    return clamp_t(u32, bytes / mss, 2, sk->sk_gso_max_segs);
}

// Bytes tcp_write_xmit is about to send in one go
static u32 brutal_burst_estimate(const struct sock *sk, u64 rate, u32 unsent)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    u32 segs = brutal_tso_segs_estimate(sk, rate, tp->mss_cache);

    segs = min(segs, tp->snd_cwnd - tcp_packets_in_flight(tp));
    unsent = min(unsent, tcp_wnd_end(tp) - tp->snd_nxt);
    return min_t(u32, segs * tp->mss_cache, unsent);
}

// Called from the TSO hook, which the kernel invokes once at the start of every
// tcp_write_xmit / tcp_xmit_retransmit_queue, under the socket lock, before it
// checks pacing. This is where a group member claims its slot on the group clock.
static void brutal_group_reserve(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);
    struct brutal_group *g = brutal->group;
    u64 now = tp->tcp_clock_cache;
    u64 rate, start;
    u32 unsent, burst;

    if (!g)
        return;

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
            return;
        }
        delta = (s64)sent - (s64)brutal->resv_bytes; // < 0: give time back
        spin_lock_bh(&g->lock);
        if (delta >= 0)
            g->next_ns += div64_u64((u64)delta * NSEC_PER_SEC, rate);
        else
            g->next_ns -= div64_u64((u64)(-delta) * NSEC_PER_SEC, rate);
        g->sent_bytes += sent;
        spin_unlock_bh(&g->lock);
        brutal->resv_bytes = 0;
    }

    // Reserve the next burst, if this call can actually send one
    unsent = tp->write_seq - tp->snd_nxt;
    if (!unsent)
    {
        if (tp->lost_out <= tp->retrans_out)
            return;
        unsent = tp->mss_cache; // retransmission pending
    }
    if (tcp_packets_in_flight(tp) >= tp->snd_cwnd || !after(tcp_wnd_end(tp), tp->snd_nxt))
        return;

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
}

#ifdef BRUTAL_HAVE_TSO_SEGS
// Kernels with the BBRv3 patchset (XanMod and others): tso_segs replaces
// tcp_tso_autosize, so the return value is the burst size, not a floor.
static u32 brutal_tso_segs(struct sock *sk, unsigned int mss_now)
{
    brutal_group_reserve(sk);
    return brutal_tso_segs_estimate(sk, READ_ONCE(sk->sk_pacing_rate), mss_now);
}
#else
static u32 brutal_min_tso_segs(struct sock *sk)
{
    brutal_group_reserve(sk);
    return 2;
}
#endif

static void brutal_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct brutal *brutal = inet_csk_ca(sk);

    brutal_sockopt_install(sk);

    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;

    memset(brutal, 0, sizeof(*brutal));
    brutal->rate = INIT_PACING_RATE;
    brutal->cwnd_gain = INIT_CWND_GAIN;
    brutal->ack_rate = 100;

    brutal_apply_rule(sk, brutal);
    if (brutal->group)
        brutal_update_rate(sk);

    // Pacing is REQUIRED for Brutal to work
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void brutal_release(struct sock *sk)
{
    brutal_group_leave(inet_csk_ca(sk));
    brutal_sockopt_uninstall(sk);
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

struct tcp_congestion_ops tcp_brutal_ops = {
    .flags = TCP_CONG_NON_RESTRICTED,
    .name = "brutal",
    .owner = THIS_MODULE,
    .init = brutal_init,
    .release = brutal_release,
    .cong_control = brutal_main,
    .undo_cwnd = brutal_undo_cwnd,
    .ssthresh = brutal_ssthresh,
#ifdef BRUTAL_HAVE_TSO_SEGS
    .tso_segs = brutal_tso_segs,
#else
    .min_tso_segs = brutal_min_tso_segs,
#endif
};

static int __init brutal_register(void)
{
    int ret;

    BUILD_BUG_ON(sizeof(struct brutal) > ICSK_CA_PRIV_SIZE);
    BUILD_BUG_ON(sizeof(struct brutal_params) != 20);

    brutal_sockopt_init();
    ret = brutal_rules_init();
    if (ret)
        return ret;
    ret = tcp_register_congestion_control(&tcp_brutal_ops);
    if (ret)
        brutal_rules_exit();
    return ret;
}

static void __exit brutal_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_brutal_ops);
    brutal_rules_exit();
}

module_init(brutal_register);
module_exit(brutal_unregister);

MODULE_AUTHOR("The Hysteria Project");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Brutal");
MODULE_VERSION(__stringify(BRUTAL_VERSION_MAJOR) "." __stringify(BRUTAL_VERSION_MINOR) "." __stringify(BRUTAL_VERSION_PATCH));
