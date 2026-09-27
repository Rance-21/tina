#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MYPORT "4950" // 用戶所要连线的 port
#define MAXBUFLEN 100

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int main()
{
    addrinfo hints;
    memset(&hints, 0, sizeof hints);

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo *server_info;
    int rv = getaddrinfo(NULL, MYPORT, &hints, &server_info);
    if (rv != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    int sock_fd;
    for (addrinfo *p = server_info; p; p = p->ai_next)
    {
        sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock_fd == -1)
        {
            perror("listener: socket");
            continue;
        }

        if (bind(sock_fd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sock_fd);
            perror("listener: bind");
            continue;
        }

        break;
    }

    if (server_info == NULL)
    {
        fprintf(stderr, "listener: failed to bind socket\n");
        return 2;
    }

    freeaddrinfo(server_info);
    printf("listener: waiting to recvfrom...\n");

    char buffer[MAXBUFLEN];
    sockaddr_storage their_addr;
    socklen_t addr_len = sizeof their_addr;
    int byte_cnt = recvfrom(sock_fd, buffer, MAXBUFLEN - 1, 0, (sockaddr *)&their_addr, &addr_len);

    if (byte_cnt == -1)
    {
        perror("recvfrom");
        exit(1);
    }

    char s[INET6_ADDRSTRLEN];
    inet_ntop(their_addr.ss_family, get_in_addr((sockaddr *)&their_addr), s, sizeof s);
    printf("listener: packet is %d bytes long\n", byte_cnt);

    buffer[byte_cnt] = '\0';
    printf("listener: packet contains \"%s\"\n", buffer);
}