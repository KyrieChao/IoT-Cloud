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

#include "conn.h"

#define PORT 9770     // 默认监听端口
#define BACKLOG 150   // listen 未完成连接队列长度
#define MAX_EVENTS 64 // epoll_wait 一次最多返回 64 个就绪事件
#define BUF_SIZE 1024 // 单次 read 的临时缓冲区大小

/*
 * TRY(n, str):
 *   先计算表达式 n，并将结果保存到同类型临时变量 __v 中（n 只求值一次）
 *   若 __v < 0，则认为失败：调用 perror(str) 输出错误信息，并跳转到 cleanup 标签
 *   若 __v >= 0，则整个语句表达式的值就是 __v
 *
 * 注意：依赖 GCC/Clang 的语句表达式 ({ ... }) 和 __typeof__ 扩展
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

// 退出标志
static volatile sig_atomic_t running = 1;

// 信号处理函数：只置退出标志，不做任何可能阻塞或不可重入的操作
static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

// 把 fd 设为非阻塞
static int set_nonblock(Socket fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 解析命令行端口参数
 * argc == 1 用默认 PORT argc == 2 解析 argv[1]
 * 成功返回 0，失败打印错误返回 -1
 */
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

/*
 * 关闭一个连接，四件事按顺序做：
 *   1. epoll_ctl DEL   ：把 fd 从 epoll 实例中摘除
 *   2. close(fd)       ：释放文件描述符
 *   3. conn_pool_remove：把连接移出连接池
 *   4. conn_free       ：释放 Conn 及读缓冲
 * 调用后 c 指针失效，调用方不能再使用
 */
static void conn_close(Conn* c, int epfd, ConnPool* pool) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    conn_pool_remove(pool, c);
    conn_free(c);
}

/*
 * 把 data 里的 len 字节追加到连接读缓冲
 * 返回值：0 成功 -1 表示超过 RX_MAX 或内存不足，调用方应断开连接
 */
static int rx_append(Conn* c, const char* data, size_t len) {
    if (c->rx_len + len > RX_MAX) return -1;

    if (c->rx_len + len > c->rx_cap) {
        size_t new_cap = c->rx_cap ? c->rx_cap : RX_INIT;
        while (new_cap < c->rx_len + len) new_cap *= 2;
        if (new_cap > RX_MAX) new_cap = RX_MAX;

        char* p = realloc(c->rx, new_cap);
        if (p == NULL) return -1;
        c->rx = p;
        c->rx_cap = new_cap;
    }

    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
    return 0;
}

/*
 * 处理一整行
 * 以后会在这里用 cJSON 解析并做消息路由
 *
 * 注意：以后 on_line 可能关闭连接，届时 conn_process 不能继续用
 */
static void on_line(Conn* c, const char* line, size_t len) {
    printf("fd=%d line(%zu): %.*s\n", c->fd, len, (int)len, line);
}

/*
 * 从读缓冲里切出所有完整行（以 \n 结尾），逐行交给 on_line
 * 剩余不足一行的数据 memmove 到缓冲头部，等待下次 read 补齐
 * 兼容 CRLF：行尾如果是 \r\n，去掉 \r 再交给 on_line
 */
static void conn_process(Conn* c) {
    size_t start = 0;

    for (size_t i = 0; i < c->rx_len; i++) {
        if (c->rx[i] != '\n') continue;

        size_t line_len = i - start;
        if (line_len > 0 && c->rx[start + line_len - 1] == '\r') line_len--;

        on_line(c, c->rx + start, line_len);
        start = i + 1;
    }
    if (start > 0) {
        memmove(c->rx, c->rx + start, c->rx_len - start);
        c->rx_len -= start;
    }
}

