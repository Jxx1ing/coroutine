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

#ifndef __NTY_COROUTINE_H__
#define __NTY_COROUTINE_H__

#define _GNU_SOURCE
#include <dlfcn.h>

// 定义宏，指示使用ucontext.h提供的上下文切换机制（而不是汇编实现的上下文切换）
#define _USE_UCONTEXT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/tcp.h>

#ifdef _USE_UCONTEXT
#include <ucontext.h>
#endif

#include <sys/epoll.h>
#include <sys/poll.h>

#include <errno.h>

// 定义了队列（TAILQ和LIST）和红黑树（RB）数据结构，用于协程调度管理
#include "nty_queue.h"
#include "nty_tree.h"

// 定义最大事件数为1024*1024，用于epoll事件数组，限制协程库能处理的最大I/O事件数量
#define NTY_CO_MAX_EVENTS (1024 * 1024)
// 定义协程的最大栈大小为128KB（注释提到HTTP协议用16KB，TCP用4KB，可能根据场景调整）
#define NTY_CO_MAX_STACKSIZE (128 * 1024) // {http: 16*1024, tcp: 4*1024}

// 将整数x转换为位掩码（如BIT(3)生成1 << 3 = 8），用于状态标志
#define BIT(x) (1 << (x))
// 生成清除某个位的掩码（如CLEARBIT(3)生成~(1 << 3)）
#define CLEARBIT(x) ~(1 << (x))

// 用64位文件描述符等待机制（可能是为了兼容性或性能优化）
#define CANCEL_FD_WAIT_UINT64 1

// 协程函数类型定义
// 定义协程函数的类型proc_coroutine，表示协程的入口函数，接受一个void*参数，返回void。用户创建协程时需要提供这样的函数。
typedef void (*proc_coroutine)(void *);

// 协程状态枚举
// 定义协程的各种状态，用于跟踪协程生命周期和行为
typedef enum
{
	NTY_COROUTINE_STATUS_WAIT_READ, // 等待文件描述符的读/写事件
	NTY_COROUTINE_STATUS_WAIT_WRITE,
	NTY_COROUTINE_STATUS_NEW,				 // 新建的协程，尚未初始化
	NTY_COROUTINE_STATUS_READY,				 // 协程就绪，等待调度
	NTY_COROUTINE_STATUS_EXITED,			 // 协程已退出
	NTY_COROUTINE_STATUS_BUSY,				 // 协程正在运行
	NTY_COROUTINE_STATUS_SLEEPING,			 // 协程处于休眠状态
	NTY_COROUTINE_STATUS_EXPIRED,			 // 协程超时
	NTY_COROUTINE_STATUS_FDEOF,				 // 文件描述符已关闭
	NTY_COROUTINE_STATUS_DETACH,			 // 协程标记为分离，退出时自动释放
	NTY_COROUTINE_STATUS_CANCELLED,			 // 协程被取消
	NTY_COROUTINE_STATUS_PENDING_RUNCOMPUTE, // 与计算任务相关（可能是为特定场景设计的扩展状态）
	NTY_COROUTINE_STATUS_RUNCOMPUTE,
	NTY_COROUTINE_STATUS_WAIT_IO_READ, // 等待I/O读/写事件（与epoll相关）
	NTY_COROUTINE_STATUS_WAIT_IO_WRITE,
	NTY_COROUTINE_STATUS_WAIT_MULTI // 等待多个事件
} nty_coroutine_status;

// 计算任务状态和事件类型
// 定义计算任务的状态（BUSY表示正在执行，FREE表示空闲），可能用于管理协程的CPU密集型任务。
typedef enum
{
	NTY_COROUTINE_COMPUTE_BUSY,
	NTY_COROUTINE_COMPUTE_FREE
} nty_coroutine_compute_status;
// 定义I/O事件类型（READ或WRITE），用于异步I/O操作（如epoll监听的事件）。
typedef enum
{
	NTY_COROUTINE_EV_READ,
	NTY_COROUTINE_EV_WRITE
} nty_coroutine_event;

