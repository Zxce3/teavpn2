
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdalign.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <sys/sysinfo.h>
#include <teavpn2/base.h>
#include <teavpn2/net/iface.h>
#include <teavpn2/server/tcp.h>


#define EPT_MAP_SIZE (0xffffu)
#define EPT_MAP_NOP  (0xffffu)	/* Unused map (nop = no operation for index) */
#define EPT_MAP_PIPE (0x0u)
// #define EPT_MAP_ADD  (0x1u)
#define EPOLL_INEVT  (EPOLLIN | EPOLLPRI)

struct tcp_client {
	int			cli_fd;		/* Client TCP file descriptor */
	uint32_t		recv_c;		/* sys_recv counter           */
	uint32_t		send_c;		/* sys_send counter           */
	uint16_t		sidx;		/* Client slot index          */
	char			uname[64];
	bool			mt_act;		/* Mutex needs to be freed?   */
	bool			is_auth;	/* Is authenticated?          */
	bool			is_used;	/* Is used?                   */
	bool			is_conn;	/* Is connected?              */
	uint8_t			err_c;		/* Error counter              */
	struct_pad(0, 5);
	pthread_mutex_t		mutex;		/* Mutex is mutex             */
};


struct _cl_stk {
	/*
	 * Stack to retrieve client slot in O(1) time complexity
	 */
	uint16_t		sp;		/* Stack pointer              */
	uint16_t		max_sp;		/* Max stack pointer          */
	struct_pad(0, 4);
	uint16_t		*arr;		/* The array container        */
};


struct srv_tcp_state {
	pid_t			pid;		/* Main process PID           */
	int			epm_fd;		/* Epoll fd (main)            */
	int			ept_fd;		/* Epoll fd (thread)          */
	int			net_fd;		/* Main TCP socket fd         */
	int			tun_fd;		/* TUN/TAP fd                 */
	int			pipe_fd[2];	/* Pipe fd                    */
	bool			stop;		/* Stop the event loop?       */
	bool			mt_act;		/* Is mutex need to be freed? */
	bool			reconn;		/* Reconnect if conn dropped? */
	uint8_t			reconn_c;	/* Reconnect count            */
	struct _cl_stk		cl_stk;		/* Stack for slot resolution  */
	uint16_t		*ept_map;	/* Epoll thread map to client */
	struct tcp_client	*(*ipm)[256];	/* IP address map             */
	struct tcp_client	*clients;	/* Client slot                */
	struct srv_cfg		*cfg;		/* Config                     */
	pthread_t		thread;		/* Thread                     */
	pthread_mutex_t		mutex;		/* Mutex is mutex             */
};


static struct srv_tcp_state *g_state;


static void interrupt_handler(int sig)
{
	struct srv_tcp_state *state = g_state;

	state->stop = 0;
	putchar('\n');
	pr_notice("Signal %d (%s) has been caught", sig, strsignal(sig));
}


static void tcp_client_init(struct tcp_client *client, uint16_t sidx)
{
	client->cli_fd   = -1;
	client->recv_c   = 0;
	client->send_c   = 0;
	client->uname[0] = '_';
	client->uname[1] = '\0';
	client->sidx     = sidx;
	client->mt_act   = false;
	client->is_used  = false;
	client->is_auth  = false;
	client->is_conn  = false;
	client->err_c    = 0;
}


static int count_online_cpu(void)
{
	return (int)sysconf(_SC_NPROCESSORS_ONLN);
}


