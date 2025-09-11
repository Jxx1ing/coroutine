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

#include "nty_coroutine.h"

// 标准poll事件和Linux epoll事件之间的转换层，使得基于poll的API能够在epoll上运行
static uint32_t nty_pollevent_2epoll(short events)
{
	uint32_t e = 0;
	if (events & POLLIN)
		e |= EPOLLIN;
	if (events & POLLOUT)
		e |= EPOLLOUT;
	if (events & POLLHUP)
		e |= EPOLLHUP;
	if (events & POLLERR)
		e |= EPOLLERR;
	if (events & POLLRDNORM)
		e |= EPOLLRDNORM;
	if (events & POLLWRNORM)
		e |= EPOLLWRNORM;
	return e;
}
static short nty_epollevent_2poll(uint32_t events)
{
	short e = 0;
	if (events & EPOLLIN)
		e |= POLLIN;
	if (events & EPOLLOUT)
		e |= POLLOUT;
	if (events & EPOLLHUP)
		e |= POLLHUP;
	if (events & EPOLLERR)
		e |= POLLERR;
	if (events & EPOLLRDNORM)
		e |= POLLRDNORM;
	if (events & EPOLLWRNORM)
		e |= POLLWRNORM;
	return e;
}
/*
 * nty_poll_inner --> 1. sockfd--> epoll, 2 yield, 3. epoll x sockfd
 * fds :
 */
/*
nty_poll_inner 就像一个 “协程版的等待”：
它不会让整个程序停下来，只会让 当前协程暂停
等待网络事件发生（比如 socket 可写，说明连接完成或者失败）
一旦事件发生，调度器会唤醒协程，继续执行 connect 检查结果

用一句话说：
它就是“等网络通道准备好”的方式，但不会阻塞整个程序。
*/
static int nty_poll_inner(struct pollfd *fds, nfds_t nfds, int timeout)
{
	// 如果 timeout 为 0，表示用户希望立即返回，不需要等待任何事件。
	if (timeout == 0)
	{
		return poll(fds, nfds, timeout);
	}
	// 如果 timeout 小于 0，表示用户希望无限期等待（阻塞）
	if (timeout < 0)
	{
		timeout = INT_MAX;
	}

	// 获取当前线程的协程调度器
	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		printf("scheduler not exit!\n");
		return -1;
	}
	// 指向当前正在运行的协程
	nty_coroutine *co = sched->curr_thread;

	// 遍历所有文件描述符，注册到 epoll 并加入等待队列
	int i = 0;
	for (i = 0; i < nfds; i++)
	{
		// 初始化 epoll_event 结构体
		struct epoll_event ev;
		ev.events = nty_pollevent_2epoll(fds[i].events);
		ev.data.fd = fds[i].fd;
		// 1. 注册到epoll：告诉系统"我对这些fd的事件感兴趣"
		epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, fds[i].fd, &ev);
		// 保存事件类型到协程
		co->events = fds[i].events;
		// 2. 登记等待：告诉调度器"我在等这些fd的事件"(将当前协程加入等待队列)
		nty_schedule_sched_wait(co, fds[i].fd, fds[i].events, timeout);
	}
	// 3. 让出CPU（开始睡觉）: 协程状态变为等待中，先切换到调度器。调度器再根据情况 resume → 协程
	nty_coroutine_yield(co);

	// 4. (当被唤醒后) 清理资源
	for (i = 0; i < nfds; i++)
	{

		struct epoll_event ev;
		ev.events = nty_pollevent_2epoll(fds[i].events);
		ev.data.fd = fds[i].fd;
		epoll_ctl(sched->poller_fd, EPOLL_CTL_DEL, fds[i].fd, &ev);
		// 清理与该文件描述符相关的等待队列
		nty_schedule_desched_wait(fds[i].fd);
	}

	return nfds;
}

int nty_socket(int domain, int type, int protocol)
{

	int fd = socket(domain, type, protocol);
	if (fd == -1)
	{
		printf("Failed to create a new socket\n");
		return -1;
	}
	int ret = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret == -1)
	{
		close(fd);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

	return fd;
}

// nty_accept
// return failed == -1, success > 0

