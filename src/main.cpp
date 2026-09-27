#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#define PORT "3490" // 提供给用戶连接的 port
#define BACKLOG 10  // 有多少个特定的连接队列（pending connections queue）

void sigchld_handler(int s)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

void *get_in_addr(sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((sockaddr_in *)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int main()
{
    addrinfo hints, *server_info, *p;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rv;
    if ((rv = getaddrinfo(NULL, PORT, &hints, &server_info)) != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    int sockfd, yes = 1;
    // 以循环找出全部的结果，并绑定（bind）到第一个能用的结果
    for (p = server_info; p; p = p->ai_next)
    {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1)
        {
            perror("server: socket");
            continue;
        }

        /*
        SOL_SOCKET 表示通用套接字层选项（与具体传输协议无关）
        SO_REUSEADDR：具体的配置项名称，表示“允许重用本地地址与端口”
        */
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                       sizeof(int)) == -1)
        {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    if (p == NULL)
    {
        fprintf(stderr, "server: failed to bind\n");
        return 2;
    }

    freeaddrinfo(server_info);

    if (listen(sockfd, BACKLOG) == -1)
    {
        perror("listen");
        exit(1);
    }

    /*
    如果不管这些子进程，僵尸进程就会随连接数不断累加，最终耗尽操作系统的可用 PID 上限
    当任何一个子进程终止时，Linux 内核会自动向其父进程投递一个 SIGCHLD（子进程状态改变信号）。
    如果不做任何配置，内核对 SIGCHLD 的默认处理行为是 忽略（SIG_IGN）。
    为了让父进程感知并清理子进程，代码使用现代 POSIX 接口 sigaction 接管了该信号
    */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler; // 收拾全部死掉的 processes
    // 清空屏蔽信号集
    sigemptyset(&sa.sa_mask);

    // 设置了 SA_RESTART 标志后，操作系统内核会在信号处理函数执行完毕后，
    // 自动重启被中断的 accept() 系统调用，而不会让 accept() 抛出 EINTR 错误退出
    sa.sa_flags = SA_RESTART;
    // 向内核注册
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(1);
    }

    printf("server: waiting for connections...\n");
    sockaddr_storage their_addr;
    char s[INET6_ADDRSTRLEN];
    while (1)
    {
        socklen_t sin_size = sizeof their_addr;
        int new_fd = accept(sockfd, (sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1)
        {
            perror("accept");
            continue;
        }

        // 二进制 IP 转可读字符串
        inet_ntop(their_addr.ss_family,
                  get_in_addr((sockaddr *)&their_addr),
                  s, sizeof s);
        printf("server: got connection from %s\n", s);

        if (!fork())
        {                  // fork 返回 0，这个是 child process
            close(sockfd); // child 不需要 listener

            if (send(new_fd, "Hello, world!", 13, 0) == -1)
                perror("send");

            close(new_fd);

            exit(0);
        }
        close(new_fd); // parent 不需要这个
    }
}