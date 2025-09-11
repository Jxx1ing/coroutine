/*
 *  Author : WangBoJing , email : 1989wangbojing@gmail.com
 *
 *  Copyright Statement:
 *  --------------------
 *  This software is protected by Copyright and the information contained
 *  herein is confidential. The software may not be copied and the information
 *  contained herein may not be used or disclosed except with the written
 *  permission of Author. (C) 2017
 *
 *

****       *****                                      *****
  ***        *                                       **    ***
  ***        *         *                            *       **
  * **       *         *                           **        **
  * **       *         *                          **          *
  *  **      *        **                          **          *
  *  **      *       ***                          **
  *   **     *    ***********    *****    *****  **                   ****
  *   **     *        **           **      **    **                 **    **
  *    **    *        **           **      *     **                 *      **
  *    **    *        **            *      *     **                **      **
  *     **   *        **            **     *     **                *        **
  *     **   *        **             *    *      **               **        **
  *      **  *        **             **   *      **               **        **
  *      **  *        **             **   *      **               **        **
  *       ** *        **              *  *       **               **        **
  *       ** *        **              ** *        **          *   **        **
  *        ***        **               * *        **          *   **        **
  *        ***        **     *         **          *         *     **      **
  *         **        **     *         **          **       *      **      **
  *         **         **   *          *            **     *        **    **
*****        *          ****           *              *****           ****
                                       *
                                      *
                                  *****
                                  ****



 *
 */

#include <sys/eventfd.h>

#include "nty_coroutine.h"

int nty_epoller_create(void)
{
  return epoll_create(1024);
}

int nty_epoller_wait(struct timespec t)
{
  nty_schedule *sched = nty_coroutine_get_sched();
  return epoll_wait(sched->poller_fd, sched->eventlist, NTY_CO_MAX_EVENTS, t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0);
}

// 注册 eventfd，用于处理用户态的内部事件（如协程切换）
int nty_epoller_ev_register_trigger(void)
{
  nty_schedule *sched = nty_coroutine_get_sched();

  if (!sched->eventfd)
  {
    sched->eventfd = eventfd(0, EFD_NONBLOCK);
    assert(sched->eventfd != -1);
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = sched->eventfd;
  int ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, sched->eventfd, &ev);

  assert(ret != -1);
}
/*
疑问1：
nty_epoller_ev_register_trigger 函数中用 epoll_ctl 注册了 eventfd，而不是直接注册 socket_fd，
因此想知道在协程系统中，socket_fd 是在哪里被监视的，以及为什么代码中似乎“替换”了 socket_fd 的位置。

回答：
协程系统通过 epoll 同时监控 socket_fd（外部 I/O 事件）和 eventfd（用户态事件），两者并存，没有“替换”关系。

1-传统 epoll 程序中的socket_fd:
socket_fd 通常在服务器初始化时（比如接受客户端连接后）通过 epoll_ctl 注册到 epoll 实例。例如：epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &ev);
2-协程系统中的 socket_fd：
在协程系统中，socket_fd 的注册通常发生在网络模块的初始化或连接处理逻辑中，而不是 nty_epoller_ev_register_trigger 函数。
nty_coroutine.h 是一个协程库的部分，专注于调度器和 eventfd 的管理。socket_fd 的注册很可能在以下地方：
* 服务器初始化：在创建监听 socket（listen_fd）时，注册到 epoll 监控连接请求。
* 接受连接：当 accept 返回新的 socket_fd（客户端连接），协程系统会调用 epoll_ctl 注册这个 socket_fd。
* 协程库的网络模块：nty_coroutine.h 如nty_accept），负责网络相关的 fd 管理: nty_accept(epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, socket_fd, &ev);)
*/

