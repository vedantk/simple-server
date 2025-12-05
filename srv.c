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
#include <limits.h>

#define PORT "80"
#define MAXEVENTS 32

struct route {
	char *url;
	char *fpath;
};

static struct route *routes = NULL;
static size_t routes_count = 0;

struct mimetype {
	char *fext;
	char *mime;
};

static struct mimetype mimetypes[] = {
	{ "jpg", "image/jpeg" },
	{ "pdf", "application/pdf" },
	{ "css", "text/css" },
	{ "asc", "text/plain; charset=UTF-8" },
	{ "html", "text/html; charset=UTF-8" },
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

static int route_compare(const void *l, const void *r)
{
	return strcmp(((struct route *) l)->url,
			((struct route *) r)->url);
}

static int mimetype_compare(const void *l, const void *r)
{
	return strcmp(((struct mimetype *) l)->fext,
			((struct mimetype *) r)->fext);
}

static void parse_routes_file(const char *filename, struct route **routes_out,
		size_t *count_out)
{
	FILE *fp;
	char line[4096];
	size_t capacity = 64;
	size_t count = 0;
	struct route *routes_arr;
	char *arrow;
	char *newline;
	char *url;
	char *fpath;
	size_t url_len;

	fp = fopen(filename, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open %s: %s\n",
				filename, strerror(errno));
		abort();
	}

	routes_arr = malloc(capacity * sizeof(struct route));
	if (!routes_arr) {
		perror("malloc");
		abort();
	}

	while (fgets(line, sizeof(line), fp)) {
		newline = strchr(line, '\n');
		if (newline) {
			*newline = '\0';
		}

		if (line[0] == '\0') {
			continue;
		}

		arrow = strstr(line, " -> ");
		if (!arrow || line[0] != '/') {
			fprintf(stderr, "bad route: %s\n", line);
			continue;
		}

		url = line;
		url_len = arrow - line;

		fpath = arrow + 4;

		if (strstr(fpath, "..") || strncmp(fpath, "www/", 4) != 0) {
			fprintf(stderr, "bad route: %s\n", line);
			continue;
		}

		if (count >= capacity) {
			capacity *= 2;
			struct route *tmp = realloc(routes_arr,
					capacity * sizeof(struct route));
			if (!tmp) {
				perror("realloc");
				abort();
			}
			routes_arr = tmp;
		}

		routes_arr[count].url = strndup(url, url_len);
		routes_arr[count].fpath = strdup(fpath);

		if (!routes_arr[count].url || !routes_arr[count].fpath) {
			perror("strndup");
			abort();
		}

		fprintf(stderr, "add route: %s -> %s\n",
				routes_arr[count].url, routes_arr[count].fpath);

		count++;
	}

	fclose(fp);

	*routes_out = routes_arr;
	*count_out = count;
}

static void build_routes(void)
{
	parse_routes_file("www/routes.txt", &routes, &routes_count);

	qsort(routes, routes_count, sizeof(struct route), route_compare);

	size_t i;
	for (i = 0; i < routes_count - 1; i++) {
		if (strcmp(routes[i].url, routes[i + 1].url) == 0) {
			fprintf(stderr, "Duplicate URL detected: %s\n",
					routes[i].url);
			fprintf(stderr, "  First:  %s -> %s\n",
					routes[i].url, routes[i].fpath);
			fprintf(stderr, "  Second: %s -> %s\n",
					routes[i + 1].url, routes[i + 1].fpath);
			abort();
		}
	}
}

static int make_listener(char *port)
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

		int one = 1;
		s = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
		if (s != 0) {
			perror("setsockopt");
			abort();
		}

		s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
		if (s == 0) {
			break;
		}

		close(sfd);
	}

	if (rp == NULL) {
		fprintf(stderr, "Could not bind to any interface\n");
		abort();
	}

	freeaddrinfo(result);

	if (getuid() == 0) {
		if (setgid(100) != 0) {
			fprintf(stderr, "setgid: %s", strerror(errno));
			close(sfd);
			abort();
		}
		if (setuid(1000) != 0) {
			fprintf(stderr, "setuid: %s", strerror(errno));
			close(sfd);
			abort();
		}
	}

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

/*
 * Pack two 32-bit file descriptors into a single 64-bit integer --
 * a trick to avoid malloc/free calls in the epoll loop.
 */
static uint64_t pack_fds(int fd1, int fd2)
{
	return (((uint64_t) fd1) << 32) | ((uint64_t) fd2);
}

static void unpack_fds(uint64_t pair, int *fd1, int *fd2)
{
	*fd1 = (int) (pair >> 32);
	*fd2 = (int) (pair & ((1ULL << 32) - 1));
}

static int enqueue_chunk(int cfd, int fd)
{
	struct epoll_event event;
	event.data.u64 = pack_fds(cfd, fd);
	event.events = EPOLLOUT | EPOLLET;
	if (epoll_ctl(efd, EPOLL_CTL_MOD, cfd, &event) < 0) {
		perror("epoll_ctl");
		return -1;
	}
	return 0;
}