// 处理已建立连接的可读事件：读数据 -> 追加缓冲 -> 切行
static void handle_conn(Conn* c, int epfd, ConnPool* pool) {
    char buf[BUF_SIZE];
    ssize_t r = read(c->fd, buf, sizeof buf);

    if (r == 0) {
        printf("client fd=%d closed\n", c->fd);
        conn_close(c, epfd, pool);
        return;
    }

    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        perror("read");
        conn_close(c, epfd, pool);
        return;
    }
    if (rx_append(c, buf, (size_t)r) < 0) {
        fprintf(stderr, "fd=%d line too long, closing\n", c->fd);
        conn_close(c, epfd, pool);
        return;
    }

    conn_process(c);
}

/*
 * 处理监听 socket 的可读事件：循环 accept 到 EAGAIN
 *
 * 为什么必须循环：
 *   epoll 水平触发下，只要监听队列非空就会一直报可读
 *   一次 accept 只取一个连接，如果不循环，剩下的连接会留到下一轮，
 *   单次事件处理效率变低 循环到 EAGAIN 一次把队列抽干最稳妥
 *
 * 每个新连接要做的四件事：
 *   1. 设非阻塞
 *   2. conn_new 分配连接对象
 *   3. 加入连接池（超过 MAX_CONN 则拒绝）
 *   4. 注册进 epoll，data.ptr 指向 Conn
 * 任何一步失败都要把已分配的资源按相反顺序释放
 */
static void do_accept(Socket listen_sock, int epfd, ConnPool* pool) {
    while (1) {
        struct sockaddr_in client_address;
        socklen_t len = sizeof(client_address);
        Socket client_sock = accept(listen_sock, (struct sockaddr*)&client_address, &len);

        if (client_sock == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        // 连接数达到上限：接受后立刻关闭，避免把进程 fd 耗尽
        if (pool->count >= MAX_CONN) {
            fprintf(stderr, "too many connections, rejecting fd=%d\n", client_sock);
            close(client_sock);
            continue;
        }

        // 新连接也设非阻塞，失败只影响当前连接，不影响已有连接
        if (set_nonblock(client_sock) == -1) {
            perror("fcntl client");
            close(client_sock);
            continue;
        }

        Conn* c = conn_new(client_sock);
        if (c == NULL) {
            perror("conn_new");
            close(client_sock);
            continue;
        }

        if (conn_pool_add(pool, c) < 0) {
            perror("conn_pool_add");
            conn_free(c);
            close(client_sock);
            continue;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_sock, &ev) == -1) {
            perror("epoll_ctl add client");
            conn_pool_remove(pool, c);
            conn_free(c);
            close(client_sock);
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_address.sin_addr, ip, sizeof(ip));
        printf("new client %s:%d fd=%d\n", ip, ntohs(client_address.sin_port), client_sock);
    }
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int result = EXIT_FAILURE;
    unsigned short port = 0;
    Socket listen_sock = -1;
    int epfd = -1;
    ConnPool pool = {0};

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

    // 开始监听 BACKLOG 是内核维护的连接队列长度
    TRY(listen(listen_sock, BACKLOG), "listen");

    // 监听 socket 设非阻塞，配合后面的 accept 循环
    TRY(set_nonblock(listen_sock), "fcntl");

    // 创建 epoll 实例
    epfd = TRY(epoll_create1(0), "epoll_create1");

    // 把监听 socket 登记进 epoll，data.ptr 用 NULL 作为哨兵，
    // 事件分发时据此区分"监听 socket"和"客户端连接"
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    TRY(epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev), "epoll_ctl add listen");

    printf("Server listening on port %d\n", port);
    result = EXIT_SUCCESS;

    // 事件循环
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            result = EXIT_FAILURE;
            break;
        }

        for (int i = 0; i < n; i++) {
            Conn* c = events[i].data.ptr;

            if (c == NULL) {
                do_accept(listen_sock, epfd, &pool);
                continue;
            }
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                conn_close(c, epfd, &pool);
                continue;
            }
            handle_conn(c, epfd, &pool);
        }
    }
    printf("Server shutting down...\n");

cleanup:
    conn_pool_destroy(&pool);
    if (epfd != -1) close(epfd);
    if (listen_sock != -1) close(listen_sock);
    return result;
}