// 数据结构定义（队列和红黑树）
// LIST_HEAD 和 TAILQ_HEAD：定义双向链表（list）和尾队列（tailq），用于管理协程的busy、就绪（ready）、延迟（defer）等队列。
LIST_HEAD(_nty_coroutine_link, _nty_coroutine);
TAILQ_HEAD(_nty_coroutine_queue, _nty_coroutine);
// RB_HEAD：定义红黑树，用于管理休眠（sleeping）和等待I/O（waiting）的协程，红黑树适合高效查找和排序。
RB_HEAD(_nty_coroutine_rbtree_sleep, _nty_coroutine);
RB_HEAD(_nty_coroutine_rbtree_wait, _nty_coroutine);

// typedef语句为这些数据结构定义了别名，方便使用
typedef struct _nty_coroutine_link nty_coroutine_link;
typedef struct _nty_coroutine_queue nty_coroutine_queue;

typedef struct _nty_coroutine_rbtree_sleep nty_coroutine_rbtree_sleep;
typedef struct _nty_coroutine_rbtree_wait nty_coroutine_rbtree_wait;

// CPU上下文结构（非ucontext模式）
#ifndef _USE_UCONTEXT
typedef struct _nty_cpu_ctx
{
	void *esp; // 栈指针，指向当前栈顶
	void *ebp; // 帧指针，指向当前栈帧
	void *eip; // 指令指针，指向下一条指令
	void *edi;
	void *esi;
	void *ebx;
	void *r1;
	void *r2;
	void *r3;
	void *r4;
	void *r5;
	/*
		edi, esi, ebx, r1-r5：保存通用寄存器，适用于x86/x86_64架构。
	*/
} nty_cpu_ctx;
#endif

// 调度器结构
typedef struct _nty_schedule
{
	uint64_t birth; // 创建时间
#ifdef _USE_UCONTEXT
	ucontext_t ctx; // 调度器上下文
#else
	nty_cpu_ctx ctx; // 调度器上下文（汇编模式）
#endif
	void *stack;						// 共享栈
	size_t stack_size;					// 栈大小
	int spawned_coroutines;				// 已创建的协程数
	uint64_t default_timeout;			// 默认超时时间
	struct _nty_coroutine *curr_thread; // 当前运行的协程
	int page_size;						// 系统页面大小

	int poller_fd;									 // epoll文件描述符
	int eventfd;									 // 事件文件描述符
	struct epoll_event eventlist[NTY_CO_MAX_EVENTS]; // epoll事件数组
	int nevents;									 // 当前事件数

	int num_new_events;			 // 新事件计数
	pthread_mutex_t defer_mutex; // 延迟队列的互斥锁

	nty_coroutine_queue ready; // 就绪队列
	nty_coroutine_queue defer; // 延迟队列

	nty_coroutine_link busy; // 忙碌协程链表

	nty_coroutine_rbtree_sleep sleeping; // 休眠协程红黑树
	nty_coroutine_rbtree_wait waiting;	 // 等待I/O的协程红黑树

	// private

} nty_schedule;

// 协程结构
typedef struct _nty_coroutine
{

	// private

#ifdef _USE_UCONTEXT
	ucontext_t ctx; // 协程上下文
#else
	nty_cpu_ctx ctx; // 协程上下文（汇编模式）
#endif
	proc_coroutine func;	// 协程入口函数
	void *arg;				// 入口函数参数
	void *data;				// 附加数据
	size_t stack_size;		// 栈大小
	size_t last_stack_size; // 上次使用的栈大小

	nty_coroutine_status status; // 协程状态
	nty_schedule *sched;		 // 所属调度器

	uint64_t birth; // 创建时间
	uint64_t id;	// 协程ID
#if CANCEL_FD_WAIT_UINT64
	int fd;				   // 文件描述符
	unsigned short events; // POLL_EVENT	// 监听的事件（读/写）
#else
	int64_t fd_wait; //// 等待的文件描述符
#endif
	char funcname[64];				// 协程函数名
	struct _nty_coroutine *co_join; // 关联的协程（用于join操作）

	void **co_exit_ptr;	  // 退出指针
	void *stack;		  // 私有栈（非ucontext模式）
	void *ebp;			  // 帧指针
	uint32_t ops;		  // 操作计数（用于优先级调度）
	uint64_t sleep_usecs; // 休眠时间（微秒）

	RB_ENTRY(_nty_coroutine)
	sleep_node; // 休眠红黑树节点
	RB_ENTRY(_nty_coroutine)
	wait_node; // 等待红黑树节点

	LIST_ENTRY(_nty_coroutine)
	busy_next; // 忙碌链表节点

	TAILQ_ENTRY(_nty_coroutine)
	ready_next; // 就绪队列节点
	TAILQ_ENTRY(_nty_coroutine)
	defer_next; // 延迟队列节点
	TAILQ_ENTRY(_nty_coroutine)
	cond_next; // 条件变量队列节点

	TAILQ_ENTRY(_nty_coroutine)
	io_next; // I/O队列节点
	TAILQ_ENTRY(_nty_coroutine)
	compute_next; // 计算任务队列节点

	struct
	{
		void *buf;	   // I/O缓冲区
		size_t nbytes; // 缓冲区大小
		int fd;		   // 文件描述符
		int ret;	   // 返回值
		int err;	   // 错误码
	} io;			   // I/O操作相关数据

	struct _nty_coroutine_compute_sched *compute_sched; // 计算任务调度器
	int ready_fds;										// 就绪的文件描述符数
	struct pollfd *pfds;								// poll文件描述符数组
	nfds_t nfds;										// poll文件描述符数量
} nty_coroutine;

