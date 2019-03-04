#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xutp.h>

#define MESSAGE_A "Hello, World!\n"
#define MESSAGE_B "Goodbye, Mundo!\n"

/* Creat a UTP server that simply listens for connections
 * and cats messages it receives to stdout.
 */

static void
usage(int errcode)
{
    fprintf(stderr, "./prog PORT\n");
    exit(errcode);
}

static void
handle_client(int xfd, struct sockaddr_in *addr)
{
    /* TODO */
    (void) xfd;
    (void) addr;
    //xutp_send(xfd, MESSAGE_A, strlen(MESSAGE_A));
    //xutp_send(xfd, MESSAGE_A, strlen(MESSAGE_B));
    //xutp_close(xfd);
}

int
main(int argc, char *argv[])
{
    int xfd = 0;
    int cxfd = 0;
    struct sockaddr_in saddr;
    struct sockaddr_in caddr;
    socklen_t calen;

    if (argc != 2)
        usage(1);

    xutp_init();

    memset(&saddr, 0x00, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(atoi(argv[1]));

    xfd = xutp_newsock((struct sockaddr *)&saddr, sizeof(saddr));
    xutp_listen(xfd, 5);    /* BACKLOG=5 -- not really used */

    while (1) {
        calen = sizeof(caddr);
        cxfd = xutp_accept(xfd, (struct sockaddr *)&caddr, &calen);
        fprintf(stderr, "new connection: (xfd=%d)\n", xfd);
        handle_client(cxfd, &caddr);
        //fprintf(stderr, "closing connection: (xfd=%d)\n", xfd);
    }

    xutp_fini();
    return (0);
}