/*疑问2：
epoll 如何同时监控 socket_fd 和 eventfd?
回答：
epoll 的统一监控：
epoll 通过 sched->poller_fd 监控所有注册的 fd，包括：
* socket_fd：处理客户端的网络 I/O（数据到达、可写）。
* eventfd：处理协程的内部事件（用户态逻辑，如任务完成）。

***nty_epoller_wait 等待所有事件：
int nty_epoller_wait(struct timespec t) {
    nty_schedule *sched = nty_coroutine_get_sched();
    return epoll_wait(sched->poller_fd, sched->eventlist, NTY_CO_MAX_EVENTS, t.tv_sec*1000.0 + t.tv_nsec/1000000.0);
}

***处理流程：
int nfds = nty_epoller_wait(timeout);
for (int i = 0; i < nfds; i++) {
    int fd = sched->eventlist[i].data.fd;
    if (fd == sched->eventfd) {
        uint64_t value;
        read(sched->eventfd, &value, sizeof(value)); // 处理用户态事件
        // 切换协程
    } else {
        // 处理 socket_fd 的 I/O 事件
        char buffer[1024];
        read(fd, buffer, sizeof(buffer)); // 读取客户端数据
        // 唤醒协程处理
    }
}
epoll 统一返回 socket_fd 和 eventfd 的事件，调度器根据 fd 类型处理
*/

/*
疑问3：为什么协程需要eventfd?
回答：
用户态事件处理：
传统 epoll：用户态事件处理无法直接处理用户态逻辑事件，需额外机制（如信号、轮询）。
协程 + epoll： 通过 write(eventfd, ...) 触发 EPOLLIN，用户态事件融入 epoll 循环。
处理方式：
传统 epoll：塞 I/O、多线程、多进程或异步回调，复杂且开销大。
协程 + epoll：协程在用户态切换，调度器根据 epoll 事件高效调度，代码简洁。
---------------------------------------------------------------------------
实际场景对比
（1）-传统 epoll 场景
1-初始化：创建 epoll 实例，注册监听 socket。

2-客户端请求：客户端发送请求，socket 可读，epoll 触发 EPOLLIN。

3-处理：
读取 socket 数据，解析请求，生成响应，写入 socket。
可能使用多线程（每个连接一个线程）或回调（异步处理）。

4-内部逻辑：
如果需要处理非 I/O 事件（如定时器、状态更新），需额外机制（如信号、轮询）。
例如，检查定时器可能需要单独的线程或循环，增加复杂性。

5-问题：
多线程开销大，高并发下资源占用高。
回调模型导致代码复杂，难以维护。
用户态逻辑（非 I/O）难以融入 epoll 循环。



（2）-协程 + epoll 场景
1-初始化：调用 nty_epoller_create 创建 epoll，nty_epoller_ev_register_trigger 注册 eventfd 和 socket fd。
客户端请求：
客户端发送请求，socket 可读，epoll 触发 EPOLLIN。
调度器唤醒协程 A 读取 socket 数据，解析请求。

2-内部逻辑：
协程 A 完成解析，调用 write(sched->eventfd, &value, sizeof(value)) 触发 epoll 事件。
epoll 检测到 eventfd 的 EPOLLIN，调度器切换到协程 B 发送响应。

3-响应：
协程 B 调用 send 写入响应，epoll 可能触发 EPOLLOUT 表示可继续写入。

4-优势：
协程切换在用户态，性能高。
eventfd 将用户态逻辑（如解析完成）融入 epoll 循环，统一管理。
代码类似同步逻辑，清晰易维护。
--------------------------------------------------------------------------
补充：
I/O 事件：我知道有变化，但无法得知具体情况：
正确部分：I/O 事件是由内核触发，通知程序某个文件描述符（fd，如 socket）状态变化（比如数据可读、可写），但事件本身只表示“有变化”（如 EPOLLIN 表示可读），不包含具体业务内容（如数据是什么、如何处理）。
补充：I/O 事件的“具体情况”需要用户态程序进一步处理（比如读取 socket 数据、解析请求）。epoll 只负责通知“有事发生”，具体业务逻辑由程序完成。
例子：客户端发送 HTTP 请求，epoll 触发 EPOLLIN（socket 可读），但具体请求内容（GET、POST 等）需要程序读取和解析。



用户态事件：涉及具体的业务：
正确部分：用户态事件通常与程序的业务逻辑相关，比如协程完成某个任务（解析请求、计算结果）、定时器到期或协程间通知。这些事件由用户态程序的逻辑触发，包含具体的业务上下文。
补充：用户态事件不直接影响内核态的 fd 状态，因此需要通过 eventfd 等机制转化为 epoll 可感知的内核态事件。
例子：协程解析完 HTTP 请求，需通知另一个协程发送响应，这是业务逻辑（用户态事件），通过写 eventfd 触发 epoll。
*/
