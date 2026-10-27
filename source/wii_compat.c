/*
 * libc/socket bits libcurl links against that libogc doesn't provide.
 */
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define IOS_O_NONBLOCK 0x04

/* libogc's fcntl() hands newlib's O_NONBLOCK (0x4000) straight to IOS, which
 * expects 0x04. Linked with -Wl,--wrap=fcntl. */
int __real_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...)
{
	int arg = 0, r;
	va_list ap;

	if (cmd == F_SETFL) {
		va_start(ap, cmd);
		arg = va_arg(ap, int);
		va_end(ap);
		return __real_fcntl(fd, F_SETFL, (arg & O_NONBLOCK) ? IOS_O_NONBLOCK : 0);
	}
	if (cmd == F_GETFL) {
		r = __real_fcntl(fd, F_GETFL);
		if (r < 0)
			return r;
		return O_RDWR | ((r & IOS_O_NONBLOCK) ? O_NONBLOCK : 0);
	}
	return __real_fcntl(fd, cmd, arg);
}

/* libogc's getsockopt() is a stub returning EINVAL, which makes curl treat
 * every non-blocking connect as failed. IOS can't report SO_ERROR, so claim
 * success; a refused connection still surfaces on the first send/recv.
 * Linked with -Wl,--wrap=getsockopt. */
int __real_getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int __wrap_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
	if (level == SOL_SOCKET && name == SO_ERROR && val && len && *len >= (socklen_t)sizeof(int)) {
		*(int *)val = 0;
		*len = sizeof(int);
		return 0;
	}
	return __real_getsockopt(fd, level, name, val, len);
}

/* curl uses select() with no descriptors as a millisecond sleep; anything
 * else is translated to poll(), which libogc does implement. */
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
	struct pollfd pfd[FD_SETSIZE];
	int n = 0, fd, r, timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;

	for (fd = 0; fd < nfds && n < FD_SETSIZE; fd++) {
		short ev = 0;
		if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
		if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
		if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
		if (ev) {
			pfd[n].fd = fd;
			pfd[n].events = ev;
			pfd[n].revents = 0;
			n++;
		}
	}

	if (n == 0) {
		if (timeout > 0)
			usleep(timeout * 1000);
		return 0;
	}

	r = poll(pfd, n, timeout);
	if (r <= 0)
		return r;

	if (rd) FD_ZERO(rd);
	if (wr) FD_ZERO(wr);
	if (ex) FD_ZERO(ex);
	r = 0;
	for (fd = 0; fd < n; fd++) {
		if (rd && (pfd[fd].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(pfd[fd].fd, rd); r++; }
		if (wr && (pfd[fd].revents & (POLLOUT | POLLERR)))          { FD_SET(pfd[fd].fd, wr); r++; }
		if (ex && (pfd[fd].revents & POLLPRI))                      { FD_SET(pfd[fd].fd, ex); r++; }
	}
	return r;
}

char *basename(char *path)
{
	char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}
