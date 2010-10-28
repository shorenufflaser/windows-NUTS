/* upsd.c - watches ups state files and answers queries 

   Copyright (C)
	1999	Russell Kroll <rkroll@exploits.org>
	2008	Arjen de Korte <adkorte-guest@alioth.debian.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "upsd.h"
#include "upstype.h"
#include "conf.h"

#include "netcmds.h"
#include "upsconf.h"

#ifndef WIN32
#include <sys/un.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
/* This override network system calls to adapt to Windows specificity */
#define W32_NETWORK_CALL_OVERRIDE
#include "wincompat.h"
#include <getopt.h>
static int install_flag = 0;
static int noservice_flag = 0;
#endif

#include "user.h"
#include "ctype.h"
#include "stype.h"
#include "ssl.h"
#include "sstate.h"
#include "desc.h"
#include "neterr.h"

#ifdef HAVE_WRAP
#include <tcpd.h>
int	allow_severity = LOG_INFO;
int	deny_severity = LOG_WARNING;
#endif	/* HAVE_WRAP */

	/* externally-visible settings and pointers */

	upstype_t	*firstups = NULL;

	/* default 15 seconds before data is marked stale */
	int	maxage = 15;

	/* preloaded to {OPEN_MAX} in main, can be overridden via upsd.conf */
	int	maxconn = 0;

	/* preloaded to STATEPATH in main, can be overridden via upsd.conf */
	char	*statepath = NULL;

	/* preloaded to NUT_DATADIR in main, can be overridden via upsd.conf */
	char	*datapath = NULL;

	/* everything else */
	const char	*progname;

static ctype_t	*firstclient = NULL;
/* static ctype_t	*lastclient = NULL; */

	/* default is to listen on all local interfaces */
static stype_t	*firstaddr = NULL;

#ifdef	HAVE_IPV6
static int 	opt_af = AF_UNSPEC;
#endif

typedef enum {
	DRIVER = 1,
	CLIENT,
	SERVER
} handler_type_t;

typedef struct {
	handler_type_t	type;
	void		*data;
} handler_t;

#ifndef WIN32
	/* pollfd  */
static struct pollfd	*fds = NULL;
#else
static HANDLE		*fds = NULL;
static HANDLE		svc_stop = NULL;
SERVICE_STATUS		SvcStatus;
SERVICE_STATUS_HANDLE	SvcStatusHandle;
#endif
static handler_t	*handler = NULL;

	/* pid file */
static char	pidfn[SMALLBUF];

	/* set by signal handlers */
static int	reload_flag = 0, exit_flag = 0;

#ifdef	HAVE_IPV6
static const char *inet_ntopW (struct sockaddr_storage *s)
{
	static char str[40];

	switch (s->ss_family)
	{
	case AF_INET:
		return inet_ntop (AF_INET, &(((struct sockaddr_in *)s)->sin_addr), str, 16);
	case AF_INET6:
		return inet_ntop (AF_INET6, &(((struct sockaddr_in6 *)s)->sin6_addr), str, 40);
	default:
		errno = EAFNOSUPPORT;
		return NULL;
	}
}
#endif

/* return a pointer to the named ups if possible */
upstype_t *get_ups_ptr(const char *name)
{
	upstype_t	*tmp;

	if (!name) {
		return NULL;
	}

	for (tmp = firstups; tmp; tmp = tmp->next) {
		if (!strcasecmp(tmp->name, name)) {
			return tmp;
		}
	}

	return NULL;
}

/* mark the data stale if this is new, otherwise cleanup any remaining junk */
static void ups_data_stale(upstype_t *ups)
{
	/* don't complain again if it's already known to be stale */
	if (ups->stale == 1) {
		return;
	}

	ups->stale = 1;

	upslogx(LOG_NOTICE, "Data for UPS [%s] is stale - check driver", ups->name);
}

/* mark the data ok if this is new, otherwise do nothing */
static void ups_data_ok(upstype_t *ups)
{
	if (ups->stale == 0) {
		return;
	}

	ups->stale = 0;

	upslogx(LOG_NOTICE, "UPS [%s] data is no longer stale", ups->name);
}

/* add another listening address */
void listen_add(const char *addr, const char *port)
{
	stype_t	*server;

	/* don't change listening addresses on reload */
	if (reload_flag) {
		return;
	}

	/* grab some memory and add the info */
	server = xcalloc(1, sizeof(*server));
	server->addr = xstrdup(addr);
	server->port = xstrdup(port);
	server->sock_fd = -1;
	server->next = firstaddr;

	firstaddr = server;

	upsdebugx(3, "listen_add: added %s:%s", server->addr, server->port);
}

