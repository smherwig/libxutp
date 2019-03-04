#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xutp.h>

static void
usage(int errcode)
{
    fprintf(stderr, "./prog IPADDR PORT MSGS...\n");
    exit(errcode);
}

int
main(int argc, char *argv[])
{
    int error = 0;
    int xfd = 0;
    int i = 0;
    struct sockaddr_in saddr;

    if (argc < 4)
        usage(1);

    xutp_init();


    xfd = xutp_newsock(NULL, 0);

    memset(&saddr, 0x00, sizeof(saddr));
    saddr.sin_family = AF_INET;
    if (inet_pton(AF_INET, argv[1], &saddr.sin_addr) != 1)
        usage(1);
    saddr.sin_port = htons(atoi(argv[2]));

    error = xutp_connect(xfd, (struct sockaddr *)&saddr, sizeof(saddr));
    if (error != 0) {
        fprintf(stderr, "xutp_connect returned %d\n", error);
        exit(1);
    }

    printf("******* %d\n", argc);
    for (i = 3; i < argc; i++) {
        error = xutp_send(xfd, argv[i], strlen(argv[i]));
        if (error == -1) {
            /* TODO: need a way to get the error value */
            fprintf(stderr, "xutp_send returned -1\n");
            exit(1);
        }
    }

    fprintf(stderr, "client: closing in 3 secs\n");
    sleep(3);

    error = xutp_close(xfd);
    if (error != 0) {
        fprintf(stderr, "xutp_close returned %d\n", error);
        exit(1);
    }

    fprintf(stderr, "client: fini in 3 secs\n");
    sleep(3);

    xutp_fini();

    return (0);
}