static void set_cpu_init(void)
{
	int err;
	int ncpu;
	int used = 0;

	cpu_set_t cs;
	cpu_set_t affinity;

	CPU_ZERO(&cs);
	if (unlikely(sched_getaffinity(0, sizeof(cs), &cs) < 0)) {
		err = errno;
		pr_err("sched_getaffinity: " PRERF, PREAR(err));
		prl_notice(4, "Continuing...");
		goto set_nice;
	}

	ncpu = count_online_cpu();
	CPU_ZERO(&affinity);
	for (int i = 0; (i < 128) && (used < 2); i++) {
		if (likely(CPU_ISSET(i, &cs))) {
			used++;
			CPU_SET(i, &affinity);
			prl_notice(4, "CPU_SET(%d, &affinity)", i);
		}
	}

	if (unlikely(sched_setaffinity(0, sizeof(cpu_set_t), &affinity) < 0)) {
		err = errno;
		pr_err("sched_setaffinity: " PRERF, PREAR(err));
	} else {
		if ((used != 2) || (used == ncpu))
			goto set_nice;

		prl_notice(4, "sched_setaffinity() success!");
		prl_notice(4, "You have %d online CPU(s)", ncpu);
		prl_notice(4, "I will only use %d specific CPU(s) for cache "
			   "and NUMA locality", used);
	}


set_nice:
	errno = 0;
	/* Unreliable return value, let's just check errno. */
	nice(-20);
	nice(-20);
	err = errno;
	if (unlikely(err != 0)) {
		pr_err("nice: " PRERF, PREAR(err));
		prl_notice(4, "nice is not mandatory, continuing...");
	} else {
		prl_notice(4, "nice(-20) success");
		prl_notice(4, "I am a high priority process now, excellent!");
	}
}


static int init_state(struct srv_tcp_state *state)
{
	int err;
	uint16_t max_conn;
	struct _cl_stk *cl_stk;
	uint16_t *ept_map = NULL;
	uint16_t *stack_arr = NULL;
	struct tcp_client *clients = NULL;
	struct tcp_client *(*ipm)[256] = NULL;

	err = pthread_mutex_init(&state->mutex, NULL);
	if (unlikely(err != 0)) {
		err = (err < 0) ? -err : err;
		pr_err("pthread_mutex_init: " PRERF, PREAR(err));
		return -err;
	}
	state->mt_act = true;

	max_conn = state->cfg->sock.max_conn;

	clients = calloc(max_conn, sizeof(struct tcp_client));
	if (unlikely(clients == NULL))
		goto out_err;

	stack_arr = calloc(max_conn, sizeof(uint16_t));
	if (unlikely(stack_arr == NULL))
		goto out_err;

	ept_map = calloc(EPT_MAP_SIZE, sizeof(uint16_t));
	if (unlikely(ept_map == NULL))
		goto out_err;

	ipm = calloc(256u, sizeof(struct tcp_client *[256u]));
	if (unlikely(ipm == NULL))
		goto out_err;

	cl_stk         = &state->cl_stk;
	cl_stk->sp     = max_conn; /* Stack growsdown, so start from high idx */
	cl_stk->max_sp = max_conn;
	cl_stk->arr    = stack_arr;

	for (uint16_t i = 0; i < max_conn; i++)
		tcp_client_init(clients + i, i);

	for (uint16_t i = 0; i < EPT_MAP_SIZE; i++)
		ept_map[i] = EPT_MAP_NOP;

	for (uint16_t i = 0; i < 256u; i++) {
		for (uint16_t j = 0; j < 256u; j++) {
			ipm[i][j] = NULL;
		}
	}

	state->epm_fd   = -1;
	state->ept_fd   = -1;
	state->net_fd   = -1;
	state->tun_fd   = -1;
	state->stop     = false;
	state->reconn   = true;
	state->reconn_c = 0;
	state->ept_map  = ept_map;
	state->ipm      = ipm;
	state->clients  = clients;
	state->pid      = getpid();

	prl_notice(0, "My PID is %d", state->pid);
	set_cpu_init();

	return 0;

out_err:
	err = errno;
	free(clients);
	free(stack_arr);
	free(ept_map);
	pr_err("calloc: Cannot allocate memory: " PRERF, PREAR(err));
	return -ENOMEM;
}


static int init_iface(struct srv_tcp_state *state)
{
	int fd;
	struct iface_cfg i;
	struct srv_iface_cfg *j = &state->cfg->iface;

	prl_notice(0, "Creating virtual network interface: \"%s\"...", j->dev);

	fd = tun_alloc(j->dev, IFF_TUN);
	if (unlikely(fd < 0))
		return -1;
	if (unlikely(fd_set_nonblock(fd) < 0))
		goto out_err;

	memset(&i, 0, sizeof(struct iface_cfg));
	strncpy(i.dev, j->dev, sizeof(i.dev) - 1);
	strncpy(i.ipv4, j->ipv4, sizeof(i.ipv4) - 1);
	strncpy(i.ipv4_netmask, j->ipv4_netmask, sizeof(i.ipv4_netmask) - 1);
	i.mtu = j->mtu;

	if (unlikely(!teavpn_iface_up(&i))) {
		pr_err("Cannot raise virtual network interface up");
		goto out_err;
	}

	state->tun_fd = fd;
	return 0;
out_err:
	close(fd);
	return -1;
}


