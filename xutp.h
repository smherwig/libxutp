#ifndef _XUTP_H_
#define _XUTP_H_

#include <sys/types.h>
#include <sys/socket.h>

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

void xutp_init(void);
void xutp_fini(void);

int xutp_newsock(const struct sockaddr *addr, socklen_t alen);

int xutp_listen(int fd, int backlog);
int xutp_connect(int fd, struct sockaddr *addr, socklen_t alen);
int xutp_accept(int fd, struct sockaddr *addr, socklen_t *alen);
ssize_t xutp_recv(int fd, void *buf, size_t len);
ssize_t xutp_send(int fd, const void *buf, size_t len);
int xutp_close(int fd);

int xutp_getpeername(int fd, struct sockaddr *addr, socklen_t *alen);
int xutp_getsockname(int fd, struct sockaddr *addr, socklen_t *alen);

int xutp_setnonblocking(int xfd);
int xutp_setblocking(int xfd);
int xutp_settimeout(int xfd, struct timespec *ts);

#ifdef __cplusplus
}
#endif

#endif /* ! _XUTP_H_ */