// 计算任务调度器结构
typedef struct _nty_coroutine_compute_sched
{
#ifdef _USE_UCONTEXT
	ucontext_t ctx;
#else
	nty_cpu_ctx ctx;
#endif
	nty_coroutine_queue coroutines; // 协程队列

	nty_coroutine *curr_coroutine; // 当前计算协程

	pthread_mutex_t run_mutex; // 运行互斥锁
	pthread_cond_t run_cond;   // 运行条件变量

	pthread_mutex_t co_mutex;
	LIST_ENTRY(_nty_coroutine_compute_sched)
	compute_next; // 计算调度器链表节点

	nty_coroutine_compute_status compute_status; // 计算状态
} nty_coroutine_compute_sched;

// 全局调度器键和工具函数
extern pthread_key_t global_sched_key;					  // 线程局部存储键，存储当前线程的调度器
static inline nty_schedule *nty_coroutine_get_sched(void) // 获取当前线程的调度器对象
{
	return pthread_getspecific(global_sched_key);
}

// 计算两个时间戳的差值（微秒）
static inline uint64_t nty_coroutine_diff_usecs(uint64_t t1, uint64_t t2)
{
	return t2 - t1;
}
// 获取当前时间（微秒），基于gettimeofday
static inline uint64_t nty_coroutine_usec_now(void)
{
	struct timeval t1 = {0, 0};
	gettimeofday(&t1, NULL);

	return t1.tv_sec * 1000000 + t1.tv_usec;
}

// 创建epoll实例
int nty_epoller_create(void);

// 取消协程的I/O事件
void nty_schedule_cancel_event(nty_coroutine *co);
// 调度I/O事件（文件描述符、事件类型、超时）
void nty_schedule_sched_event(nty_coroutine *co, int fd, nty_coroutine_event e, uint64_t timeout);
// 从休眠队列移除协程
void nty_schedule_desched_sleepdown(nty_coroutine *co);
// 调度协程休眠指定时间
void nty_schedule_sched_sleepdown(nty_coroutine *co, uint64_t msecs);
// 从等待队列移除协程
nty_coroutine *nty_schedule_desched_wait(int fd);
// 调度协程等待I/O事件
void nty_schedule_sched_wait(nty_coroutine *co, int fd, unsigned short events, uint64_t timeout);
// 运行调度器，处理就绪协程
void nty_schedule_run(void);
// 注册触发事件
int nty_epoller_ev_register_trigger(void);
// 等待epoll事件
int nty_epoller_wait(struct timespec t);
// 恢复协程执行
int nty_coroutine_resume(nty_coroutine *co);
// 释放协程资源
void nty_coroutine_free(nty_coroutine *co);
// 创建新协程
int nty_coroutine_create(nty_coroutine **new_co, proc_coroutine func, void *arg);
// 让出协程控制权
void nty_coroutine_yield(nty_coroutine *co);
// 让协程休眠
void nty_coroutine_sleep(uint64_t msecs);

