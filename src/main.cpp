#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define MYPORT "3490" // 使用者将连接的 port
#define BACKLOG 10    // 在队列中可以有多少个连接在等待
/*
domain 是 AF_INET 或 AF_INET6，
type 是 SOCK_STREAM 或 SOCK_DGRAM，
而 protocol 可以设置为 0，用来帮给予的 type 选择适当的协议。
或者你可以调用 getprotobyname() 来查询你想要的协议，＂tcp＂或＂udp＂
*/
// int socket(int domain, int type, int protocol);

/*
sockfd 是 socket() 传回的 socket file descriptor。
my_addr 是指向包含你的地址资料丶名称及 IP address 的 struct sockaddr 之指针。
addrlen 是以 byte 为单位的地址长度
*/
// int bind(int sockfd, sockaddr *my_addr, int addrlen);

// int connect(int sockfd, sockaddr *serv_addr, int addrlen);

/*
backlog 是进入的队列（incoming queue）中所允许的连接数目
*/
// int listen(int sockfd, int backlog);

/*
sockfd 是正在进行 listen() 的 socket descriptor。
很简单，addr 通常是一个指向 local struct sockaddr_storage 的指针，
关於进来的连接将往哪里去的资料［而你可以用它来得知是哪一台主机从哪一个 port 调用你的］。
addrlen 是一个 local 的整数变量，应该在将它的地址传递给 accept() 以前，将它设置为 sizeof(sockaddr_storage)
从backlog拿一个出来
*/
// int accept(int sockfd, sockaddr *addr, socklen_t *addrlen);

/*
sockfd 是你想要送资料过去的 socket descriptor［
不论它是不是 socket() 返回的，或是你用 accept() 取得的］。
msg 是一个指向你想要传送资料之指标，而 len 是以 byte 为单位的资料长度。
而 flags 设置为 0 就好
*/
// int send(int sockfd, const void *msg, int len, int flags);

/*
sockfd 是要读取的 socket descriptor，
buf 是要记录读到资料的缓冲区（buffer），
len 是缓冲区的最大长度，
而 flags 可以再设置为 0
*/
// int recv(int sockfd, void *buf, int len, int flags);

// sendto(int sockfd, const void *msg, int len, unsigned int flags, const sockaddr *to, socklen_t tolen);

// int recvfrom(int sockfd, void *buf, int len, unsigned int flags, sockaddr *from, int *fromlen);
int main()
{
    sockaddr_storage their_addr;
    socklen_t addr_size;
    addrinfo hints, *res;
    int sockfd, new_fd;

    // !! 不要忘了帮这些调用做错误检查 !!
    // 首先，使用 getaddrinfo() 载入 address struct：
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; // 使用 IPv4 或 IPv6，都可以
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // 帮我填上我的 IP

    getaddrinfo(NULL, MYPORT, &hints, &res);

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bind(sockfd, res->ai_addr, res->ai_addrlen);
    listen(sockfd, BACKLOG);

    // 现在接受一个进入的连接：
    addr_size = sizeof their_addr;
    new_fd = accept(sockfd, (sockaddr *)&their_addr, &addr_size);
}