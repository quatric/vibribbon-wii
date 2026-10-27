/*
 * http - blocking HTTP/1.1 GET on libogc's net_* sockets, TLS by mbedTLS.
 *
 * Sockets stay blocking and every send/recv waits in net_poll() first, so a
 * dead server times out instead of hanging the caller. TLS verifies the chain
 * and host name against the roots in certs.h (dates aren't checked, since
 * many Wii clocks are wrong; see config/mbedtls_wii.h).
 */
#include <gccore.h>
#include <network.h>
#include <poll.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/platform_time.h>

#include "http.h"
#include "certs.h"

#define TIMEOUT_MS      20000
#define ERR_SEND_FAILED -0x004E
#define ERR_RECV_FAILED -0x004C

static bool tls_ready;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt cas;
static mbedtls_ssl_config conf;

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)ticks_to_millisecs(gettime());
}

/* The Wii has no exposed hardware RNG; gather timing jitter from the CPU
 * timebase while doing memory work, which varies with bus and cache state. */
static int wii_entropy(void *data, unsigned char *out, size_t len, size_t *olen)
{
	static volatile u32 scratch[256];
	size_t i;
	int k, n;

	for (i = 0; i < len; i++) {
		u32 acc = 0;
		for (k = 0; k < 16; k++) {
			u64 t0 = gettime(), t1;
			for (n = 0; n < (int)(t0 & 31) + 8; n++)
				scratch[(t0 + n * 37) & 255] += (u32)t0 ^ n;
			t1 = gettime();
			acc = (acc << 3 | acc >> 29) ^ (u32)(t1 - t0) ^ (u32)t1;
		}
		out[i] = (unsigned char)(acc ^ acc >> 8 ^ acc >> 16 ^ acc >> 24);
	}
	*olen = len;
	return 0;
}

static int tls_init(char *err, size_t err_len)
{
	char personal[64];
	u32 device = 0;
	int ret;

	if (tls_ready)
		return 0;
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&cas);
	mbedtls_ssl_config_init(&conf);
	mbedtls_entropy_add_source(&entropy, wii_entropy, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);

	ES_GetDeviceID(&device);
	snprintf(personal, sizeof(personal), "WiiCD %u %llu %lld", device, (unsigned long long)gettime(),
	         (long long)time(NULL));
	ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)personal,
	                            strlen(personal));
	if (!ret)
		ret = mbedtls_x509_crt_parse(&cas, (const unsigned char *)CA_ROOTS_PEM, sizeof(CA_ROOTS_PEM));
	if (ret < 0) {
		snprintf(err, err_len, "TLS setup failed (-0x%04x)", -ret);
		return -1;
	}
	mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conf, &cas, NULL);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	tls_ready = true;
	return 0;
}

/* ---- sockets with timeouts ---- */

static int wait_fd(int fd, int events)
{
	struct pollsd p = { .socket = fd, .events = events, .revents = 0 };
	int ret = net_poll(&p, 1, TIMEOUT_MS);
	if (ret == 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	return ret < 0 ? ret : 0;
}

static int sock_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx, ret;
	if (wait_fd(fd, POLLOUT) < 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	ret = net_send(fd, buf, len > 16384 ? 16384 : len, 0);
	if (ret == -EAGAIN)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return ret < 0 ? ERR_SEND_FAILED : ret;
}

static int sock_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = (int)(intptr_t)ctx, ret;
	if (wait_fd(fd, POLLIN) < 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	ret = net_recv(fd, buf, len, 0);
	if (ret == -EAGAIN)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return ret < 0 ? ERR_RECV_FAILED : ret;
}

typedef struct {
	int fd;
	bool tls;
	mbedtls_ssl_context ssl;
} conn_t;

static int conn_write(conn_t *c, const void *data, size_t len)
{
	const unsigned char *p = data;
	while (len) {
		int ret = c->tls ? mbedtls_ssl_write(&c->ssl, p, len) : sock_send((void *)(intptr_t)c->fd, p, len);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret <= 0)
			return -1;
		p += ret;
		len -= ret;
	}
	return 0;
}

/* bytes read, 0 when the server closed, <0 on errors */
static int conn_read(conn_t *c, void *buf, size_t len)
{
	for (;;) {
		int ret = c->tls ? mbedtls_ssl_read(&c->ssl, buf, len) : sock_recv((void *)(intptr_t)c->fd, buf, len);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		return ret;
	}
}

static int conn_open(conn_t *c, bool tls, const char *host, int port, char *err, size_t err_len)
{
	struct hostent *he;
	struct sockaddr_in sa;
	int ret;

	memset(c, 0, sizeof(*c));
	c->fd = -1;
	c->tls = tls;
	if (tls && tls_init(err, err_len) < 0)
		return -1;

	he = net_gethostbyname(host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
		snprintf(err, err_len, "DNS lookup for %s failed", host);
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = htons(port);
	memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));

	c->fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (c->fd < 0) {
		snprintf(err, err_len, "socket failed (%d)", c->fd);
		return -1;
	}
	ret = net_connect(c->fd, (struct sockaddr *)&sa, sizeof(sa));
	if (ret < 0) {
		snprintf(err, err_len, "connect to %s:%d failed (%d)", host, port, ret);
		net_close(c->fd);
		c->fd = -1;
		return -1;
	}
	if (!tls)
		return 0;

	mbedtls_ssl_init(&c->ssl);
	if ((ret = mbedtls_ssl_setup(&c->ssl, &conf)) || (ret = mbedtls_ssl_set_hostname(&c->ssl, host))) {
		snprintf(err, err_len, "TLS setup failed (-0x%04x)", -ret);
		goto fail;
	}
	mbedtls_ssl_set_bio(&c->ssl, (void *)(intptr_t)c->fd, sock_send, sock_recv, NULL);
	while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
			snprintf(err, err_len, "%s certificate not trusted (flags %x)", host,
			         mbedtls_ssl_get_verify_result(&c->ssl));
		else
			snprintf(err, err_len, "TLS handshake with %s failed (-0x%04x)", host, -ret);
		goto fail;
	}
	return 0;

