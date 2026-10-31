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

#define PORT 9770
#define BACKLOG 150
#define MAX_EVENTS 64
#define BUF_SIZE 1024

/*
 * 评估表达式n 失败则 perror 并 goto cleanup.
 * 成功则返回表达式的值 需要 GCC 语句表达式支持.
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

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static int set_nonblock(Socket fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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

    listen_sock = TRY(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), "socket");

    int reuse_address = 1;
    TRY(setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse_address, sizeof(reuse_address)), "setsockopt");

    struct sockaddr_in local_address;
    memset(&local_address, 0, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = htons(port);

    TRY(bind(listen_sock, (struct sockaddr*)&local_address, (int)sizeof(local_address)), "bind");
    TRY(listen(listen_sock, BACKLOG), "listen");
    TRY(set_nonblock(listen_sock), "fcntl");

    epfd = TRY(epoll_create1(0), "epoll_create1");

    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    TRY(epoll_ctl(epfd, EPOLL_CTL_ADD, listen_sock, &ev), "epoll_ctl add listen");

    printf("Server listening on port %d\n", port);
    result = EXIT_SUCCESS;

    while (1) {
        if (!running) break;

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
                while (1) {
                    struct sockaddr_in client_address;
                    socklen_t len = sizeof(client_address);
                    Socket client_sock = accept(listen_sock,
                                                (struct sockaddr*)&client_address, &len);
                    if (client_sock == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }

                    if (set_nonblock(client_sock) == -1) {
                        perror("fcntl client");
                        close(client_sock);
                        continue;
                    }

                    ev.events = EPOLLIN;
                    ev.data.fd = client_sock;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_sock, &ev) == -1) {
                        perror("epoll_ctl add client");
                        close(client_sock);
                        continue;
                    }

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_address.sin_addr, ip, sizeof(ip));
                    printf("new client %s:%d fd=%d\n", ip,
                           ntohs(client_address.sin_port), client_sock);
                }
            } else {
                char buf[BUF_SIZE];
                ssize_t r = read(fd, buf, sizeof(buf) - 1);
                if (r > 0) {
                    buf[r] = '\0';
                    printf("fd=%d recv %zd bytes: %s\n", fd, r, buf);
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