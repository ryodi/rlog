#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define VERSION "1.0"

#define DEBUG_ENVVAR    "RLOG_DEBUG"
#define DEFAULT_BIND    "127.0.0.1:1040"
#define DEFAULT_PORT    1040
#define BACKLOG         64
#define DATE_FMT        "%b %d %H:%M:%S  "   /* Jan 14 19:23:08 */
#define DATE_FMT_EXLEN  17                   /* expanded string length */
#define TRUNC_MSG       "...(clipped)\n"
#define TRUNC_MSG_LEN   strlen(TRUNC_MSG)


#define MAX_EVENTS    64
#define MAX_CLIENTS   512
#define MAX_MSGLEN    8192
#define MAX_MSGIN     MAX_MSGLEN - DATE_FMT_EXLEN  /* date will be prefixed */
#define RING_SIZE     1024

#define NOT_A_CLIENT -1

static int verbose = 0;
static inline void debug1(const char *fmt, ...)
{
	if (!verbose) return;

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
static inline void debug2(const char *prefix, int rc, int ok)
{
	if (!verbose) return;
	if (ok) {
		fprintf(stderr, "%s returned rc %i (no error)\n",
			prefix, rc);
	} else {
		fprintf(stderr, "%s returned rc %i (errno = %d '%s')\n",
			prefix, rc, errno, strerror(errno));
	}
}
static inline void debug3(const char *prefix, const char *data, size_t len)
{
	if (!verbose) return;

	char *out;
	size_t outlen;
	int i, j;

	/* count the number of chars to escape */
	outlen = len;
	for (i = 0; i < len; i++) {
		if (data[i] == '\t' || data[i] == '\n' || data[i] == '\r') {
			outlen++;
		}
	}

	out = malloc(sizeof(char) * outlen);
	for (i = 0, j = 0; i < len && j < outlen; ) {
		switch (data[i]) {
		case '\t': out[j++] = '\\'; out[j++] = 't'; i++; break;
		case '\r': out[j++] = '\\'; out[j++] = 'r'; i++; break;
		case '\n': out[j++] = '\\'; out[j++] = 'n'; i++; break;
		default: out[j++] = data[i++]; break;
		}
	}

	fprintf(stderr, "%s: [%.*s]\n", prefix, (int)outlen, out);
}

/*
   `struct client` rolls up all of the salient details for each
   connected client, including where they are in the ring buffer,
   their current offset into that message (to allow for short writes)
   and their connected socket file descriptor.

   If `client.fd` is NOT_A_CLIENT, this client slot is open and unused.
 */
struct client {
	int           fd;
	size_t        offset;
	unsigned long msgno;
};

static void disconnect(struct client *c, int epfd)
{
	int rc;
	struct epoll_event ev;

	debug1("disconnect(): disconnecting client [%p] %i from epfd %i\n", c, c->fd, epfd);
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLOUT|EPOLLHUP;
	ev.data.fd = c->fd;
	rc = epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, &ev);
	debug2("disconnect(): epoll_ctl(DEL)", rc, rc == 0);
	if (rc != 0) {
		fprintf(stderr, "unable to unregister client %i from epoll set: %s (error %i)\n",
		                c->fd, strerror(errno), errno);
	}
	debug1("disconnect(): closing fd %i\n", c->fd);
	close(c->fd);
	c->fd = NOT_A_CLIENT;
}

