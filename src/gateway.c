#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9770     // 默认监听端口
#define BACKLOG 150   // listen 未完成连接队列长度
#define MAX_EVENTS 64 // epoll_wait 一次最多返回的事件数
#define BUF_SIZE 1024 // 读缓冲区大小

/*
 * TRY(n, str):
 *   先计算表达式 n，并将结果保存到同类型临时变量 __v 中（n 只求值一次）。
 *   若 __v < 0，则认为失败：调用 perror(str) 输出错误信息，并跳转到 cleanup 标签。
 *   若 __v >= 0，则整个语句表达式的值就是 __v。
 *
 * 注意：依赖 GCC/Clang 的语句表达式 ({ ... }) 和 __typeof__ 扩展。
 */
#define TRY(n, str) ({       \
    __typeof__(n) __v = (n); \
    if (__v < 0) {           \
        perror(str);         \
        goto cleanup;        \
    }                        \
    __v;                     \
})

typedef int Socket;

// 退出标志：信号处理函数置 0，主循环据此退出。
static volatile sig_atomic_t running = 1;

// 信号处理函数：只置退出标志
static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

// 把 fd 设为非阻塞。
static int set_nonblock(Socket fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 解析命令行端口参数。
 * argc == 1 用默认 PORT；argc == 2 解析 argv[1]，范围 1~65535。
 * 成功返回 0，失败打印错误返回 -1。 */
static int parse_port(int argc, char** argv, unsigned short* port) {
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return -1;
    }

    *port = PORT;
    if (argc == 2) {
        char* end = NULL;
        long parsed_port = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed_port < 1 || parsed_port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return -1;
        }
        *port = (int)parsed_port;
    }
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    // 注册 SIGINT 和 SIGTERM的处理函数
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int result = EXIT_FAILURE;
    unsigned short port = 0;
    Socket listen_sock = -1;
    int epfd = -1;

    if (parse_port(argc, argv, &port) < 0) return result;

    // 创建 监听 socket
    listen_sock = TRY(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), "socket");

    // 允许端口复用
    int reuse_address = 1;
    TRY(setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_address, sizeof(reuse_address)), "setsockopt");

    // 构造绑定地址
    struct sockaddr_in local_address;
    memset(&local_address, 0, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(port);

    // 绑定地址到监听 socket
    TRY(bind(listen_sock, (struct sockaddr*)&local_address, (int)sizeof(local_address)), "bind");

    // 开始监听
    TRY(listen(listen_sock, BACKLOG), "listen");

    // 监听 socket 设非阻塞
    TRY(set_nonblock(listen_sock), "fcntl");

    // 创建 epoll 实例，返回 epfd 作为事件登记簿句柄
    epfd = TRY(epoll_create1(0), "epoll_create1");

    // 把监听 socket 登记进 epoll，关注 EPOLLIN
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    TRY(epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev), "epoll_ctl add listen");

    printf("Server listening on port %d\n", port);
    result = EXIT_SUCCESS;

    while (1) {
        if (!running) break;

        // 阻塞等待事件
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) {
                if (!running) break;
                continue;
            }
            perror("epoll_wait");
            result = EXIT_FAILURE;
            break;
        }

        for (int i = 0; i < n; i++) {
            Socket fd = events[i].data.fd;

            if (fd == listen_sock) {
                // 监听 socket 可读：有新的连接待 accept。
                while (1) {
                    struct sockaddr_in client_address;
                    socklen_t len = sizeof(client_address);
                    Socket client_sock = accept(listen_sock, (struct sockaddr*)&client_address, &len);
                    if (client_sock == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }

                    // 新连接也设非阻塞，失败只影响当前连接
                    if (set_nonblock(client_sock) == -1) {
                        perror("fcntl client");
                        close(client_sock);
                        continue;
                    }

                    // 把新连接登记进 epoll，关注可读(客户端发数据时触发)
                    ev.events = EPOLLIN;
                    ev.data.fd = client_sock;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_sock, &ev) == -1) {
                        perror("epoll_ctl add client");
                        close(client_sock);
                        continue;
                    }
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_address.sin_addr, ip, sizeof(ip));
                    printf("new client %s:%d fd=%d\n", ip, ntohs(client_address.sin_port), client_sock);
                }
            } else {
                // 客户端连接可读
                char buf[BUF_SIZE];
                ssize_t r = read(fd, buf, sizeof(buf) - 1);
                if (r > 0) {
                    // todo: 这里没处理粘包/半包，后面接 JSON 行协议时要改
                    buf[r] = '\0';
                    printf("fd=%d recv %zd bytes: %s", fd, r, buf);
                } else if (r == 0) {
                    printf("client fd=%d closed\n", fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("read");
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                    }
                }
            }
        }
    }
    printf("Server shutting down...\n");

cleanup:
    if (epfd != -1) close(epfd);
    if (listen_sock != -1) close(listen_sock);
    return result;
}