#include "conn.h"
#include <stdlib.h>

/* 连接池扩容：容量为 0 时先给 16，之后每次翻倍
 * 用 realloc 复用旧数组内容；失败时保持原数组不动，返回 -1 */
static int pool_grow(ConnPool* p) {
    size_t new_cap = p->cap ? p->cap * 2 : 16;
    Conn** items = realloc(p->items, new_cap * sizeof *items);
    if (items == NULL) return -1;
    p->items = items;
    p->cap = new_cap;
    return 0;
}

/* 创建连接对象
 * 用 calloc 保证 rx/rx_len/state 等字段初始为 0
 * 避免未初始化内存被误当成有效数据 */
Conn* conn_new(int fd) {
    Conn* c = calloc(1, sizeof *c);
    if (c == NULL) return NULL;
    c->fd = fd;
    c->state = CONN_NEW;
    c->dev_id[0] = '\0';
    return c;
}

/* 释放连接对象
 * 只释放堆内存，不 close(fd) fd 的生命周期由调用方管理
 */
void conn_free(Conn* c) {
    if (c == NULL) return;
    free(c->rx);
    free(c);
}

// 把新连接放进池池满则先扩容，扩容失败返回 -1
int conn_pool_add(ConnPool* p, Conn* c) {
    if (p->count == p->cap && pool_grow(p) < 0) return -1;
    p->items[p->count++] = c;
    return 0;
}

// 从池中删除指定连接
void conn_pool_remove(ConnPool* p, Conn* c) {
    for (size_t i = 0; i < p->count; i++) {
        if (p->items[i] == c) {
            p->items[i] = p->items[--p->count];
            return;
        }
    }
}

// 销毁连接池：逐个释放 Conn，再释放指针数组本身
void conn_pool_destroy(ConnPool* p) {
    for (size_t i = 0; i < p->count; i++) {
        conn_free(p->items[i]);
    }
    free(p->items);
    p->items = NULL;
    p->count = p->cap = 0;
}