static int send_to(struct client *c, int epfd, void *data, size_t len)
{
	ssize_t nwrit, n;

	debug1("send_to(): sending %i bytes to client [%p] %i\n", len, c, c->fd);
	debug3("send_to():   data", data, len);

	n = 0;
	while (len > 0) {
		debug1("send_to(): attempting to write %i bytes to client [%p] %i\n", len, c, c->fd);
		nwrit = write(c->fd, data, len);
		debug2("send_to(): write(fd,data,remaining)", nwrit, nwrit > 0);
		if (nwrit < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			debug1("send_to(): client [%p] %i has no more room for data.  only wrote %i bytes\n",
					c, c->fd, n);
			break;
		}
		if (nwrit <= 0) {
			fprintf(stderr, "failed to send data to client %d: %s (error %d)\n",
					c->fd, strerror(errno), errno);

			disconnect(c, epfd);
			return nwrit;
		}

		debug1("send_to(): wrote %i bytes to client [%p] %i\n", nwrit, c, c->fd);
		data += nwrit;
		len  -= nwrit;
		n    += nwrit;
	}
	debug1("send_to(): final summary: wrote %i bytes to client [%p] %i\n", n, c, c->fd);
	return n;
}

/* `struct msg` represents a single message in the ring buffer.
   The ring buffer itself is merely an array of n msg structs,
   and we maintain a separate (unsigned long) variable for the
   current message number, and another (unsigned int) variable
   for the size of the array.

   Note that `msg.data` is _not_ NULL-terminated, since we keep
   track of the length of the used portion of the message string.
 */
struct msg {
	size_t len;
	char   data[MAX_MSGLEN];
};

struct buf {
	size_t len;
	int    prefix;
	char   data[MAX_MSGIN];
};

static int buf_hasline(struct buf *b);
static void buf_discard(struct buf *b, const char *to);

/* returns 1 if b has one full line in it */
static int buf_hasline(struct buf *b)
{
	return memchr(b->data, '\n', b->len) != NULL;
}

/* discard all bytes between start of data and `to` */
static void buf_discard(struct buf *b, const char *to)
{
	debug1("buf_discard(): discarding %i bytes from buffer [%p]", (to - b->data) + 1);
	debug3("", b->data, (to - b->data) + 1);
	memmove(b->data, to + 1, b->len - (to - b->data) - 1);
	b->len = b->len - (to - b->data) - 1;
}

/* move the first line out of buffer and into m */
static void buf_moveto(struct msg *m, struct buf *b)
{
	char *nl;
	size_t n;
	time_t ts;
	struct tm *now;

	nl = memchr(b->data, '\n', b->len);
	if (!nl) return;

	debug1("buf_moveto(): moving %i bytes from buffer [%p] to msg [%p]\n",
		(nl - b->data) + 1, b, m);

	m->len = DATE_FMT_EXLEN + (nl - b->data) + 1;

	ts = time(NULL);
	now = localtime(&ts);

	/* 'max' arg to strftime _includes_ the NULL terminator... */
	n = strftime(m->data, DATE_FMT_EXLEN + 1, DATE_FMT, now);
	if (n == 0) {
		memset(m->data, ' ', DATE_FMT_EXLEN);
	}

	memcpy(m->data + n, b->data, m->len);
	buf_discard(b, nl);
}

/* read from fd into b, filling b if possible */
static int buf_fill(struct buf *b, int fd)
{
	ssize_t n;
	char *nl;

	debug1("buf_fill(): filling buffer [%p] from fd %i\n", b, fd);
	for (;;) {
		debug1("buf_fill(): attempting to read up to %i bytes into buffer [%p] from fd %i\n",
			MAX_MSGIN - b->len, b, fd);
		n = read(fd, b->data + b->len, MAX_MSGIN - b->len);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
		if (n <= 0) return n;

		debug1("buf_fill(): read %i bytes into buffer [%p] from fd %i\n",
			n, b, fd);

		b->len += n;
		if (b->prefix) {
			while (b->prefix) {
				debug1("buf_fill(): buffer[%p] operating in prefix mode; looking for %i more newlines\n",
					b, b->prefix);
				nl = memchr(b->data, '\n', b->len);
				if (nl) {
					b->prefix--;
					debug1("buf_fill(): newline found in buffer [%p], discarding leading characters (%i more newlines to go)\n",
						b, b->prefix);
					buf_discard(b, nl);
				} else {
					debug1("buf_fill(): no newline found in buffer [%p]; discarding everything\n", b);
					b->len = 0;
					return 1;
				}
			}

		} else if (b->len == MAX_MSGIN) {
			debug1("buf_fill(): buffer [%p] is full; looking for to see if we need to truncate.\n", b);
			if (!buf_hasline(b)) {
				debug1("buf_fill(): no complete line found in buffer [%p]; appending truncation marker and flipping into prefix mode\n", b);
				memcpy(b->data + b->len - TRUNC_MSG_LEN, TRUNC_MSG, TRUNC_MSG_LEN);
				b->prefix = 2;
			}
			return 1;
		}
	}
}