/* create a listening socket for tcp connections */
static void setuptcp(stype_t *server)
{
#ifdef WIN32
        WSADATA WSAdata;
        WSAStartup(2,&WSAdata);
	atexit((void(*)(void))WSACleanup);
#endif

#ifndef	HAVE_IPV6
	struct hostent		*host;
	struct sockaddr_in	sockin;
	int	res, one = 1;

	memset(&sockin, '\0', sizeof(sockin));
	host = gethostbyname(server->addr);
#ifndef WIN32

	if (!host) {
		struct  in_addr	listenaddr;

		if (!inet_aton(server->addr, &listenaddr)) {
			fatal_with_errno(EXIT_FAILURE, "inet_aton");
		}

		host = gethostbyaddr(&listenaddr, sizeof(listenaddr), AF_INET);

		if (!host) {
			fatal_with_errno(EXIT_FAILURE, "gethostbyaddr");
		}
	}
#else
	unsigned long numeric_addr;
	numeric_addr = inet_addr(server->addr);
	if ( numeric_addr == INADDR_NONE ) {
		fatal_with_errno(EXIT_FAILURE, "inet_addr");
	}
	sockin.sin_addr.s_addr = numeric_addr;

#endif
	if ((server->sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		fatal_with_errno(EXIT_FAILURE, "socket");
	}

	res = setsockopt(server->sock_fd, SOL_SOCKET, SO_REUSEADDR, (void *) &one, sizeof(one));

	if (res != 0) {
		fatal_with_errno(EXIT_FAILURE, "setsockopt(SO_REUSEADDR)");
	}

	sockin.sin_family = AF_INET;
	sockin.sin_port = htons(atoi(server->port));

	memcpy(&sockin.sin_addr, host->h_addr, host->h_length);

	if (bind(server->sock_fd, (struct sockaddr *) &sockin, sizeof(sockin)) == -1) {
		fatal_with_errno(EXIT_FAILURE, "Can't bind TCP port %s", server->port);
	}

#ifndef WIN32
	if ((res = fcntl(server->sock_fd, F_GETFL, 0)) == -1) {
		fatal_with_errno(EXIT_FAILURE, "fcntl(get)");
	}

	if (fcntl(server->sock_fd, F_SETFL, res | O_NDELAY) == -1) {
		fatal_with_errno(EXIT_FAILURE, "fcntl(set)");
	}
#else
	server->Event = CreateEvent(NULL, /*Security,*/
				FALSE, /*auo-reset */
				FALSE, /*initial state*/
				NULL); /* no name */

	/* Associate socket event to the socket via its Event object */
	WSAEventSelect( server->sock_fd, server->Event, FD_ACCEPT );
#endif

	if (listen(server->sock_fd, 16)) {
		fatal_with_errno(EXIT_FAILURE, "listen");
	}
#else
	struct addrinfo		hints, *res, *ai;
	int	v = 0, one = 1;

	upsdebugx(3, "setuptcp: try to bind to %s port %s", server->addr, server->port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags		= AI_PASSIVE;
	hints.ai_family		= opt_af;
	hints.ai_socktype	= SOCK_STREAM;
	hints.ai_protocol	= IPPROTO_TCP;

	if ((v = getaddrinfo(server->addr, server->port, &hints, &res)) != 0) {
		if (v == EAI_SYSTEM) {
			fatal_with_errno(EXIT_FAILURE, "getaddrinfo");
		}

		fatalx(EXIT_FAILURE, "getaddrinfo: %s", gai_strerror(v));
	}

	for (ai = res; ai; ai = ai->ai_next) {
		int sock_fd;

		if ((sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
			upsdebug_with_errno(3, "setuptcp: socket");
			continue;
		}
		
		if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof(one)) != 0) {
			fatal_with_errno(EXIT_FAILURE, "setuptcp: setsockopt");
		}

		if (bind(sock_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
			upsdebug_with_errno(3, "setuptcp: bind");
			close(sock_fd);
			continue;
		}

		if ((v = fcntl(sock_fd, F_GETFL, 0)) == -1) {
			fatal_with_errno(EXIT_FAILURE, "setuptcp: fcntl(get)");
		}

		if (fcntl(sock_fd, F_SETFL, v | O_NDELAY) == -1) {
			fatal_with_errno(EXIT_FAILURE, "setuptcp: fcntl(set)");
		}

		if (listen(sock_fd, 16) < 0) {
			upsdebug_with_errno(3, "setuptcp: listen");
			close(sock_fd);
			continue;
		}

		server->sock_fd = sock_fd;
		break;
	}

	freeaddrinfo(res);
#endif

	/* don't fail silently */
	if (server->sock_fd < 0) {
		fatalx(EXIT_FAILURE, "not listening on %s port %s", server->addr, server->port);
	} else {
		upslogx(LOG_INFO, "listening on %s port %s", server->addr, server->port);
	}

	return;
}

/* decrement the login counter for this ups */
static void declogins(const char *upsname)
{
	upstype_t	*ups;

	ups = get_ups_ptr(upsname);

	if (!ups) {
		upslogx(LOG_INFO, "Tried to decrement invalid ups name (%s)", upsname);
		return;
	}

	ups->numlogins--;

	if (ups->numlogins < 0) {
		upslogx(LOG_ERR, "Programming error: UPS [%s] has numlogins=%d", ups->name, ups->numlogins);
	}
}

/* disconnect a client connection and free all related memory */
static void client_disconnect(ctype_t *client)
{
	if (!client) {
		return;
	}

	upsdebugx(2, "Disconnect from %s", client->addr);

	shutdown(client->sock_fd, 2);
	close(client->sock_fd);

	if (client->loginups) {
		declogins(client->loginups);
	}

	ssl_finish(client);

	pconf_finish(&client->ctx);

	if (client->prev) {
		client->prev->next = client->next;
	} else {
		/* deleting first entry */
		firstclient = client->next;
	}

	if (client->next) {
		client->next->prev = client->prev;
	} else {
		/* deleting last entry */
		/* lastclient = client->prev; */
	}

	free(client->addr);
	free(client->loginups);
	free(client->password);
	free(client->username);
	free(client);

	return;
}

/* send the buffer <sendbuf> of length <sendlen> to host <dest> */
int sendback(ctype_t *client, const char *fmt, ...)
{
	int	res, len;
	char ans[NUT_NET_ANSWER_MAX+1];
	va_list ap;

	if (!client) {
		return 0;
	}

	va_start(ap, fmt);
	vsnprintf(ans, sizeof(ans), fmt, ap);
	va_end(ap);

	len = strlen(ans);

	if (client->ssl) {
		res = ssl_write(client, ans, len);
	} else {
		res = write(client->sock_fd, ans, len);
	}

	upsdebugx(2, "write: [destfd=%d] [len=%d] [%s]", client->sock_fd, len, rtrim(ans, '\n'));

	if (len != res) {
		upslog_with_errno(LOG_NOTICE, "write() failed for %s", client->addr);
		client->last_heard = 0;
		return 0;	/* failed */
	}

	return 1;	/* OK */
}

/* just a simple wrapper for now */
int send_err(ctype_t *client, const char *errtype)
{
	if (!client) {
		return -1;
	}

	upsdebugx(4, "Sending error [%s] to client %s", errtype, client->addr);

	return sendback(client, "ERR %s\n", errtype);
}

/* disconnect anyone logged into this UPS */
void kick_login_clients(const char *upsname)
{
	ctype_t	*client, *cnext;

	for (client = firstclient; client; client = cnext) {

		cnext = client->next;

		/* if it's not logged in, don't check it */
		if (!client->loginups) {
			continue;
		}

		if (!strcmp(client->loginups, upsname)) {
			upslogx(LOG_INFO, "Kicking client %s (was on UPS [%s])\n", client->addr, upsname);
			client_disconnect(client);
		}
	}
}

/* make sure a UPS is sane - connected, with fresh data */
int ups_available(const upstype_t *ups, ctype_t *client)
{
	if (ups->sock_fd < 0) {
		send_err(client, NUT_ERR_DRIVER_NOT_CONNECTED);
		return 0;
	}

	if (ups->stale) {
		send_err(client, NUT_ERR_DATA_STALE);
		return 0;
	}

	/* must be OK */
	return 1;
}

/* check flags and access for an incoming command from the network */
static void check_command(int cmdnum, ctype_t *client, int numarg, 
	const char **arg)
{
	if (netcmds[cmdnum].flags & FLAG_USER) {
#ifdef HAVE_WRAP
		struct request_info	req;
#endif	/* HAVE_WRAP */

		if (!client->username) {
			send_err(client, NUT_ERR_USERNAME_REQUIRED);
			return;
		}

		if (!client->password) {
			send_err(client, NUT_ERR_PASSWORD_REQUIRED);
			return;
		}

#ifdef HAVE_WRAP
		request_init(&req, RQ_DAEMON, progname, RQ_CLIENT_ADDR, client->addr, RQ_USER, client->username, 0);

		if (!hosts_access(&req)) {
			/* tcp-wrappers says access should be denied */
			send_err(client, NUT_ERR_ACCESS_DENIED);
			return;
		}
#endif	/* HAVE_WRAP */
	}

	/* looks good - call the command */
	netcmds[cmdnum].func(client, numarg - 1, &arg[1]);
}

/* parse requests from the network */
static void parse_net(ctype_t *client)
{
	int	i;

	/* shouldn't happen */
	if (client->ctx.numargs < 1) {
		send_err(client, NUT_ERR_UNKNOWN_COMMAND);
		return;
	}

	for (i = 0; netcmds[i].name; i++) {
		if (!strcasecmp(netcmds[i].name, client->ctx.arglist[0])) {
			check_command(i, client, client->ctx.numargs, (const char **) client->ctx.arglist);
			return;
		}
	}

	/* fallthrough = not matched by any entry in netcmds */

	send_err(client, NUT_ERR_UNKNOWN_COMMAND);
}

/* answer incoming tcp connections */
static void client_connect(stype_t *server)
{
#ifndef	HAVE_IPV6
	struct	sockaddr_in csock;
#else
	struct	sockaddr_storage csock;
#endif
	socklen_t	clen;
	int		fd;
	ctype_t		*client;

	clen = sizeof(csock);
	fd = accept(server->sock_fd, (struct sockaddr *) &csock, &clen);

	if (fd < 0) {
		return;
	}

	client = xcalloc(1, sizeof(*client));

	client->sock_fd = fd;

	time(&client->last_heard);

#ifndef	HAVE_IPV6
	client->addr = xstrdup(inet_ntoa(csock.sin_addr));
#else
	client->addr = xstrdup(inet_ntopW(&csock));
#endif

	pconf_init(&client->ctx, NULL);

	if (firstclient) {
		firstclient->prev = client;
		client->next = firstclient;
	}

	firstclient = client;

/*
	if (lastclient) {
		client->prev = lastclient;
		lastclient->next = client;
	}

	lastclient = client;
 */
	upsdebugx(2, "Connect from %s", client->addr);
}

/* read tcp messages and handle them */
static void client_readline(ctype_t *client)
{
	char	buf[SMALLBUF];
	int	i, ret;

	if (client->ssl) {
		ret = ssl_read(client, buf, sizeof(buf));
	} else {
		ret = read(client->sock_fd, buf, sizeof(buf));
	}

	if (ret < 0) {
		upsdebug_with_errno(2, "Disconnect %s (read failure)", client->addr);
		client_disconnect(client);
		return;
	}

	if (ret == 0) {
		upsdebugx(2, "Disconnect %s (no data available)", client->addr);
		client_disconnect(client);
		return;
	}

	/* fragment handling code */
	for (i = 0; i < ret; i++) {

		/* add to the receive queue one by one */
		switch (pconf_char(&client->ctx, buf[i]))
		{
		case 1:
			time(&client->last_heard);	/* command received */
			parse_net(client);
			continue;

		case 0:
			continue;	/* haven't gotten a line yet */

		default:
			/* parse error */
			upslogx(LOG_NOTICE, "Parse error on sock: %s", client->ctx.errmsg);
			return;
		}
	}

	return;
}

void server_load(void)
{
	stype_t	*server;

	/* default behaviour if no LISTEN addres has been specified */
	if (!firstaddr) {
#ifdef	HAVE_IPV6
		if (opt_af != AF_INET) {
			listen_add("::1", string_const(PORT));
		}

		if (opt_af != AF_INET6) {
			listen_add("127.0.0.1", string_const(PORT));
		}
#else
		listen_add("127.0.0.1", string_const(PORT));
#endif
	}

	for (server = firstaddr; server; server = server->next) {
		setuptcp(server);
	}
}

void server_free(void)
{
	stype_t	*server, *snext;

	/* cleanup server fds */
	for (server = firstaddr; server; server = snext) {
		snext = server->next;

		if (server->sock_fd != -1) {
			close(server->sock_fd);
		}

		free(server->addr);
		free(server->port);
		free(server);
	}

	firstaddr = NULL;
}

static void client_free(void)
{
	ctype_t		*client, *cnext;

	/* cleanup client fds */
	for (client = firstclient; client; client = cnext) {
		cnext = client->next;
		client_disconnect(client);
	}
}

void driver_free(void)
{
	upstype_t	*ups, *unext;

	for (ups = firstups; ups; ups = unext) {
		unext = ups->next;

#ifndef WIN32
		if (ups->sock_fd != -1) {
			close(ups->sock_fd);
		}
#else
		if (ups->sock_fd != INVALID_HANDLE_VALUE) {
			DisconnectNamedPipe(ups->sock_fd);
			CloseHandle(ups->sock_fd);
			ups->sock_fd = INVALID_HANDLE_VALUE;
		}
#endif

		sstate_infofree(ups);
		sstate_cmdfree(ups);

		pconf_finish(&ups->sock_ctx);

		free(ups->fn);
		free(ups->name);
		free(ups->desc);
		free(ups);
	}
}

static void upsd_cleanup(void)
{
	if (strlen(pidfn) > 0) {
		unlink(pidfn);
	}

	/* dump everything */

	user_flush();
	desc_free();
	
	server_free();
	client_free();
	driver_free();

	free(statepath);
	free(datapath);
	free(certfile);

	free(fds);
	free(handler);
}

void poll_reload(void)
{
#ifndef WIN32
	int	ret;

	ret = sysconf(_SC_OPEN_MAX);

	if (ret < maxconn) {
		fatalx(EXIT_FAILURE,
			"Your system limits the maximum number of connections to %d\n"
			"but you requested %d. The server won't start until this\n"
			"problem is resolved.\n", ret, maxconn);
	}

	fds = xrealloc(fds, maxconn * sizeof(*fds));
	handler = xrealloc(handler, maxconn * sizeof(*handler));
#else
	fds = xrealloc(fds, MAXIMUM_WAIT_OBJECTS * sizeof(*fds));
	handler = xrealloc(handler, MAXIMUM_WAIT_OBJECTS * sizeof(*handler));
#endif
}

static void set_exit_flag(int sig)
{
	exit_flag = sig;
}
#ifdef WIN32
void ReportSvcStatus(   DWORD CurrentState,
                        DWORD Win32ExitCode,
                        DWORD WaitHint);
#endif
/* service requests and check on new data */
static void mainloop(void)
{
#ifndef WIN32
	int	i, ret, nfds = 0;
#else
	DWORD	ret;
	int 	nfds = 0;
#endif

	upstype_t	*ups;
	ctype_t		*client, *cnext;
	stype_t		*server;
	time_t	now;

	time(&now);

	if (reload_flag) {
		conf_reload();
		poll_reload();
		reload_flag = 0;
	}

#ifndef WIN32
	/* scan through driver sockets */
	for (ups = firstups; ups && (nfds < maxconn); ups = ups->next) {

		/* see if we need to (re)connect to the socket */
		if (ups->sock_fd < 0) {
			ups->sock_fd = sstate_connect(ups);
			continue;
		}

		/* throw some warnings if it's not feeding us data any more */
		if (sstate_dead(ups, maxage)) {
			ups_data_stale(ups);
		} else {
			ups_data_ok(ups);
		}

		fds[nfds].fd = ups->sock_fd;
		fds[nfds].events = POLLIN;

		handler[nfds].type = DRIVER;
		handler[nfds].data = ups;

		nfds++;
	}

	/* scan through client sockets */
	for (client = firstclient; client; client = cnext) {

		cnext = client->next;

		if (difftime(now, client->last_heard) > 60) {
			/* shed clients after 1 minute of inactivity */
			client_disconnect(client);
			continue;
		}

		if (nfds >= maxconn) {
			/* ignore clients that we are unable to handle */
			continue;
		}

		fds[nfds].fd = client->sock_fd;
		fds[nfds].events = POLLIN;

		handler[nfds].type = CLIENT;
		handler[nfds].data = client;

		nfds++;
	}

	/* scan through server sockets */
	for (server = firstaddr; server && (nfds < maxconn); server = server->next) {

		if (server->sock_fd < 0) {
			continue;
		}

		fds[nfds].fd = server->sock_fd;
		fds[nfds].events = POLLIN;

		handler[nfds].type = SERVER;
		handler[nfds].data = server;

		nfds++;
	}

	upsdebugx(2, "%s: polling %d filedescriptors", __func__, nfds);

	ret = poll(fds, nfds, 2000);

	if (ret == 0) {
		upsdebugx(2, "%s: no data available", __func__);
		return;
	}

	if (ret < 0) {
		upslog_with_errno(LOG_ERR, "%s", __func__);
		return;
	}

	for (i = 0; i < nfds; i++) {

		if (fds[i].revents & (POLLHUP|POLLERR|POLLNVAL)) {

			switch(handler[i].type)
			{
			case DRIVER:
				sstate_disconnect((upstype_t *)handler[i].data);
				break;
			case CLIENT:
				client_disconnect((ctype_t *)handler[i].data);
				break;
			case SERVER:
				upsdebugx(2, "%s: server disconnected", __func__);
				break;
			default:
				upsdebugx(2, "%s: <unknown> disconnected", __func__);
				break;
			}

			continue;
		}

		if (fds[i].revents & POLLIN) {

			switch(handler[i].type)
			{
			case DRIVER:
				sstate_readline((upstype_t *)handler[i].data);
				break;
			case CLIENT:
				client_readline((ctype_t *)handler[i].data);
				break;
			case SERVER:
				client_connect((stype_t *)handler[i].data);
				break;
			default:
				upsdebugx(2, "%s: <unknown> has data available", __func__);
				break;
			}

			continue;
		}
	}
#else
	/* scan through driver sockets */
	for (ups = firstups; ups && (nfds < maxconn); ups = ups->next) {

		/* see if we need to (re)connect to the socket */
		if (ups->sock_fd == INVALID_HANDLE_VALUE) {
			ups->sock_fd = sstate_connect(ups);
			continue;
		}

		/* throw some warnings if it's not feeding us data any more */
		if (sstate_dead(ups, maxage)) {
			ups_data_stale(ups);
		} else {
			ups_data_ok(ups);
		}

		if( ups->sock_fd != INVALID_HANDLE_VALUE) {
			fds[nfds] = ups->read_overlapped.hEvent;

			handler[nfds].type = DRIVER;
			handler[nfds].data = ups;

			nfds++;
		}
	}

	/* scan through client sockets */
	for (client = firstclient; client; client = cnext) {

		cnext = client->next;

		if (difftime(now, client->last_heard) > 60) {
			/* shed clients after 1 minute of inactivity */
			client_disconnect(client);
			continue;
		}

		if (nfds >= maxconn) {
			/* ignore clients that we are unable to handle */
			continue;
		}

		fds[nfds] = (HANDLE)client->sock_fd;

		handler[nfds].type = CLIENT;
		handler[nfds].data = client;

		nfds++;
	}

	/* scan through server sockets */
	for (server = firstaddr; server && (nfds < maxconn); server = server->next) {

		if (server->sock_fd < 0) {
			continue;
		}

		fds[nfds] = (HANDLE)server->Event;

		handler[nfds].type = SERVER;
		handler[nfds].data = server;

		nfds++;
	}

	/* Add SCM event handler in service mode*/
	if( !noservice_flag ) {
		fds[nfds] = svc_stop;
		nfds++;
	}

	upsdebugx(2, "%s: wait for %d filedescriptors", __func__, nfds);

	ret = WaitForMultipleObjects(nfds,fds,FALSE,2000);

	if (ret == WAIT_TIMEOUT) {
		upsdebugx(2, "%s: no data available", __func__);
		return;
	}

	if (ret == WAIT_FAILED) {
		DWORD err = GetLastError();
		err =err; /* remove compile time warning */
		upslog_with_errno(LOG_ERR, "%s", __func__);
		return;
	}

	if( !noservice_flag && fds[ret] == svc_stop ) {
		ReportSvcStatus( SERVICE_STOPPED, NO_ERROR, 0);
		set_exit_flag(1);
	} 
	else {
		switch(handler[ret].type) {
			case DRIVER:
				sstate_readline((upstype_t *)handler[ret].data);
				break;
			case CLIENT:
				client_readline((ctype_t *)handler[ret].data);
				break;
			case SERVER:
				client_connect((stype_t *)handler[ret].data);
				break;
			default:
				upsdebugx(2, "%s: <unknown> has data available", __func__);
				break;
		}
	}
#endif
}

static void help(const char *progname) 
{
	printf("Network server for UPS data.\n\n");
	printf("usage: %s [OPTIONS]\n", progname);

	printf("\n");
	printf("  -c <command>	send <command> via signal to background process\n");
	printf("		commands:\n");
	printf("		 - reload: reread configuration files\n");
	printf("		 - stop: stop process and exit\n");
	printf("  -D		raise debugging level\n");
	printf("  -h		display this help\n");
	printf("  -r <dir>	chroots to <dir>\n");
	printf("  -q		raise log level threshold\n");
	printf("  -u <user>	switch to <user> (if started as root)\n");
	printf("  -V		display the version of this software\n");
#ifdef	HAVE_IPV6
	printf("  -4		IPv4 only\n");
	printf("  -6		IPv6 only\n");
#endif
#ifdef WIN32
	printf("  -I		Install service\n");
	printf("  -N		Do not start as a service (for debug purpose)\n");
#endif

	exit(EXIT_SUCCESS);
}

#ifndef WIN32
static void set_reload_flag(int sig)
{
	reload_flag = 1;
}
#endif


static void setup_signals(void)
{
#ifndef WIN32
	struct sigaction	sa;

	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGHUP);
	sa.sa_flags = 0;

	/* basic signal setup to ignore SIGPIPE */
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	/* handle shutdown signals */
	sa.sa_handler = set_exit_flag;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* handle reloading */
	sa.sa_handler = set_reload_flag;
	sigaction(SIGHUP, &sa, NULL);
#endif
}

void check_perms(const char *fn)
{
#ifndef WIN32
	int	ret;
	struct stat	st;

	ret = stat(fn, &st);

	if (ret != 0) {
		fatal_with_errno(EXIT_FAILURE, "stat %s", fn);
	}

	/* include the x bit here in case we check a directory */
	if (st.st_mode & (S_IROTH | S_IXOTH)) {
		upslogx(LOG_WARNING, "%s is world readable", fn);
	}
#endif
}

#ifdef WIN32
void SvcInstall()
{
	SC_HANDLE SCManager;
	SC_HANDLE Service;
	TCHAR Path[MAX_PATH];

	if( !GetModuleFileName( NULL, Path, MAX_PATH ) ) {
		printf("Cannot install service (%d)\n", GetLastError());
		return;
	}

	SCManager = OpenSCManager(
		NULL,			/* local computer */
		NULL,			/* ServiceActive database */
		SC_MANAGER_ALL_ACCESS);	/* full access rights */

	if (NULL == SCManager) {
		upslogx(LOG_ERR, "OpenSCManager failed (%d)\n", (int)GetLastError());
		return;
	}

	Service = CreateService(
		SCManager,			/* SCM database */
		UPSD_SVCNAME,			/* name of service */
		UPSD_SVCNAME,			/* service name to display */
		SERVICE_ALL_ACCESS,		/* desired access */
		SERVICE_WIN32_OWN_PROCESS,	/* service type */
		SERVICE_DEMAND_START,		/* start type */
		SERVICE_ERROR_NORMAL,		/* error control type */
		Path,				/* path to service binary */
		NULL,				/* no load ordering group */
		NULL,				/* no tag identifier */
		NULL,				/* no dependencies */
		NULL,				/* LocalSystem account */
		NULL);				/* no password */

	if (Service == NULL) {
		upslogx(LOG_ERR, "CreateService failed (%d)\n", (int)GetLastError());
		CloseServiceHandle(SCManager);
		return;
	}
	else {
		upslogx(LOG_INFO, "Service installed successfully\n");
	}

	CloseServiceHandle(Service);
	CloseServiceHandle(SCManager);
		
}

void ReportSvcStatus( 	DWORD CurrentState,
			DWORD Win32ExitCode,
			DWORD WaitHint)
{
	static DWORD CheckPoint = 1;

	SvcStatus.dwCurrentState = CurrentState;
	SvcStatus.dwWin32ExitCode = Win32ExitCode;
	SvcStatus.dwWaitHint = WaitHint;

	if (CurrentState == SERVICE_START_PENDING)
		SvcStatus.dwControlsAccepted = 0;
	else SvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

	if ( (CurrentState == SERVICE_RUNNING) ||
		(CurrentState == SERVICE_STOPPED) ) {
		SvcStatus.dwCheckPoint = 0;
	}
	else {
		SvcStatus.dwCheckPoint = CheckPoint++;
	}

	/* report the status of the service to the SCM */
	SetServiceStatus( SvcStatusHandle, &SvcStatus );
}

void WINAPI SvcCtrlHandler( DWORD Ctrl )
{
	switch(Ctrl)
	{
		case SERVICE_CONTROL_STOP:
		case SERVICE_CONTROL_SHUTDOWN:
			ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);

			/* Signal the service to stop */
			SetEvent(svc_stop);
			ReportSvcStatus(SvcStatus.dwCurrentState, NO_ERROR, 0);

			return;

		case SERVICE_CONTROL_INTERROGATE:
			break;

		default:
			break;
	}
}

