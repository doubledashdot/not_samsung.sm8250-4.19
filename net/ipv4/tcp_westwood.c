/*
 * TCP Westwood+: end-to-end bandwidth estimation for TCP
 *
 *      Angelo Dell'Aera: author of the first version of TCP Westwood+ in Linux 2.4
 *
 * Support at http://c3lab.poliba.it/index.php/Westwood
 * Main references in literature:
 *
 * - Mascolo S, Casetti, M. Gerla et al.
 *   "TCP Westwood: bandwidth estimation for TCP" Proc. ACM Mobicom 2001
 *
 * - A. Grieco, s. Mascolo
 *   "Performance evaluation of New Reno, Vegas, Westwood+ TCP" ACM Computer
 *     Comm. Review, 2004
 *
 * - A. Dell'Aera, L. Grieco, S. Mascolo.
 *   "Linux 2.4 Implementation of Westwood+ TCP with Rate-Halving :
 *    A Performance Evaluation Over the Internet" (ICC 2004), Paris, June 2004
 *
 * Westwood+ employs end-to-end bandwidth measurement to set cwnd and
 * ssthresh after packet loss. The probing phase is as the original Reno.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/inet_diag.h>
#include <net/tcp.h>
#include <linux/win_minmax.h>

#define CAL_SCALE 8
#define CAL_UNIT (1 << CAL_SCALE)

#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

static int debug = 0;
module_param(debug, int, 0644);

/* TCP Westwood structure */
struct westwood {
	u32    last_bdp;
	u32    rtt;
	u32    min_rtt_us;          /* minimum observed RTT */
	u32    rtt_cnt;
	u32    next_rtt_delivered;
	struct minmax bw;
	u32    prior_cwnd;
	u8     prev_ca_state;
};

/*
 * @tcp_westwood_create
 * This function initializes fields used in TCP Westwood+,
 * it is called after the initial SYN, so the sequence numbers
 * are correct but new passive connections we have no
 * information about RTTmin at this time so we simply set it to
 * TCP_WESTWOOD_INIT_RTT. This value was chosen to be too conservative
 * since in this way we're sure it will be updated in a consistent
 * way as soon as possible. It will reasonably happen within the first
 * RTT period of the connection lifetime.
 */
static void tcp_westwood_init(struct sock *sk)
{
	struct westwood *w = inet_csk_ca(sk);

	w->last_bdp = TCP_INIT_CWND;
	w->prior_cwnd = TCP_INIT_CWND;
	w->min_rtt_us = 0x7fffffff;
	w->rtt_cnt = 0;
	minmax_reset(&w->bw, w->rtt_cnt, 0);
	w->next_rtt_delivered = 0;
}

static void tcp_westwood_event(struct sock *sk, enum tcp_ca_event event)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct westwood *w = inet_csk_ca(sk);

	switch (event) {
	case CA_EVENT_COMPLETE_CWR:
		tp->snd_cwnd = tp->snd_ssthresh = w->last_bdp;
		break;
	default:
		/* don't care */
		break;
	}
}

static void tcp_westwood_state(struct sock *sk, u8 new_state)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (new_state == TCP_CA_Loss) {
		tp->snd_cwnd = tcp_packets_in_flight(tp) + 1;
	}
}

static u32 tcp_westwood_undo_cwnd(struct sock *sk)
{
	struct westwood *w = inet_csk_ca(sk);

	return max_t(u32, 2, w->prior_cwnd);
}

static u32 tcp_westwood_ssthresh(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct westwood *w = inet_csk_ca(sk);

	w->prior_cwnd = tp->snd_cwnd;
	return tcp_sk(sk)->snd_ssthresh;
}

