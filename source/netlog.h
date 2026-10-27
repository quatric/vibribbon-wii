#ifndef NETLOG_H
#define NETLOG_H

#include <stdbool.h>

#define NETLOG_PORT 5555

/* Thread-safe debug log: kept in a RAM ring, mirrored to sd:/cdplayer.log
 * when an SD card is present, and streamed to TCP clients on NETLOG_PORT. */
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void netlog_init(bool use_sd);
void netlog_shutdown(void);
const char *netlog_ip(void);          /* "" until the network is up */
bool netlog_client_connected(void);
/* copies the last n log lines (oldest first) into lines[], returns count */
int  netlog_tail(char lines[][80], int n);

#endif
