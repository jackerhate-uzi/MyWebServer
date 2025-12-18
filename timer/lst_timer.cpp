#include "lst_timer.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <regex>
#include <type_traits>

#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst() {
    head = nullptr;
    tail = nullptr;
}

sort_timer_lst::~sort_timer_lst() {
    util_timer* temp = head;
    while (temp) {
        head = temp->next;
        delete temp;
        temp = head;
    }
}

void sort_timer_lst::add_timer(util_timer* timer) {
    if (!timer) {
        return;
    }

    if (!head) {
        head = tail = timer;
        return;
    }
    // 如果新的定时器的超时时间小于当前头部,插在头部
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }

    add_timer(timer, head);
}

void sort_timer_lst::adjust_timer(util_timer* timer) {
    if (!timer) {
        return;
    }
    util_timer* temp = timer->next;

    // 如果被调整的timer在链表尾部,或者超时时间仍然小于下一个节点,则不要移动
    if (!temp || (timer->expire < temp->expire)) {
        return;
    }

    // 如果是头部节点,将其取出,重新插入
    if (timer == head) {
        head = head->next;
        head->prev = nullptr;
        timer->next = nullptr;
        add_timer(timer, head);
    } else {
        // 既不是头部,又不是尾部,且时间变大了
        // 先取出来
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer* timer) {
    if (!timer) {
        return;
    }

    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = nullptr;
        tail = nullptr;
        return;
    }

    if (timer == head) {
        head = head->next;
        head->prev = nullptr;
        delete timer;
        return;
    }

    if (timer == tail) {
        tail = tail->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }

    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
    return;
}

void sort_timer_lst::tick() {
    if (!head) {
        return;
    }
    time_t cur = time(nullptr);
    util_timer* temp = head;

    // 便利查找所有过期的定时器
    while (temp) {
        // 如果当前节点还没有过期,后面的肯定也没有过期,直接退出
        if (cur < temp->expire) {
            break;
        }

        // 执行回调函数
        temp->cb_func(temp->user_data);
        // 删除该节点,继续检查下一个
        head = temp->next;
        if (head) {
            head->prev = nullptr;
        }
        delete temp;
        temp = head;
    }
}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head) {
    util_timer* prev = lst_head;
    util_timer* temp = prev->next;

    // 遍历找到第一个大于 timer->expire 的节点
    while (temp) {
        if (timer->expire < temp->expire) {
            prev->next = timer;
            timer->next = temp;
            temp->prev = timer;
            timer->prev = prev;
            return;
        }
        prev = temp;
        temp = temp->next;
    }

    // 如果遍历完都没找到,说明他是最大的,插在尾部
    prev->next = timer;
    timer->prev = prev;
    timer->next = nullptr;
    tail = timer;
}

void Utils::init(int timeslot) { m_TIMESLOT = timeslot; }

int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    } else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void Utils::sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, nullptr) != -1);
}

void Utils::timer_handler() {
    // 处理链表上过期的任务
    m_timer_lst.tick();
    // 重新设定闹钟
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char* info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int* Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

void cb_func(client_data* user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