void WINAPI SvcMain( DWORD argc, LPTSTR *argv )
#else /* NOT WIN32 */
int main(int argc, char **argv)
#endif
{
	int	i, cmd = 0;
	char	*chroot_path = NULL;
	const char	*user = RUN_AS_USER;
	struct passwd	*new_uid = NULL;

#ifdef WIN32
	if( !noservice_flag) {
		/* Register the handler function for the service */
		SvcStatusHandle = RegisterServiceCtrlHandler(
				UPSD_SVCNAME,
				SvcCtrlHandler);

		if( !SvcStatusHandle ) {
			upslogx(LOG_ERR, "RegisterServiceCtrlHandler\n");
			return;
		}

		SvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		SvcStatus.dwServiceSpecificExitCode = 0;

		/* Report initial status to the SCM */
		ReportSvcStatus( SERVICE_START_PENDING, NO_ERROR, 3000 );
	}
#endif

	progname = xbasename(argv[0]);

	/* yes, xstrdup - the conf handlers call free on this later */
	statepath = xstrdup(dflt_statepath());
	datapath = xstrdup(NUT_DATADIR);

	/* set up some things for later */
	snprintf(pidfn, sizeof(pidfn), "%s/%s.pid", altpidpath(), progname);

	printf("Network UPS Tools %s %s\n", progname, UPS_VERSION);

	while ((i = getopt(argc, argv, "+h46p:qr:i:fu:Vc:DNI")) != -1) {
		switch (i) {
			case 'h':
				help(progname);
				break;
			case 'p':
			case 'i':
				fatalx(EXIT_FAILURE, "Specifying a listening addresses with '-i <address>' and '-p <port>'\n"
					"is deprecated. Use 'LISTEN <address> [<port>]' in 'upsd.conf' instead.\n"
					"See 'man 8 upsd.conf' for more information.");
			case 'q':
				nut_log_level++;
				break;
			case 'r':
				chroot_path = optarg;
				break;
			case 'u':
				user = optarg;
				break;
			case 'V':
				/* do nothing - we already printed the banner */
				exit(EXIT_SUCCESS);
#ifndef WIN32
			case 'c':
				if (!strncmp(optarg, "reload", strlen(optarg)))
					cmd = SIGCMD_RELOAD;
				if (!strncmp(optarg, "stop", strlen(optarg)))
					cmd = SIGCMD_STOP;

				/* bad command given */
				if (cmd == 0)
					help(progname);
				break;
#endif
			case 'D':
				nut_debug_level++;
				break;

#ifdef	HAVE_IPV6
			case '4':
				opt_af = AF_INET;
				break;

			case '6':
				opt_af = AF_INET6;
				break;
#endif
			case 'N':
			case 'I':
				/* nothing to do, already processed */
				break;

			default:
				help(progname);
				break;
		}
	}

	if (cmd) {
		sendsignalfn(pidfn, cmd);
		exit(EXIT_SUCCESS);
	}

	argc -= optind;
	argv += optind;

	if (argc != 0) {
		help(progname);
	}

	atexit(upsd_cleanup);

	setup_signals();

	open_syslog(progname);

	/* send logging to the syslog pre-background for later use */
	syslogbit_set();

	/* do this here, since getpwnam() might not work in the chroot */
	new_uid = get_user_pwent(user);

	if (chroot_path) {
		chroot_start(chroot_path);
	}

#ifndef WIN32
	/* default to system limit (may be overridden in upsd.conf */
	maxconn = sysconf(_SC_OPEN_MAX);
#else
	maxconn = 64;  /*FIXME : silly value just to compile */
#endif

	/* handle upsd.conf */
	load_upsdconf(0);	/* 0 = initial */

	/* start server */
	server_load();

	/* initialize SSL before we drop privileges (we may not be able to read the keyfile as non-root) */
	ssl_init();

	become_user(new_uid);
#ifndef WIN32
	if (chdir(statepath)) {
		fatal_with_errno(EXIT_FAILURE, "Can't chdir to %s", statepath);
	}
#endif

	/* check statepath perms */
	check_perms(statepath);

	/* handle ups.conf */
	read_upsconf();
	upsconf_add(0);		/* 0 = initial */
	poll_reload();

	if (num_ups == 0) {
		fatalx(EXIT_FAILURE, "Fatal error: at least one UPS must be defined in ups.conf");
	}

	/* try to bring in the var/cmd descriptions */
	desc_load();

	/* handle upsd.users */
	user_load();

	if (!nut_debug_level) {
		background();
		writepid(pidfn);
	} else {
		memset(pidfn, 0, sizeof(pidfn));
	}

#ifdef WIN32
	if( !noservice_flag) {
		svc_stop = CreateEvent(
				NULL,		/* default security attributes */
				TRUE,		/* manual reset event */
				FALSE,		/* not signaled */
				NULL);		/*no name */

		if( svc_stop == NULL ) {
			ReportSvcStatus( SERVICE_STOPPED, NO_ERROR, 0);
			return;
		}

		ReportSvcStatus( SERVICE_RUNNING, NO_ERROR, 0);
	}
#endif

	while (!exit_flag) {
		mainloop();
	}

	upslogx(LOG_INFO, "Signal %d: exiting", exit_flag);
#ifndef WIN32
	return EXIT_SUCCESS;
#else
	return;
#endif
}

#ifdef WIN32

int main(int argc, char **argv)
{
	int i;
	while ((i = getopt(argc, argv, "+h46p:qr:i:fu:Vc:DNI")) != -1) {
		switch (i) {
			case 'N':
				noservice_flag = 1;
				break;
			case 'I':
				install_flag = 1;
				break;
			default:
				break;
		}
	}

	/* Set optind to 0 not 1 because we use GNU extension '+' in optstring */
	optind = 0;

	if( install_flag ) {
		SvcInstall();
		return EXIT_SUCCESS;
	}
	if( !noservice_flag ) {
		SERVICE_TABLE_ENTRY DispatchTable[] =
		{
			{ UPSD_SVCNAME, (LPSERVICE_MAIN_FUNCTION) SvcMain },
			{ NULL, NULL }
		};

		/* This call returns when the service has stopped */
		if (!StartServiceCtrlDispatcher( DispatchTable ))
		{
			upslogx(LOG_ERR, "StartServiceCtrlDispatcher failed (%d): exiting, try -N to avoid starting as a service", (int)GetLastError());
		}
	}
	else {
		SvcMain(argc,argv);
	}

	return EXIT_SUCCESS;
}
#endif