static void send_chunk(int cfd, int fd)
{
	off_t pos;
	ssize_t count;
	ssize_t nread;
	ssize_t nwritten;
	static char page[4096];
	struct stat sbuf;

	if (fstat(fd, &sbuf) < 0) {
		perror("send_chunk/fstat");
		goto done;
	}

	pos = lseek(fd, 0, SEEK_CUR);
	if (pos < 0) {
		perror("send_chunk/lseek/1");
		goto done;
	}

	if (pos == sbuf.st_size) {
		goto done;
	}

	while (pos < sbuf.st_size) {
		nread = read(fd, page, sizeof(page));
		if (nread < 0) {
			perror("send_chunk/read");
			goto done;
		}

		nwritten = 0;
		while (nwritten < nread) {
			count = write(cfd, page + nwritten, nread - nwritten);
			if (count < 0) {
				if (lseek(fd, pos + nwritten, SEEK_SET) < 0) {
					perror("send_chunk/lseek/2");
					goto done;
				}

				if (errno == EAGAIN) {
					if (enqueue_chunk(cfd, fd) != 0) {
						goto done;
					}
					return;
				} else if (errno == EPIPE || errno == ECONNRESET) {
					/*
					 * Client closed the connection.
					 */
					goto done;
				} else {
					perror("send_chunk/write");
					goto done;
				}
			}
			nwritten += count;
		}

		pos += nread;
	}

	/* Closing a TCP connection requires space in the outgoing sk_buff. */
	if (enqueue_chunk(cfd, fd) != 0) {
		goto done;
	}

	return;

done:
	shutdown(cfd, SHUT_RDWR);
	close(cfd);
	close(fd);
}

static void send_file(int cfd, char *fpath)
{
	int clen;
	struct stat sbuf;
	char *extension;
	struct mimetype needle, *mt = NULL;
	static char buf[512];

	int fd = open(fpath, O_RDONLY);
	if (fd < 0) {
		perror("send_file/open");
		goto fail1;
	}

	if (fstat(fd, &sbuf) < 0) {
		perror("send_file/fstat");
		goto fail0;
	}

	extension = strrchr(fpath, '.');
	if (extension) {
		needle.fext = extension + 1;
		mt = bsearch(&needle, mimetypes,
				sizeof(mimetypes) / sizeof(struct mimetype),
				sizeof(struct mimetype), mimetype_compare);
	}

	if (!mt) {
		needle.mime = "text/plain; charset=UTF-8";
		mt = &needle;
	}

	clen = snprintf(buf, sizeof(buf),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"\r\n", mt->mime, (size_t) sbuf.st_size);

	if (write(cfd, buf, clen) != clen) {
		perror("send_file/write");
		goto fail0;
	}

	if (enqueue_chunk(cfd, fd) != 0) {
		goto fail0;
	}

	return;

fail0:
	close(fd);
fail1:
	shutdown(cfd, SHUT_RDWR);
	close(cfd);
}

static void handle_client(int cfd)
{
	ssize_t count;
	struct route needle, *route;
	char *path, *end;
	static char buf[4096];

	count = read(cfd, buf, sizeof(buf));
	if (count < 0) {
		perror("read");
		goto done;
	}

	if (count < 6) {
		goto done;
	}

	path = &buf[4];
	if (*path != '/') {
		goto done;
	}

	end = memchr(path, ' ', count);
	if (!end) {
		goto done;
	}

	*end = '\0';

	needle.url = path;
	route = bsearch(&needle, routes, routes_count,
			sizeof(struct route), route_compare);
	if (route) {
		send_file(cfd, route->fpath);
		return;
	} else {
		if (write(cfd, nourl, sizeof(nourl)) != sizeof(nourl)) {
			perror("handle_client/write");
		}
	}

done:
	shutdown(cfd, SHUT_RDWR);
	close(cfd);
}

static int accept_conn()
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
		shutdown(cfd, SHUT_RDWR);
		close(cfd);
		return errno;
	}

	event.data.fd = cfd;
	event.events = EPOLLIN | EPOLLET;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &event) < 0) {
		shutdown(cfd, SHUT_RDWR);
		close(cfd);
		perror("epoll_ctl");
		return errno;
	}

	return 0;
}

static void serve()
{
	int cfd, fd;
	int i, nr_events;
	memset(&events, 0, MAXEVENTS * sizeof(struct epoll_event));

	nr_events = epoll_wait(efd, events, MAXEVENTS, -1);
	if (nr_events < 0) {
		if (errno == EINTR) {
			return;
		}
		perror("epoll_wait");
	}

	for (i = 0; i < nr_events; i++) {
		if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
			if (events[i].events & EPOLLIN) {
				close(events[i].data.fd);
			} else if (events[i].events & EPOLLOUT) {
				unpack_fds(events[i].data.u64, &cfd, &fd);
				shutdown(cfd, SHUT_RDWR);
				close(cfd);
				close(fd);
			}
		} else if (lfd == events[i].data.fd) {
			while (accept_conn() == 0);
		} else if (events[i].events & EPOLLIN) {
			handle_client(events[i].data.fd);
		} else if (events[i].events & EPOLLOUT) {
			unpack_fds(events[i].data.u64, &cfd, &fd);
			send_chunk(cfd, fd);
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

	struct sigaction ig = {0};
	ig.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &ig, NULL);

	lfd = make_listener(PORT);

	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = exit_handler;
	sigaction(SIGTERM, &action, NULL);

	if (make_nonblocking(lfd) != 0) {
		return -1;
	}

	if (listen(lfd, SOMAXCONN) < 0) {
		perror("listen");
		return -1;
	}

	efd = epoll_create(MAXEVENTS);
	if (efd < 0) {
		perror("epoll_create");
		return -1;
	}

	event.data.fd = lfd;
	event.events = EPOLLIN;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, lfd, &event) < 0) {
		close(lfd);
		perror("epoll_ctl");
		return -1;
	}

	build_routes();

	qsort(mimetypes, sizeof(mimetypes) / sizeof(struct mimetype),
			sizeof(struct mimetype), mimetype_compare);

	for (;;) {
		serve();
	}
	return -1;
}