static int init_pipe(struct srv_tcp_state *state)
{
	int err;

	prl_notice(0, "Initializing pipe...");
	if (pipe(state->pipe_fd) < 0) {
		err = errno;
		pr_err("pipe: " PRERF, PREAR(err));
		return -1;
	}

	return 0;
}


static int socket_setup(int fd, struct srv_cfg *cfg)
{
	int rv;
	int err;
	int y;
	socklen_t len = sizeof(y);
	const void *pv = (const void *)&y;

	y = 1;
	rv = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1;
	rv = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1;
	rv = setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1024 * 1024 * 2;
	rv = setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1024 * 1024 * 2;
	rv = setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 5000;
	rv = setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	/*
	 * TODO: Utilize `cfg` to set some socket options from config
	 */
	(void)cfg;
	return rv;
out_err:
	err = errno;
	pr_err("setsockopt(): " PRERF, PREAR(err));
	return rv;
}


static int init_socket(struct srv_tcp_state *state)
{
	int fd;
	int err;
	int retval;
	struct sockaddr_in addr;
	struct srv_sock_cfg *sock = &state->cfg->sock;

	prl_notice(0, "Creating TCP socket...");
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
	if (unlikely(fd < 0)) {
		err = errno;
		retval = -err;
		pr_err("socket(): " PRERF, PREAR(err));
		goto out_err;
	}

	prl_notice(0, "Setting up socket file descriptor...");
	retval = socket_setup(fd, state->cfg);
	if (unlikely(retval < 0))
		goto out_err;

	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(sock->bind_port);
	addr.sin_addr.s_addr = inet_addr(sock->bind_addr);

	retval = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (unlikely(retval < 0)) {
		err = errno;
		retval = -err;
		pr_err("bind(): " PRERF, PREAR(err));
		goto out_err;
	}

	retval = listen(fd, sock->backlog);
	if (unlikely(retval < 0)) {
		err = errno;
		retval = -err;
		pr_err("listen(): " PRERF, PREAR(err));
		goto out_err;
	}

	state->net_fd = fd;
	prl_notice(0, "Listening on %s:%u...", sock->bind_addr,
		   sock->bind_port);

	return retval;
out_err:
	if (fd > 0)
		close(fd);
	return retval;
}


static int epoll_add(int epl_fd, int fd, uint32_t events)
{
	int err;
	struct epoll_event event;

	/* Shut the valgrind up! */
	memset(&event, 0, sizeof(struct epoll_event));

	event.events = events;
	event.data.fd = fd;
	if (unlikely(epoll_ctl(epl_fd, EPOLL_CTL_ADD, fd, &event) < 0)) {
		err = errno;
		pr_err("epoll_ctl(EPOLL_CTL_ADD): " PRERF, PREAR(err));
		return -1;
	}
	return 0;
}


static int init_epoll(struct srv_tcp_state *state)
{
	int err;
	int ret;
	int epm_fd = -1;
	int ept_fd = -1;

	epm_fd = epoll_create(2);
	if (unlikely(epm_fd < 0))
		goto out_create_err;

	ret = epoll_add(epm_fd, state->tun_fd, EPOLL_INEVT);
	if (unlikely(ret < 0))
		goto out_err;

	ret = epoll_add(epm_fd, state->net_fd, EPOLL_INEVT);
	if (unlikely(ret < 0))
		goto out_err;

	ept_fd = epoll_create(state->cfg->sock.max_conn + 1);
	if (unlikely(ept_fd < 0))
		goto out_create_err;

	ret = epoll_add(ept_fd, state->pipe_fd[1], EPOLL_INEVT)	;
	if (unlikely(ret < 0))
		goto out_err;

	state->epm_fd = epm_fd;
	state->ept_fd = ept_fd;
	state->ept_map[state->pipe_fd[1]] = EPT_MAP_PIPE;
	return 0;

out_create_err:
	err = errno;
	pr_err("epoll_create(): " PRERF, PREAR(err));
out_err:
	if (epm_fd > 0)
		close(epm_fd);
	if (ept_fd > 0)
		close(ept_fd);
	return -1;
}


