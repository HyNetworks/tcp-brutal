#ifndef BRUTAL_H
#define BRUTAL_H

#include <linux/version.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <net/tcp.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#error "TCP Brutal requires Linux 5.10 or later"
#endif

#define BRUTAL_VERSION_MAJOR 2
#define BRUTAL_VERSION_MINOR 0
#define BRUTAL_VERSION_PATCH 0
#define BRUTAL_VERSION ((BRUTAL_VERSION_MAJOR << 16) | (BRUTAL_VERSION_MINOR << 8) | BRUTAL_VERSION_PATCH)

#define TCP_BRUTAL_PARAMS 23301  // setsockopt/getsockopt: struct brutal_params
#define TCP_BRUTAL_VERSION 23302 // getsockopt: u32 (major << 16 | minor << 8 | patch)

#define INIT_PACING_RATE 125000 // 1 Mbps
#define INIT_CWND_GAIN 20

#define MIN_PACING_RATE 62500 // 500 Kbps
#define MAX_PACING_RATE 125000000000ULL // 1 Tbps; keeps all the u64 arithmetic in range
#define MIN_CWND_GAIN 5
#define MAX_CWND_GAIN 80
#define MIN_CWND 4

#define PKT_INFO_SLOTS 4

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
    spinlock_t lock; // protects next_ns and members
    u64 id;          // application group id, or rule id for a rule's group
    kuid_t uid;
    struct net *net;

    u64 rate;
    u32 cwnd_gain;
    u8 locked; // rule group: applications may not change the params
    u32 members;
    u64 sent_bytes;
    u64 next_ns;
};

// Per-socket state, lives in icsk_ca_priv
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

// brutal_cc.c: the congestion control
extern struct tcp_congestion_ops tcp_brutal_ops;
void brutal_update_rate(struct sock *sk);

// brutal_sockopt.c: groups and the application interface
struct brutal_group *brutal_group_alloc(u64 id);
void brutal_group_put(struct brutal_group *g);
void brutal_group_join(struct brutal *brutal, struct brutal_group *g);
void brutal_group_leave(struct brutal *brutal);
void brutal_sockopt_init(void);
void brutal_sockopt_install(struct sock *sk);
void brutal_sockopt_uninstall(struct sock *sk);

// brutal_rules.c: destination rules and /proc/net/tcp_brutal/rules
void brutal_apply_rule(struct sock *sk, struct brutal *brutal);
int brutal_rules_init(void);
void brutal_rules_exit(void);

#endif // BRUTAL_H
