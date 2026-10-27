/*
 * http - minimal blocking HTTP/1.1 GET over libogc sockets, with HTTPS via
 * mbedTLS. Needs the network to be up already (netlog brings it up).
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* GET an http:// or https:// URL. Returns the HTTP status, or -1 when the
 * request failed (err says why). *body is NUL-terminated and must be freed. */
int http_get(const char *url, const char *user_agent, size_t max_body, char **body, size_t *len,
             char *err, size_t err_len);

#endif
