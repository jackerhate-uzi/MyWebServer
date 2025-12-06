#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ctime>

#include "log/log.h"

class util_timer;

// 用户数据结构
struct client_data {
    sockaddr_in address;
    int sockfd;
    util_timer* timer;
};

// 定时器节点类
class util_timer {
public:
    util_timer* prev;
    util_timer* next;
    time_t expire;
    void (*cb_func)(client_data*);
    client_data* user_data;

public:
    util_timer() : prev(nullptr), next(nullptr) {}

private:
};

// 基于升序链表的定时器容器
class sort_timer_lst {
public:
    sort_timer_lst();
    sort_timer_lst(sort_timer_lst&&) = default;
    sort_timer_lst(const sort_timer_lst&) = default;
    sort_timer_lst& operator=(sort_timer_lst&&) = default;
    sort_timer_lst& operator=(const sort_timer_lst&) = default;
    ~sort_timer_lst();

    // 添加定时器
    void add_timer(util_timer* timer);
    // 调整定时器 (当某个人物发生时,比如收到了数据,要延长多少时间,需要往后移动)
    void adjust_timer(util_timer* timer);
    // 删除定时器
    void del_timer(util_timer* timer);
    // 心跳函数
    void tick();

private:
    util_timer* head;
    util_timer* tail;

    // 私有辅助函数
    void add_timer(util_timer* timer, util_timer* lst_head);
};

// 工具类
class Utils {
public:
    static int* u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;

public:
    Utils();
    Utils(Utils&&) = default;
    Utils(const Utils&) = default;
    Utils& operator=(Utils&&) = default;
    Utils& operator=(const Utils&) = default;
    ~Utils();
    void init(int timeslot);

    // 对文件描述符设置为非阻塞
    int setnonblocking(int fd);
    // 将内核事件表注册读事件,ET模式,选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMods);
    // 信号处理函数
    static void sig_handler(int sig);
    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);
    // 定时处理任务,重新定时以不断触发SIGALRM信号
    void timer_handler();
    // 输出错误信息
    void show_error(int connfd, const char* info);
};

void cb_func(client_data* user_data);
#endif  // !LST_TIMER_H