/*
//传统方式 - 真阻塞
int sockfd = accept(listen_fd, addr, len);	线程在这里真的被操作系统挂起，CPU不能做其他事情
// 协程方式 - 假阻塞
int sockfd = nty_accept(listen_fd, addr, len);
这里看起来阻塞，但实际上CPU去运行其他协程了
*/
int nty_accept(int fd, struct sockaddr *addr, socklen_t *len)
{
	int sockfd = -1;
	int timeout = 1;
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;

	while (1)
	{
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN | POLLERR | POLLHUP;
		// 协程现在要等待这个socket上的事件，在等待期间请让其他协程运行，等事件就绪后再叫醒协程继续工作。
		/*
		为什么不能直接 accept，而是要先通过 nty_poll_inner 等待？-- 因为 accept 可能会立即失败
		当 socket 有连接到达时（POLLIN），协程会被唤醒继续执行
		*/
		nty_poll_inner(&fds, 1, timeout);

		sockfd = accept(fd, addr, len);
		if (sockfd < 0)
		{
			if (errno == EAGAIN)
			{
				continue;
			}
			// 客户端关闭了连接
			else if (errno == ECONNABORTED)
			{
				printf("accept : ECONNABORTED\n");
			}
			// 进程或系统文件描述符耗尽
			else if (errno == EMFILE || errno == ENFILE)
			{
				printf("accept : EMFILE || ENFILE\n");
			}
			return -1;
		}
		else
		{
			break;
		}
	}

	int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if (ret == -1)
	{
		close(sockfd);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

	return sockfd;
}

/*
为什么不能直接循环 connect？
直接循环 connect 的问题：
CPU 会白白空转，浪费资源
协程调度器没法让其他协程执行
在高并发场景下会严重拖慢程序

而 nty_poll_inner 让你可以：
当前协程暂停
其他协程继续工作
网络准备好时再回来继续连接

这就是协程 + 非阻塞 I/O 的核心思想。
*/
int nty_connect(int fd, struct sockaddr *name, socklen_t namelen)
{

	int ret = 0;

	while (1)
	{

		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;
		nty_poll_inner(&fds, 1, 1);

		ret = connect(fd, name, namelen);
		if (ret == 0)
			break;

		if (ret == -1 && (errno == EAGAIN ||
						  errno == EWOULDBLOCK ||
						  errno == EINPROGRESS))
		{
			continue;
		}
		else
		{
			break;
		}
	}

	return ret;
}

// recv
//  add epoll first
//
ssize_t nty_recv(int fd, void *buf, size_t len, int flags)
{

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	nty_poll_inner(&fds, 1, 1);

	int ret = recv(fd, buf, len, flags);
	if (ret < 0)
	{
		// if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET)
			return -1;
		// printf("recv error : %d, ret : %d\n", errno, ret);
	}
	return ret;
}

ssize_t nty_send(int fd, const void *buf, size_t len, int flags)
{

	int sent = 0;

	int ret = send(fd, ((char *)buf) + sent, len - sent, flags);
	if (ret == 0)
		return ret;
	if (ret > 0)
		sent += ret;

	while (sent < len)
	{
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		nty_poll_inner(&fds, 1, 1);
		ret = send(fd, ((char *)buf) + sent, len - sent, flags);
		// printf("send --> len : %d\n", ret);
		if (ret <= 0)
		{
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0)
		return ret;

	return sent;
}

ssize_t nty_sendto(int fd, const void *buf, size_t len, int flags,
				   const struct sockaddr *dest_addr, socklen_t addrlen)
{

	int sent = 0;

	while (sent < len)
	{
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		nty_poll_inner(&fds, 1, 1);
		int ret = sendto(fd, ((char *)buf) + sent, len - sent, flags, dest_addr, addrlen);
		if (ret <= 0)
		{
			if (errno == EAGAIN)
				continue;
			else if (errno == ECONNRESET)
			{
				return ret;
			}
			printf("send errno : %d, ret : %d\n", errno, ret);
			assert(0);
		}
		sent += ret;
	}
	return sent;
}

ssize_t nty_recvfrom(int fd, void *buf, size_t len, int flags,
					 struct sockaddr *src_addr, socklen_t *addrlen)
{

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	nty_poll_inner(&fds, 1, 1);

	int ret = recvfrom(fd, buf, len, flags, src_addr, addrlen);
	if (ret < 0)
	{
		if (errno == EAGAIN)
			return ret;
		if (errno == ECONNRESET)
			return 0;

		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;
}

int nty_close(int fd)
{
#if 0
	nty_schedule *sched = nty_coroutine_get_sched();

	nty_coroutine *co = sched->curr_thread;
	if (co) {
		TAILQ_INSERT_TAIL(&nty_coroutine_get_sched()->ready, co, ready_next);
		co->status |= BIT(NTY_COROUTINE_STATUS_FDEOF);
	}
#endif
	return close(fd);
}

#ifdef COROUTINE_HOOK

socket_t socket_f = NULL;

read_t read_f = NULL;
recv_t recv_f = NULL;
recvfrom_t recvfrom_f = NULL;

write_t write_f = NULL;
send_t send_f = NULL;
sendto_t sendto_f = NULL;

accept_t accept_f = NULL;
close_t close_f = NULL;
connect_t connect_f = NULL;

int init_hook(void)
{

	socket_f = (socket_t)dlsym(RTLD_NEXT, "socket");

	read_f = (read_t)dlsym(RTLD_NEXT, "read");
	recv_f = (recv_t)dlsym(RTLD_NEXT, "recv");
	recvfrom_f = (recvfrom_t)dlsym(RTLD_NEXT, "recvfrom");

	write_f = (write_t)dlsym(RTLD_NEXT, "write");
	send_f = (send_t)dlsym(RTLD_NEXT, "send");
	sendto_f = (sendto_t)dlsym(RTLD_NEXT, "sendto");

	accept_f = (accept_t)dlsym(RTLD_NEXT, "accept");
	close_f = (close_t)dlsym(RTLD_NEXT, "close");
	connect_f = (connect_t)dlsym(RTLD_NEXT, "connect");
}

int socket(int domain, int type, int protocol)
{

	if (!socket_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return socket_f(domain, type, protocol);
	}

	int fd = socket_f(domain, type, protocol);
	if (fd == -1)
	{
		printf("Failed to create a new socket\n");
		return -1;
	}
	int ret = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret == -1)
	{
		close(ret);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

	return fd;
}

ssize_t read(int fd, void *buf, size_t count)
{

	if (!read_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return read_f(fd, buf, count);
	}

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	nty_poll_inner(&fds, 1, 1);

	int ret = read_f(fd, buf, count);
	if (ret < 0)
	{
		// if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET)
			return -1;
		// printf("recv error : %d, ret : %d\n", errno, ret);
	}
	return ret;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{

	if (!recv_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return recv_f(fd, buf, len, flags);
	}

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	nty_poll_inner(&fds, 1, 1);

	int ret = recv_f(fd, buf, len, flags);
	if (ret < 0)
	{
		// if (errno == EAGAIN) return ret;
		if (errno == ECONNRESET)
			return -1;
		// printf("recv error : %d, ret : %d\n", errno, ret);
	}
	return ret;
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
				 struct sockaddr *src_addr, socklen_t *addrlen)
{

	if (!recvfrom_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return recvfrom_f(fd, buf, len, flags, src_addr, addrlen);
	}

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN | POLLERR | POLLHUP;

	nty_poll_inner(&fds, 1, 1);

	int ret = recvfrom_f(fd, buf, len, flags, src_addr, addrlen);
	if (ret < 0)
	{
		if (errno == EAGAIN)
			return ret;
		if (errno == ECONNRESET)
			return 0;

		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{

	if (!write_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return write_f(fd, buf, count);
	}

	int sent = 0;

	int ret = write_f(fd, ((char *)buf) + sent, count - sent);
	if (ret == 0)
		return ret;
	if (ret > 0)
		sent += ret;

	while (sent < count)
	{
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		nty_poll_inner(&fds, 1, 1);
		ret = write_f(fd, ((char *)buf) + sent, count - sent);
		// printf("send --> len : %d\n", ret);
		if (ret <= 0)
		{
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0)
		return ret;

	return sent;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{

	if (!send_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return send_f(fd, buf, len, flags);
	}

	int sent = 0;

	int ret = send_f(fd, ((char *)buf) + sent, len - sent, flags);
	if (ret == 0)
		return ret;
	if (ret > 0)
		sent += ret;

	while (sent < len)
	{
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;

		nty_poll_inner(&fds, 1, 1);
		ret = send_f(fd, ((char *)buf) + sent, len - sent, flags);
		// printf("send --> len : %d\n", ret);
		if (ret <= 0)
		{
			break;
		}
		sent += ret;
	}

	if (ret <= 0 && sent == 0)
		return ret;

	return sent;
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
			   const struct sockaddr *dest_addr, socklen_t addrlen)
{

	if (!sendto_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return sendto_f(sockfd, buf, len, flags, dest_addr, addrlen);
	}

	struct pollfd fds;
	fds.fd = sockfd;
	fds.events = POLLOUT | POLLERR | POLLHUP;

	nty_poll_inner(&fds, 1, 1);

	int ret = sendto_f(sockfd, buf, len, flags, dest_addr, addrlen);
	if (ret < 0)
	{
		if (errno == EAGAIN)
			return ret;
		if (errno == ECONNRESET)
			return 0;

		printf("recv error : %d, ret : %d\n", errno, ret);
		assert(0);
	}
	return ret;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{

	if (!accept_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return accept_f(fd, addr, len);
	}

	int sockfd = -1;
	int timeout = 1;
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;

	while (1)
	{
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN | POLLERR | POLLHUP;
		nty_poll_inner(&fds, 1, timeout);

		sockfd = accept_f(fd, addr, len);
		if (sockfd < 0)
		{
			if (errno == EAGAIN)
			{
				continue;
			}
			else if (errno == ECONNABORTED)
			{
				printf("accept : ECONNABORTED\n");
			}
			else if (errno == EMFILE || errno == ENFILE)
			{
				printf("accept : EMFILE || ENFILE\n");
			}
			return -1;
		}
		else
		{
			break;
		}
	}

	int ret = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if (ret == -1)
	{
		close(sockfd);
		return -1;
	}
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

	return sockfd;
}

int close(int fd)
{

	if (!close_f)
		init_hook();

	return close_f(fd);
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{

	if (!connect_f)
		init_hook();

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		return connect_f(fd, addr, addrlen);
	}

	int ret = 0;

	while (1)
	{

		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLOUT | POLLERR | POLLHUP;
		nty_poll_inner(&fds, 1, 1);

		ret = connect_f(fd, addr, addrlen);
		if (ret == 0)
			break;

		if (ret == -1 && (errno == EAGAIN ||
						  errno == EWOULDBLOCK ||
						  errno == EINPROGRESS))
		{
			continue;
		}
		else
		{
			break;
		}
	}

	return ret;
}

#endif

/*
协程的真实调度流程
1.协程 A 执行中
比如 accept() 阻塞不住，所以它调用 nty_poll_inner → yield。
在 yield 里做了两件事：
（1）-保存 A 的上下文到 A.ctx（寄存器、栈、程序计数器）。
（2）-跳转到调度器 sched.ctx。
这时 A 就“睡着了”，不会再动。

2.调度器接管 CPU
调度器 loop 在跑（epoll_wait + 管理超时队列）。
它发现某个 fd 就绪，或者某个协程超时了。
它就会调用 resume(B)，让 协程 B 恢复运行。

3.resume 某个协程（比如 B）
resume(B) 本质上就是 swapcontext(&sched->ctx, &B.ctx)。
CPU 从调度器跳进 B，继续跑 B 上次 yield 之后的指令。

4. A 什么时候继续？
当调度器发现 A 等待的事件就绪了（例如 socket 有连接到达），它才会调用 resume(A)。
这时 CPU 切回到 A.ctx，从 A 的 yield 下一行继续执行。
对应你的例子：A 在 nty_poll_inner → yield → 等 IO → 被 resume → 回到 accept() 之后。

注意：协程 不会自动 resume 回去，必须靠调度器判断条件。
流程：A -> yield -> 调度器 -> resume(B) -> B运行 -> yield -> 调度器 -> resume(A) ...
*/

/*
协程切换就是用户态程序自己通过保存/恢复寄存器和栈指针的方式“直接操控 CPU 最小单元”，绕过了操作系统调度，所以非常轻量高效。

CPU 最小执行单元是 寄存器 + 指令指针 + 栈。
协程切换本质上就是在 保存/恢复这些最小单元状态：
	保存当前协程寄存器到内存
	恢复目标协程寄存器到 CPU
	跳转到目标协程指令
所以可以理解为“用户通过汇编直接操作 CPU 的最小单元（寄存器、栈、指令指针）”，完成任务切换。
*/