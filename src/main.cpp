#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

int main(int argc, char *argv[])
{
    int status;
    addrinfo hints;
    addrinfo *server_info;
    char ipstr[INET6_ADDRSTRLEN];

    if (argc != 2)
    {
        fprintf(stderr, "dns");
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     // 不用管是 IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;     // 幫我填好我的 IP

    //    域名    port或http   ...       ...
    if ((status = getaddrinfo(argv[1], NULL, &hints, &server_info)) != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return 0;
    }
    printf("IP addresses for %s:\n\n", argv[1]);

    for (addrinfo *p = server_info; p; p = p->ai_next)
    {
        void *addr;
        char *ipver;

        // 取得本身位址的指標，
        // 在 IPv4 與 IPv6 中的欄位不同：
        if (p->ai_family == AF_INET)
        { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
            ipver = (char *)"IPv4";
        }
        else
        { // IPv6
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
            ipver = (char *)"IPv6";
        }

        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
        printf(" %s: %s\n", ipver, ipstr);
    }

    freeaddrinfo(server_info); // 釋放這個鏈結串列
}