/* set fd to O_NONBLOCK */
static void noblocking(int fd)
{
	int rc, flags;

	debug1("noblocking(): set fd %i to O_NONBLOCK\n");
	rc = fcntl(fd, F_GETFL);
	debug2("noblocking(): fcntl(fd, F_GETFL)", rc, rc == 0);
	if (rc < 0) {
		perror("fcntl(F_GETFL) in noblocking()");
		exit(4);
	}
	debug1("noblocking(): flags for fd %i are %08x\n", rc);
	debug1("noblocking(): OR-ing in% %08x (O_NONBLOCK)\n", O_NONBLOCK);
	rc |= O_NONBLOCK;
	debug1("noblocking(): setting flags for fd %i to %08x\n", rc);
	rc = fcntl(fd, F_SETFL, rc, rc == 0);
	debug2("noblocking(): fcntl(fd, F_SETFL, flags)", rc, rc == 0);
	if (rc < 0) {
		perror("fcntl(F_SETFL) in noblocking()");
		exit(4);
	}
}
/* parse an address spec into an IPv4 struct sockaddr_in.
   spec can be one of:
     "host:port"
     "host"
     ":port"
 */
static void * parse_addr(const char *spec)
{
	struct sockaddr_in *addr;
	const char *p;
	char *ip, *end;
	long port;
	int rc;

	debug1("parse_addr(): parsing address [%p] '%s'\n", spec, spec);
	addr = malloc(sizeof(struct sockaddr_in));
	if (!addr) {
		perror("malloc in parse_addr");
		exit(4);
	}

	if (!spec || !*spec) {
		fprintf(stderr, "address '%s' is invalid\n", spec);
		exit(1);
	}

	p = strchr(spec, ':');
	if (!p) {                    /* i.e. 'host' */
		debug1("parse_addr(): address [%p] '%s' is in 'host' format\n", spec, spec);
		ip = strdup(spec);
		port = DEFAULT_PORT;
		debug1("parse_addr(): using given host '%s' and default port %s\n", ip, port);

	} else {
		if (p == spec) {         /* i.e. ':port' */
			debug1("address [%p] '%s' is in ':port' format\n", spec, spec);
			debug1("parse_addr(): using default host '127.0.0.1' and given port '%s'\n", p+1);
			ip = NULL;
		} else {                 /* i.e. 'host:port' */
			debug1("address [%p] '%s' is in 'host:port' format\n", spec, spec);
			ip = strndup(spec, p-spec);
			debug1("parse_addr(): using given host '%s' and given port '%s'\n", ip, p+1);
		}
		port = strtol(p+1, &end, 10);
		if (end && *end) {
			fprintf(stderr, "address '%s' contains a bad (non-numeric) port\n", spec);
			exit(1);
		}
		if (port < 1 || port > 65535) {
			fprintf(stderr, "address '%s' specifies a bad (out-of-range) port\n", spec);
			exit(1);
		}
	}

	/* we only support ipv4 at the moment */
	addr->sin_family = AF_INET;
	addr->sin_port = htons((short)port);
	if (!ip) ip = strdup("127.0.0.1");
	if (strcmp(ip, "*") != 0) {
		rc = inet_pton(addr->sin_family, ip, &addr->sin_addr);
		debug2("parse_addr(): inet_pton(host)", rc, rc == 1);
		if (rc == 0) {
			fprintf(stderr, "address '%s' contains an invalid IPv4 address '%s'\n", spec, ip);
			exit(1);
		}
		if (rc < 0) {
			fprintf(stderr, "this host does not support IPv4.  strange.\n");
			exit(1);
		}
	} else {
		debug1("parse_addr(): given wildcard host '%s' - binding INADDR_ANY\n");
		addr->sin_addr.s_addr = INADDR_ANY;
	}

	return addr;
}

