#ifndef CONN_H
#define CONN_H

#include <stddef.h>

#define MAX_CONN 150 // 最大并发连接数
#define RX_INIT 1024 // 读缓冲初始容量 按需翻倍
#define RX_MAX 4096  // 单个连接读缓冲上限 超过即视为恶意灌包

typedef enum {
    CONN_NEW,    // 已 accept，尚未登录
    CONN_AUTHED, // 登录成功
    CONN_CLOSING // 准备关闭（当前版本暂未使用）
} ConnState;

/* 一个客户端连接的完整上下文 */
typedef struct {
    int fd;          // 套接字文件描述符
    char* rx;        // 读缓冲
    size_t rx_len;   // 缓冲里已有多少字节
    size_t rx_cap;   // 缓冲当前容量
    int state;       // 连接状态 ConnState
    char dev_id[64]; // 登录成功后填入的设备 ID
} Conn;

typedef struct {
    Conn** items; // 指针数组，Conn 本身地址稳定
    size_t count; // 当前连接数
    size_t cap;   // 数组容量
} ConnPool;

// 创建一个连接对象，分配并清零 fd 以外的字段。失败返回 NULL
Conn* conn_new(int fd);

// 释放连接对象及其读缓冲。不负责 close(fd)，调用方自行处理
void conn_free(Conn* c);

// 把连接加入池。成功返回 0，扩容失败返回 -1
int conn_pool_add(ConnPool* p, Conn* c);

// 从池中移除连接（swap-remove）。找不到时什么也不做
void conn_pool_remove(ConnPool* p, Conn* c);

// 释放池中所有连接并清空池，用于进程退出前的清理
void conn_pool_destroy(ConnPool* p);

#endif