static void *thread_handler(void *_state_p)
{
	struct srv_tcp_state *state = _state_p;

	prl_notice(3, "Thread has been spawned with ID %lu", state->thread);
	pthread_mutex_unlock(&state->mutex);

	return state;
}


static int init_thread(struct srv_tcp_state *state)
{
	int err;

	prl_notice(3, "Spawning a thread...");
	pthread_mutex_lock(&state->mutex);

	err = pthread_create(&state->thread, NULL, thread_handler, state);
	if (unlikely(err != 0)) {
		err = (err < 0) ? -err : err;
		pr_err("pthread_create: " PRERF, PREAR(err));
		return -1;
	}

	err = pthread_detach(state->thread);
	if (unlikely(err != 0)) {
		err = (err < 0) ? -err : err;
		pr_err("pthread_detach: " PRERF, PREAR(err));
		return -1;
	}

	/* Let's wait until the thread has been spawned. */
	pthread_mutex_lock(&state->mutex);
	return 0;
}


static void destroy_state(struct srv_tcp_state *state)
{
	int epm_fd = state->epm_fd;
	int ept_fd = state->ept_fd;
	int tun_fd = state->tun_fd;
	int net_fd = state->net_fd;
	struct tcp_client *clients = state->clients;
	uint16_t max_conn = state->cfg->sock.max_conn;

	prl_notice(0, "Cleaning state...");

	if (likely(tun_fd != -1)) {
		prl_notice(0, "Closing state->tun_fd (%d)", tun_fd);
		close(tun_fd);
	}

	if (likely(net_fd != -1)) {
		prl_notice(0, "Closing state->net_fd (%d)", net_fd);
		close(net_fd);
	}

	if (likely(epm_fd != -1)) {
		prl_notice(0, "Closing state->epm_fd (%d)", epm_fd);
		close(epm_fd);
	}

	if (likely(ept_fd != -1)) {
		prl_notice(0, "Closing state->ept_fd (%d)", ept_fd);
		close(ept_fd);
	}

	if (unlikely(clients != NULL)) {
		while (likely(max_conn--)) {
			struct tcp_client *client = clients + max_conn;

			if (likely(client->mt_act)) {
				pthread_mutex_lock(&client->mutex);
			}

			if (unlikely(!client->is_used))
				goto clear;
			
			prl_notice(6, "Closing clients[%d].cli_fd (%d)",
				   max_conn, client->cli_fd);
			close(client->cli_fd);

			if (likely(client->mt_act)) {
				pthread_mutex_unlock(&client->mutex);
				pthread_mutex_destroy(&client->mutex);
			}

		clear:
			memset(client, 0, sizeof(struct tcp_client));
		}
	}

	free(state->ipm);
	free(state->clients);
	free(state->ept_map);
	free(state->cl_stk.arr);

	state->ipm = NULL;
	state->clients = NULL;
	state->ept_map = NULL;
	state->cl_stk.arr = NULL;
}


int teavpn_server_tcp_handler(struct srv_cfg *cfg)
{
	int retval;
	struct srv_tcp_state state;

	/* Shut the valgrind up! */
	memset(&state, 0, sizeof(struct srv_tcp_state));

	state.cfg = cfg;
	g_state = &state;
	signal(SIGHUP, interrupt_handler);
	signal(SIGINT, interrupt_handler);
	signal(SIGTERM, interrupt_handler);
	signal(SIGQUIT, interrupt_handler);
	signal(SIGPIPE, SIG_IGN);

	retval = init_state(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_iface(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_pipe(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_socket(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_epoll(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_thread(&state);
	if (unlikely(retval < 0))
		goto out;
	// retval = event_loop(&state);
out:
	destroy_state(&state);
	return retval;
}
