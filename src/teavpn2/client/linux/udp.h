// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021  Ammar Faizi
 */

#ifndef TEAVPN2__CLIENT__LINUX__UDP_H
#define TEAVPN2__CLIENT__LINUX__UDP_H

#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <stdatomic.h>
#include <teavpn2/mutex.h>
#include <teavpn2/stack.h>
#include <teavpn2/packet.h>
#include <teavpn2/client/common.h>


#define EPOLL_EVT_ARR_NUM 3u


struct cli_udp_state;


struct epl_thread {
	/*
	 * Pointer to the UDP state struct.
	 */
	struct cli_udp_state			*state;

	/*
	 * pthread reference.
	 */
	pthread_t				thread;

	int					epoll_fd;
	int					epoll_timeout;
	struct epoll_event			events[EPOLL_EVT_ARR_NUM];

	/*
	 * Is this thread online?
	 */
	_Atomic(bool)				is_online;

	uint16_t				idx;
	struct sc_pkt				*pkt;
};


struct cli_udp_state {
	/*
	 * @stop is false when event loop is supposed to run.
	 * @stop is true when event loop needs to be stopped.
	 */
	volatile bool				stop;

	/*
	 * @in_emergency will be true in case we run out of
	 * buffer, or when we are in the similar urgent
	 * situation that needs more attention.
	 */
	volatile bool				in_emergency;

	/*
	 * When we're exiting, the main thread will wait for
	 * the subthreads to exit for the given timeout. If
	 * the subthreads won't exit, @threads_wont_exit is
	 * set to true. This is an indicator that we are not
	 * allowed to free() and close() the resources as it
	 * may lead to UAF bug.
	 */
	bool					threads_wont_exit;

	/*
	 * @need_remove_iff is true when we need to remove
	 * virtual network interface configuration before
	 * exit, otherwise it's false.
	 */
	bool					need_remove_iff;


	/*
	 * @sig should contain signal after signal interrupt
	 * handler is called. If the signal interrupt handle
	 * is never called, the value of @sig should be -1.
	 */
	int					sig;

	event_loop_t				evt_loop;
	int					udp_fd;
	struct cli_cfg				*cfg;


	_Atomic(uint16_t)			n_on_threads;


	/*
	 * @tun_fds is an array of TUN file descriptors.
	 * Number of TUN file descriptor can be more than
	 * one because on Linux it's possible to parallelize
	 * the read/write to TUN fd.
	 */
	int					*tun_fds;

	struct sc_pkt				*pkt;

	union {
		/*
		 * For epoll event loop.
		 */
		struct {
			struct epl_thread	*epl_threads;
		};


		/*
		 * For io_uring event loop.
		 */
		struct {
			struct iou_thread	*iou_threads;
		};
	};
};


extern int teavpn2_udp_client_epoll(struct cli_udp_state *state);
extern int teavpn2_udp_client_io_uring(struct cli_udp_state *state);
extern ssize_t udp_client_do_send_to(int udp_fd, const void *pkt,
				     size_t send_len);
extern ssize_t udp_client_do_recv_from(int udp_fd, void *pkt, size_t recv_len);


static inline size_t do_send_to(int udp_fd, const void *pkt, size_t send_len)
{
	return udp_client_do_send_to(udp_fd, pkt, send_len);
}


static inline size_t do_recv_from(int udp_fd, void *pkt, size_t send_len)
{
	return udp_client_do_recv_from(udp_fd, pkt, send_len);
}


static __always_inline size_t cli_pprep(struct cli_pkt *cli_pkt, uint8_t type,
					uint16_t data_len, uint8_t pad_len)
{
	cli_pkt->type    = type;
	cli_pkt->len     = htons(data_len);
	cli_pkt->pad_len = pad_len;
	return (size_t)(data_len + PKT_MIN_LEN);
}


static __always_inline size_t cli_pprep_handshake(struct cli_pkt *cli_pkt)
{
	struct pkt_handshake *hand = &cli_pkt->handshake;
	struct teavpn2_version *cur = &hand->cur;
	const uint16_t data_len = (uint16_t)sizeof(*hand);

	memset(hand, 0, sizeof(*hand));
	cur->ver = VERSION;
	cur->patch_lvl = PATCHLEVEL;
	cur->sub_lvl = SUBLEVEL;
	strncpy2(cur->extra, EXTRAVERSION, sizeof(cur->extra));

	return cli_pprep(cli_pkt, TCLI_PKT_HANDSHAKE, data_len, 0);

}


static inline size_t cli_pprep_auth(struct cli_pkt *cli_pkt, const char *user,
				    const char *pass)
{
	struct pkt_auth *auth = &cli_pkt->auth;
	const uint16_t data_len = (uint16_t)sizeof(*auth);

	strncpy2(auth->username, user, sizeof(auth->username));
	strncpy2(auth->password, pass, sizeof(auth->password));
	return cli_pprep(cli_pkt, TCLI_PKT_AUTH, data_len, 0);
}


static inline int send_close_packet(struct cli_udp_state *state)
{
	size_t send_len;
	ssize_t send_ret;
	struct cli_pkt *cli_pkt = &state->pkt->cli;

	send_len = cli_pprep(cli_pkt, TCLI_PKT_CLOSE, 0, 0);
	send_ret = do_send_to(state->udp_fd, cli_pkt, send_len);
	pr_debug("send_close_packet() = %zd", send_ret);
	return unlikely(send_ret < 0) ? send_ret : 0;
}


#endif /* #ifndef TEAVPN2__CLIENT__LINUX__UDP_H */
