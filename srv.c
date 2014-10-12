/*
 * srv.c
 *
 * A simple epoll-based http server
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>

#define PORT "80"
#define MAXEVENTS 64

struct route {
	char *url;
	char *fpath;
};

static struct route routes[] = {
	{ "/", "index.html" },
	{ "/about/", "about.html" }
};

static const char nourl[] =
	"HTTP/1.0 404 Not Found\r\n"
	"Content-Type: text/html; charset=UTF-8\r\n"
	"Connection: close\r\n"
	"Content-Length: 12\r\n"
	"\r\n"
	"Oh no! 404.";

static int lfd;
static int efd;
static struct epoll_event events[MAXEVENTS];

static int create_and_bind(char *port)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int s, sfd;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	s = getaddrinfo(NULL, port, &hints, &result);
	if (s != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		abort();
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd < 0) {
			continue;
		}

		s = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &s, sizeof(int));
		if (s != 0) {
			perror("setsockopt");
			abort();
		}

		s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
		if (s == 0) {
			printf("Bound to %s:%s\n", rp->ai_canonname, PORT);
			break;
		}

		close(sfd);
	}

	if (rp == NULL) {
		fprintf(stderr, "Could not bind to any interface\n");
		abort();
	}

	freeaddrinfo(result);

	return sfd;
}

static int make_nonblocking(int sfd)
{
	int flags;

	flags = fcntl(sfd, F_GETFL, 0);
	if (flags < 0) {
		perror("fcntl");
		return -1;
	}

	flags |= O_NONBLOCK;
	if (fcntl(sfd, F_SETFL, flags) < 0) {
		perror("fcntl");
		return -1;
	}

	return 0;
}

static void send_file(int cfd, char *fpath)
{
	int clen;
	off_t pos;
	static char page[4096];
	static struct stat sbuf;

	int fd = open(fpath, O_RDONLY, 0777);
	if (fd < 0) {
		return;
	}

	if (fstat(fd, &sbuf) < 0) {
		goto done;
	}

	clen = snprintf(page, sizeof(page),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html; charset=UTF-8\r\n"
		"Connection: close\r\n"
		"Content-Length: %td\r\n"
		"\r\n", sbuf.st_size);

	if (write(cfd, page, clen) != clen) {
		goto done;
	}

	for (pos = 0; pos < sbuf.st_size; pos += sizeof(page)) {
		clen = read(fd, page, sizeof(page));
		if (clen < 0) {
			goto done;
		}

		if (write(cfd, page, clen) != clen) {
			goto done;
		}
	}

done:
	close(fd);
}

static void handle_client(struct epoll_event *evt)
{
	size_t i;
	ssize_t count;
	static char buf[512];

	count = read(evt->data.fd, buf, sizeof(buf));
	if (count < 0) {
		perror("read");
		goto done;
	}

	if (count < 6) {
		goto done;
	}

	char *path = &buf[4];
	char *end = strchr(path, ' ');

	if (!end) {
		goto done;
	}

	*end = '\0';

	for (i = 0; i < sizeof(routes) / sizeof(struct route); ++i) {
		if (strcmp(routes[i].url, path) == 0) {
			send_file(evt->data.fd, routes[i].fpath);
			goto done;
		}
	}

	if (write(evt->data.fd, nourl, sizeof(nourl)) != sizeof(nourl)) {
		perror("write");
	}

done:
	if (close(evt->data.fd) != 0) {
		perror("close");
	}
}

static int accept_conn(int lfd, int efd)
{
	int cfd;
	socklen_t in_len;
	struct sockaddr in_addr;
	struct epoll_event event;

	in_len = sizeof(struct sockaddr);
	cfd = accept(lfd, &in_addr, &in_len);
	if (cfd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			perror("accept");
		}
		return errno;
	}

	if (make_nonblocking(cfd) != 0) {
		close(cfd);
		return errno;
	}

	event.data.fd = cfd;
	event.events = EPOLLIN | EPOLLET;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &event) < 0) {
		close(cfd);
		perror("epoll_ctl");
		return errno;
	}

	return 0;
}

static void serve(int lfd, int efd)
{
	int i, nr_events;
	memset(&events, 0, MAXEVENTS * sizeof(struct epoll_event));

	nr_events = epoll_wait(efd, events, MAXEVENTS, -1);

	for (i = 0; i < nr_events; i++) {
		errno = 0;
		if (events[i].events & (EPOLLERR | EPOLLHUP)) {
			perror("epoll");
			close(events[i].data.fd);
			continue;
		} else if (lfd == events[i].data.fd) {
			while (accept_conn(lfd, efd) == 0);
		} else {
			handle_client(&events[i]);
		}
	}
}

static void exit_handler(int signum)
{
	close(lfd);
	close(efd);
	exit(signum);
}

int main()
{
	struct sigaction action;
	struct epoll_event event;

	lfd = create_and_bind(PORT);

	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = exit_handler;
	sigaction(SIGTERM, &action, NULL);

	if (make_nonblocking(lfd) != 0) {
		close(lfd);
		return -1;
	}

	if (listen(lfd, SOMAXCONN) < 0) {
		perror("listen");
		return -1;
	}

	efd = epoll_create(MAXEVENTS);
	if (efd < 0) {
		perror("epoll_create");
		close(lfd);
		return -1;
	}

	event.data.fd = lfd;
	event.events = EPOLLIN;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, lfd, &event) < 0) {
		perror("epoll_ctl");
		return -1;
	}

	for (;;) {
		serve(lfd, efd);
	}
	return -1;
}