static inline unsigned long earliest(unsigned long cur)
{
	return cur < RING_SIZE ? 0
	                       : cur - (RING_SIZE - 1);
}

#define MSG(n) (ring[(n) % RING_SIZE])
int main(int argc, char **argv)
{
	static struct buf    input;                /* for buffered stdin reads */
	static struct msg    ring[RING_SIZE];      /* the ring buffer */
	static struct client clients[MAX_CLIENTS]; /* connected clients */
	unsigned long current = 0;                 /* current message number */

	int epfd;
	struct epoll_event ev, events[MAX_EVENTS]; /* epoll event interface */

	int listenfd, acceptfd;
	struct sockaddr_in *local, peer;
	size_t len, nwrit;
	int i, j, rc, n, nfd, eof;
	char *env;

	if ((env = getenv(DEBUG_ENVVAR))) {
		if (*env) {
			verbose = 1;
		}
	}
	if (argc > 2 || (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0))) {
		fprintf(stderr, "usage: ./foo | rlog [127.0.0.1:1040]\n\n");
		fprintf(stderr, "to connect to rlog, try nc:\n\n");
		fprintf(stderr, "  nc 127.0.0.1 1040\n\n");
		return 0;
	}

	memset(&input,  0, sizeof(input));
	memset(ring,    0, sizeof(ring));
	memset(clients, 0, sizeof(clients));
	for (i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = NOT_A_CLIENT;
	}

	memset(&ev,    0, sizeof(ev));
	memset(events, 1, sizeof(events));

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0) {
		perror("socket");
		exit(4);
	}

	i = 1;
	rc = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));
	if (rc != 0) {
		perror("setsockopt(SO_REUSEADDR)");
		exit(4);
	}

	local = parse_addr(argc == 2 ? argv[1] : DEFAULT_BIND);
	rc = bind(listenfd, (struct sockaddr*)local, sizeof(*local));
	if (rc != 0) {
		perror("bind");
		exit(4);
	}

	rc = listen(listenfd, BACKLOG);
	if (rc != 0) {
		perror("listen");
		exit(4);
	}

	epfd = epoll_create(1); /* size arg must be >0, but is ignored.  go figure. */
	if (epfd < 0) {
		perror("epoll_create");
		exit(4);
	}

	ev.events = EPOLLIN;
	ev.data.fd = listenfd;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);
	if (rc == -1) {
		perror("epoll_ctl: listenfd");
		exit(4);
	}

	noblocking(0);
	ev.events = EPOLLIN;
	ev.data.fd = 0;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);
	if (rc == -1) {
		perror("epoll_ctl: stdin");
		exit(4);
	}

	for (n = 2, eof = 0; n > 0;) {
		nfd = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (nfd == -1) {
			perror("epoll_wait");
			exit(4);
		}

		for (i = 0; i < nfd; i++) {
			if (events[i].data.fd == listenfd) {
				/* new connection! */
				len = sizeof(peer);
				acceptfd = accept(listenfd, (struct sockaddr*)&peer, (socklen_t*)&len);
				if (acceptfd < 0) {
					fprintf(stderr, "failed accept(): %s (errno %d)\n",
							strerror(errno), errno);
					continue;
				}
				if (len != sizeof(peer)) {
					fprintf(stderr, "a very strange error occurred during accept; len %li != sizeof(peero( %li\n",
							len, sizeof(peer));
					fprintf(stderr, "closing this connection, since I don't know how to handle it...\n");
					close(acceptfd);
					continue;
				}

				/* find a client slot */
				for (j = 0; j < MAX_CLIENTS; j++) {
					if (clients[j].fd == NOT_A_CLIENT) {
						clients[j].fd = acceptfd;
						clients[j].offset = 0;
						clients[j].msgno = earliest(current);
						break;
					}
				}
				if (j == MAX_CLIENTS) {
					fprintf(stderr, "max connections reached.  closing new connection.\n");
					close(acceptfd);
					continue;
				}

				/* register the client with epoll */
				ev.events = EPOLLOUT|EPOLLHUP;
				ev.data.fd = clients[j].fd;
				rc = epoll_ctl(epfd, EPOLL_CTL_ADD, clients[j].fd, &ev);
				if (rc == -1) {
					fprintf(stderr, "failed to add client connection %d to our epoll set: %s (error %d); closing connection.\n",
							clients[j].fd, strerror(errno), errno);
					close(acceptfd);
					clients[j].fd = NOT_A_CLIENT;
					continue;
				}

				fprintf(stderr, "new client connected on fd %d\n", clients[j].fd);
				n++;
				continue;
			}

			if (events[i].data.fd == 0) {
				/* input on stdin */
				rc = buf_fill(&input, events[i].data.fd);
				if (rc == 0) {
					fprintf(stderr, "stdin closed; sending remaining outstanding messages to connected clients and then shutting down\n");
					ev.events = EPOLLIN;
					ev.data.fd = 0;
					rc = epoll_ctl(epfd, EPOLL_CTL_DEL, 0, &ev);
					if (rc != 0) {
						perror("epoll_ctl() while de-registering stdin after EOF");
						exit(4);
					}
					close(0);
					eof = 1;
					n--;

					ev.events = EPOLLIN;
					ev.data.fd = listenfd;
					rc = epoll_ctl(epfd, EPOLL_CTL_DEL, listenfd, &ev);
					if (rc != 0) {
						perror("epoll_ctl() while de-registering listenfd after EOF");
						exit(4);
					}
					close(listenfd);
					n--;
				}
				if (rc < 0) {
					fprintf(stderr, "failed reading from stdin: %s (errno %d)\n",
							strerror(errno), errno);
					exit(3);
				}

				while (buf_hasline(&input)) {
					buf_moveto(&ring[current % RING_SIZE], &input);
					debug1("main(): added new %i-byte message[%i] to ring buffer [%p]\n",
							ring[current % RING_SIZE].len, current, ring);
					debug3("main(): message", ring[current % RING_SIZE].data, (int)ring[current % RING_SIZE].len);
					current++;
				}
				continue;
			}

			for (j = 0; j < MAX_CLIENTS; j++) {
				if (clients[j].fd == NOT_A_CLIENT)      continue;
				if (clients[j].fd != events[i].data.fd) continue;

				if (events[i].events & EPOLLHUP) {
					disconnect(&clients[j], epfd);
				}

				for (;;) {
					if (current > RING_SIZE && clients[j].msgno < (current - RING_SIZE)) {
						/* slow subscriber;  try to be nice. */
						if (clients[j].offset != 0) {
							nwrit = send_to(&clients[j], epfd, "...\n", 4);
							if (nwrit <= 0) n--;
							if (nwrit != 4) continue;
							clients[j].offset = 0;
							clients[j].msgno = current - RING_SIZE - 1;
						}
					}

					if (clients[j].msgno >= current) {
						if (eof) {
							disconnect(&clients[j], epfd);
							n--;
						}
						break;
					}
					nwrit = send_to(&clients[j], epfd,
					                MSG(clients[j].msgno).data,
					                MSG(clients[j].msgno).len - clients[j].offset);
					if (nwrit <= 0) {
						break;
					}
					clients[j].offset += nwrit;
					if (clients[j].offset == MSG(clients[j].msgno).len) {
						clients[j].offset = 0;
						clients[j].msgno++;
					}
				}
			}
		}
	}

	fprintf(stderr, "shutting down.\n");
	/* FIXME no cleanup being done atm */
	return 0;
}