/*
网络I/O函数原型
这些函数是对标准网络I/O函数（如socket, accept, recv, send等）的封装，可能通过钩子机制拦截标准调用，使其与协程的异步I/O机制集成。
*/
int nty_socket(int domain, int type, int protocol);
int nty_accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t nty_recv(int fd, void *buf, size_t len, int flags);
ssize_t nty_send(int fd, const void *buf, size_t len, int flags);
int nty_close(int fd);
int nty_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int nty_connect(int fd, struct sockaddr *name, socklen_t namelen);

ssize_t nty_sendto(int fd, const void *buf, size_t len, int flags,
				   const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t nty_recvfrom(int fd, void *buf, size_t len, int flags,
					 struct sockaddr *src_addr, socklen_t *addrlen);

// 钩子函数（COROUTINE_HOOK）
/*
启用钩子功能，允许拦截标准网络I/O调用
定义函数指针类型（如socket_t, connect_t等），指向标准库的对应函数（如socket, connect）。
extern声明全局函数指针（如socket_f, connect_f），用于存储被拦截的函数地址。
init_hook：初始化钩子，通过dlsym动态加载标准库函数。
这些钩子使协程库能将阻塞的I/O操作（如recv, send）转换为非阻塞的异步操作，与epoll集成。
*/
#define COROUTINE_HOOK

#ifdef COROUTINE_HOOK

typedef int (*socket_t)(int domain, int type, int protocol);
extern socket_t socket_f;

typedef int (*connect_t)(int, const struct sockaddr *, socklen_t);
extern connect_t connect_f;

typedef ssize_t (*read_t)(int, void *, size_t);
extern read_t read_f;

typedef ssize_t (*recv_t)(int sockfd, void *buf, size_t len, int flags);
extern recv_t recv_f;

typedef ssize_t (*recvfrom_t)(int sockfd, void *buf, size_t len, int flags,
							  struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_t recvfrom_f;

typedef ssize_t (*write_t)(int, const void *, size_t);
extern write_t write_f;

typedef ssize_t (*send_t)(int sockfd, const void *buf, size_t len, int flags);
extern send_t send_f;

typedef ssize_t (*sendto_t)(int sockfd, const void *buf, size_t len, int flags,
							const struct sockaddr *dest_addr, socklen_t addrlen);
extern sendto_t sendto_f;

typedef int (*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern accept_t accept_f;

// new-syscall
typedef int (*close_t)(int);
extern close_t close_f;

int init_hook(void);

/*

typedef int(*fcntl_t)(int __fd, int __cmd, ...);
extern fcntl_t fcntl_f;

typedef int (*getsockopt_t)(int sockfd, int level, int optname,
		void *optval, socklen_t *optlen);
extern getsockopt_t getsockopt_f;

typedef int (*setsockopt_t)(int sockfd, int level, int optname,
		const void *optval, socklen_t optlen);
extern setsockopt_t setsockopt_f;

*/

#endif

#endif

/*AL总结
nty_coroutine.h 是协程库的核心头文件，定义了：
1-数据结构：调度器（nty_schedule）、协程（nty_coroutine）、计算调度器（nty_coroutine_compute_sched），以及队列和红黑树。
2-状态和事件：协程状态（nty_coroutine_status）、计算状态（nty_coroutine_compute_status）、I/O事件类型（nty_coroutine_event）。
3-函数原型：协程管理（创建、切换、释放）、调度（事件、休眠）、网络I/O（异步封装）和钩子机制。
4-宏和工具：最大事件数、栈大小、位操作、时间计算等。

框架理解：
协程库通过ucontext或汇编实现上下文切换，支持轻量级并发。
调度器管理协程的就绪、休眠、等待I/O等状态，使用epoll处理异步I/O。
钩子机制拦截标准I/O调用，使阻塞操作适配协程的异步模型。
队列和红黑树优化协程管理和事件调度。

建议：作为初学者，重点关注nty_coroutine和nty_schedule结构，理解协程的状态转换（NEW → READY → RUNNING → EXITED）。
结合nty_coroutine.c的实现，尝试写简单例子（如创建协程并调用yield/resume）来加深理解。
*/