static void tcp_westwood_cwnd_reduction(struct sock *sk, int newly_acked_sacked, int fast_rexmit)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int sndcnt = 0;
	int delta = tp->snd_ssthresh - tcp_packets_in_flight(tp);

	tp->prr_delivered += newly_acked_sacked;
	if (tcp_packets_in_flight(tp) > tp->snd_ssthresh) {
		u64 dividend = (u64)tp->snd_ssthresh * tp->prr_delivered + tp->prior_cwnd - 1;
		sndcnt = div_u64(dividend, tp->prior_cwnd) - tp->prr_out;
	} else {
		sndcnt = min_t(int, delta, max_t(int, tp->prr_delivered - tp->prr_out, newly_acked_sacked) + 1);
	}

	sndcnt = max(sndcnt, (fast_rexmit ? 1 : 0));
	tp->snd_cwnd = tcp_packets_in_flight(tp) + sndcnt;
}

static void tcp_westwood_cong_control(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct westwood *w = inet_csk_ca(sk);
	u8 prev_state = w->prev_ca_state, state = inet_csk(sk)->icsk_ca_state;
	u64 bw, bdp;

	if (!before(rs->prior_delivered, w->next_rtt_delivered)) {
		w->next_rtt_delivered = tp->delivered;
		w->rtt_cnt++;
	}

	bw = (u64)rs->delivered * BW_UNIT;
	do_div(bw, rs->interval_us);
	minmax_running_max(&w->bw, 10, w->rtt_cnt, bw);

	if (rs->rtt_us > 0 && rs->rtt_us <= w->min_rtt_us)
		w->min_rtt_us = rs->rtt_us;

	w->prev_ca_state = state;
	if (state == TCP_CA_Recovery && prev_state != TCP_CA_Recovery) {
		if (w->min_rtt_us == 0x7fffffff)
			w->last_bdp = TCP_INIT_CWND;
		else {
			bw = minmax_get(&w->bw);
			bdp = (u64)bw * w->min_rtt_us;
			w->last_bdp = (((bdp * CAL_UNIT) >> CAL_SCALE) + BW_UNIT - 1) / BW_UNIT;
		}
		tp->snd_ssthresh = max_t(u32, 2, tp->snd_cwnd >> 1);
	} else if (state == TCP_CA_Open && prev_state != TCP_CA_Open) {
		tp->snd_cwnd = w->last_bdp;
		tcp_westwood_cwnd_reduction(sk, rs->acked_sacked, 1);
	} else if (state == TCP_CA_Open) {
		tcp_reno_cong_avoid(sk, 0, rs->acked_sacked);
	}
	if (debug)
		printk("##st:%d->%d bw:%llu last_bdp:%d cwnd:%d minrtt:%d\n", prev_state, state, bw, w->last_bdp, tp->snd_cwnd, w->min_rtt_us);
}

/* Extract info for Tcp socket info provided via netlink. */
static size_t tcp_westwood_info(struct sock *sk, u32 ext, int *attr,
				union tcp_cc_info *info)
{
	const struct westwood *ca = inet_csk_ca(sk);

	if (ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
		info->vegas.tcpv_enabled = 1;
		info->vegas.tcpv_rttcnt	= 0;
		info->vegas.tcpv_rtt	= jiffies_to_usecs(ca->rtt);
		info->vegas.tcpv_minrtt	= ca->min_rtt_us;

		*attr = INET_DIAG_VEGASINFO;
		return sizeof(struct tcpvegas_info);
	}
	return 0;
}

static struct tcp_congestion_ops tcp_westwood __read_mostly = {
	.init		= tcp_westwood_init,
	.ssthresh	= tcp_westwood_ssthresh,
	.cong_control   = tcp_westwood_cong_control,
	.undo_cwnd      = tcp_westwood_undo_cwnd,
	.set_state	= tcp_westwood_state,
	.cwnd_event	= tcp_westwood_event,
	.get_info	= tcp_westwood_info,
	.owner		= THIS_MODULE,
	.name		= "westwood"
};

static int __init tcp_westwood_register(void)
{
	BUILD_BUG_ON(sizeof(struct westwood) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_westwood);
}

static void __exit tcp_westwood_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_westwood);
}

module_init(tcp_westwood_register);
module_exit(tcp_westwood_unregister);

MODULE_AUTHOR("Stephen Hemminger, Angelo Dell'Aera");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Westwood+");