fail:
	mbedtls_ssl_free(&c->ssl);
	net_close(c->fd);
	c->fd = -1;
	return -1;
}

static void conn_close(conn_t *c)
{
	if (c->fd < 0)
		return;
	if (c->tls) {
		mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
	}
	net_close(c->fd);
	c->fd = -1;
}

/* ---- HTTP ---- */

static int parse_url(const char *url, bool *tls, char *host, size_t host_len, int *port, const char **path)
{
	const char *p, *end;

	if (!strncmp(url, "https://", 8)) {
		*tls = true; *port = 443; p = url + 8;
	} else if (!strncmp(url, "http://", 7)) {
		*tls = false; *port = 80; p = url + 7;
	} else {
		return -1;
	}
	end = p + strcspn(p, ":/");
	if (end == p || (size_t)(end - p) >= host_len)
		return -1;
	memcpy(host, p, end - p);
	host[end - p] = 0;
	if (*end == ':') {
		*port = atoi(end + 1);
		end += strcspn(end, "/");
	}
	*path = *end ? end : "/";
	return 0;
}

static int dechunk(char *body, size_t len, size_t *out_len)
{
	size_t r = 0, w = 0;

	while (r < len) {
		char *line_end = strstr(body + r, "\r\n");
		size_t size;
		if (!line_end)
			return -1;
		size = strtoul(body + r, NULL, 16);
		r = line_end - body + 2;
		if (size == 0)
			break;
		if (r + size > len)
			return -1;
		memmove(body + w, body + r, size);
		w += size;
		r += size + 2;
	}
	body[w] = 0;
	*out_len = w;
	return 0;
}

/* value of header `name` in the NUL-terminated header block, or NULL */
static const char *header(const char *head, const char *name)
{
	size_t n = strlen(name);
	const char *line;

	for (line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
		if (!strncasecmp(line + 2, name, n) && line[2 + n] == ':') {
			const char *v = line + 3 + n;
			while (*v == ' ')
				v++;
			return v;
		}
	}
	return NULL;
}

int http_get(const char *url, const char *user_agent, size_t max_body, char **body, size_t *len,
             char *err, size_t err_len)
{
	char host[128], req[512];
	const char *path, *v;
	char *buf = NULL, *sep = NULL;
	size_t cap = 16384, got = 0, want = 0, start;
	bool tls, chunked = false;
	int port, n, status = -1;
	conn_t c;

	*body = NULL;
	*len = 0;
	err[0] = 0;
	if (parse_url(url, &tls, host, sizeof(host), &port, &path) < 0) {
		snprintf(err, err_len, "bad URL");
		return -1;
	}
	if (conn_open(&c, tls, host, port, err, err_len) < 0)
		return -1;

	n = snprintf(req, sizeof(req),
	             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: application/json\r\n"
	             "Connection: close\r\n\r\n", path, host, user_agent);
	if (n >= (int)sizeof(req) || conn_write(&c, req, n) < 0) {
		snprintf(err, err_len, "send failed");
		goto done;
	}

	buf = malloc(cap + 1);
	for (;;) {
		int r;
		if (got + 4096 > cap) {
			char *grown;
			if (cap >= max_body) {
				snprintf(err, err_len, "response too large");
				goto done;
			}
			cap *= 2;
			grown = realloc(buf, cap + 1);
			if (!grown) {
				snprintf(err, err_len, "out of memory");
				goto done;
			}
			buf = grown;
		}
		r = conn_read(&c, buf + got, cap - got);
		if (r < 0) {
			if (!sep) {
				snprintf(err, err_len, r == MBEDTLS_ERR_SSL_TIMEOUT ? "timed out" : "read failed (-0x%04x)", -r);
				goto done;
			}
			break;   /* keep what we have; the length checks below decide */
		}
		if (r == 0)
			break;
		got += r;
		buf[got] = 0;
		if (!sep && (sep = strstr(buf, "\r\n\r\n"))) {
			*sep = 0;
			v = header(buf, "Content-Length");
			want = v ? strtoul(v, NULL, 10) : 0;
			v = header(buf, "Transfer-Encoding");
			chunked = v && strstr(v, "chunked");
			*sep = '\r';
		}
		/* don't wait for the server to close once the body is complete */
		if (sep && !chunked && want && got - (sep + 4 - buf) >= want)
			break;
	}
	buf[got] = 0;

	if (strncmp(buf, "HTTP/1.", 7) || !(sep = strstr(buf, "\r\n\r\n"))) {
		snprintf(err, err_len, "not an HTTP response (%u bytes)", (unsigned)got);
		goto done;
	}
	start = sep + 4 - buf;
	*len = got - start;
	if (want && !chunked && *len < want) {
		snprintf(err, err_len, "response cut off (%u of %u bytes)", (unsigned)*len, (unsigned)want);
		*len = 0;
		goto done;
	}
	n = atoi(buf + 9);
	memmove(buf, buf + start, *len + 1);
	if (chunked && dechunk(buf, *len, len) < 0) {
		snprintf(err, err_len, "chunked response cut off");
		*len = 0;
		goto done;
	}
	status = n;
	*body = buf;
	buf = NULL;

done:
	conn_close(&c);
	free(buf);
	return status;
}
