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

#define FD_KEY(f, e) (((int64_t)(f) << (sizeof(int32_t) * 8)) | e)
#define FD_EVENT(f) ((int32_t)(f))
#define FD_ONLY(f) ((f) >> ((sizeof(int32_t) * 8)))

/*
协程可能因为需要等待一段时间（如 sleep）而暂停，这个函数帮助调度器按睡眠时间组织协程，确保最早到期的协程被优先处理。
*/
// 比较函数，用于比较两个协程 co1 和 co2 的睡眠时间（sleep_usecs），以便在红黑树中排序。
static inline int nty_coroutine_sleep_cmp(nty_coroutine *co1, nty_coroutine *co2)
{
	// 如果 co1 的睡眠时间小于 co2 的睡眠时间，返回 -1（表示 co1 排在前面）
	if (co1->sleep_usecs < co2->sleep_usecs)
	{
		return -1;
	}
	// 如果睡眠时间相等，返回 0。
	if (co1->sleep_usecs == co2->sleep_usecs)
	{
		return 0;
	}
	// 如果 co1 的睡眠时间大于 co2 的睡眠时间，返回 1（表示 co2 排在前面）。
	return 1;
}

/*协程可能因为等待 I/O 事件（如 socket 数据到达）而暂停，这个函数帮助调度器按文件描述符组织协程，以便在事件发生时快速找到对应的协程。*/
// 比较函数，用于比较两个协程的等待文件描述符（fd 或 fd_wait），以便在红黑树中排序。
static inline int nty_coroutine_wait_cmp(nty_coroutine *co1, nty_coroutine *co2)
{
#if CANCEL_FD_WAIT_UINT64
	if (co1->fd < co2->fd)
		return -1;
	else if (co1->fd == co2->fd)
		return 0;
	else
		return 1;
#else
	if (co1->fd_wait < co2->fd_wait)
	{
		return -1;
	}
	if (co1->fd_wait == co2->fd_wait)
	{
		return 0;
	}
#endif
	return 1;
}

/*
_nty_coroutine_rbtree_sleep：定义一个红黑树，用于存储睡眠中的协程。
_nty_coroutine：红黑树节点的类型（协程结构体）。
sleep_node：nty_coroutine 结构体中用于睡眠红黑树的字段（节点指针）。
nty_coroutine_sleep_cmp：比较函数，用于按 sleep_usecs 排序。
*/
/*
_nty_coroutine_rbtree_wait：定义一个红黑树，用于存储等待 I/O 事件的协程。
_nty_coroutine：红黑树节点的类型。
wait_node：nty_coroutine 结构体中用于等待红黑树的字段。
nty_coroutine_wait_cmp：比较函数，用于按 fd 或 fd_wait 排序。
*/
RB_GENERATE(_nty_coroutine_rbtree_sleep, _nty_coroutine, sleep_node, nty_coroutine_sleep_cmp);
RB_GENERATE(_nty_coroutine_rbtree_wait, _nty_coroutine, wait_node, nty_coroutine_wait_cmp);

// 将协程 co 置于睡眠状态，等待指定的毫秒数（msecs）
void nty_schedule_sched_sleepdown(nty_coroutine *co, uint64_t msecs)
{
	uint64_t usecs = msecs * 1000u;

	// 检查协程是否已经在睡眠红黑树中，如果是，则先移除。
	nty_coroutine *co_tmp = RB_FIND(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co); //  在调度器的 sleeping 红黑树中查找协程 co
	// 如果找到（co_tmp != NULL），说明协程已经在睡眠状态，使用 RB_REMOVE 将其从红黑树中移除。
	if (co_tmp != NULL)
	{
		RB_REMOVE(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co_tmp);
	}
	/*原因是
		防止协程重复添加到睡眠红黑树（可能因为之前的睡眠尚未结束）
		红黑树中不能有重复的节点，所以在添加新睡眠时间之前，必须移除旧的记录。
	*/

	/* 计算从 birth 到当前时间的差值（微秒）
		nty_coroutine_usec_now()：获取当前时间（微秒）。
		co->sched->birth：调度器启动时间（微秒）。
		nty_coroutine_diff_usecs：计算从 birth 到当前时间的差值（微秒）。
		co->sleep_usecs：将差值加上 usecs（睡眠时间），得到协程的绝对到期时间。
	*/
	// 调度器需要知道协程何时应该被唤醒，存储的是绝对时间（而非相对时间），便于比较。
	co->sleep_usecs = nty_coroutine_diff_usecs(co->sched->birth, nty_coroutine_usec_now()) + usecs;

	// 将协程插入睡眠红黑树，并标记其状态为睡眠。
	while (msecs)
	{
		// 将协程插入 sleeping 红黑树，按 sleep_usecs 排序
		co_tmp = RB_INSERT(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		// 如果插入失败（co_tmp != NULL），说明红黑树中已有相同的 sleep_usecs，增加 sleep_usecs（避免冲突），继续尝试插入。
		/*
			红黑树要求键值唯一，如果多个协程的睡眠时间相同，会导致插入失败，这里通过递增 sleep_usecs 解决冲突。
		*/
		if (co_tmp)
		{
			printf("1111 sleep_usecs %" PRIu64 "\n", co->sleep_usecs);
			co->sleep_usecs++;
			continue;
		}
		// 插入成功后，设置协程状态为 NTY_COROUTINE_STATUS_SLEEPING，然后退出循环。
		co->status |= BIT(NTY_COROUTINE_STATUS_SLEEPING);
		break;
	}

	// yield
}

// 取消协程的睡眠状态，将其标记为就绪状态。
/*
当睡眠时间到期或被外部事件唤醒时，调用此函数将协程从睡眠状态切换到就绪状态，准备执行
*/
void nty_schedule_desched_sleepdown(nty_coroutine *co)
{
	// 检查协程是否处于睡眠状态
	if (co->status & BIT(NTY_COROUTINE_STATUS_SLEEPING))
	{
		RB_REMOVE(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co); // 从睡眠红黑树中移除协程

		co->status &= CLEARBIT(NTY_COROUTINE_STATUS_SLEEPING); // 清除睡眠状态标志
		co->status |= BIT(NTY_COROUTINE_STATUS_READY);		   // 设置就绪状态标志
		co->status &= CLEARBIT(NTY_COROUTINE_STATUS_EXPIRED);  // 清除过期状态标志（如果有）
	}
}

/*
waiting 红黑树存储等待 I/O 事件的协程，按文件描述符排序，方便快速查找。
当某个文件描述符有事件发生（如 socket 可读），调度器需要找到等待该描述符的协程
*/
nty_coroutine *nty_schedule_search_wait(int fd)
{
	nty_coroutine find_it = {0};
	find_it.fd = fd;

	nty_schedule *sched = nty_coroutine_get_sched(); // 获取当前调度器实例

	// 在调度器的 waiting 红黑树中查找与 fd 匹配的协程
	nty_coroutine *co = RB_FIND(_nty_coroutine_rbtree_wait, &sched->waiting, &find_it);
	// 如果找到协程，清除其状态（co->status = 0）
	co->status = 0;

	// 返回找到的协程（或 NULL 如果未找到）
	return co;
}

/*
取消某个文件描述符的等待状态，并唤醒对应的协程
当不再需要等待某个文件描述符（例如 socket 关闭），调用此函数移除协程的等待状态并唤醒
这个函数用于清理等待状态，通常在 I/O 事件完成或被取消时使用
*/
nty_coroutine *nty_schedule_desched_wait(int fd)
{

	nty_coroutine find_it = {0};
	find_it.fd = fd;

	nty_schedule *sched = nty_coroutine_get_sched();

	nty_coroutine *co = RB_FIND(_nty_coroutine_rbtree_wait, &sched->waiting, &find_it);
	if (co != NULL)
	{
		RB_REMOVE(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);
	}
	co->status = 0;
	// 调用 nty_schedule_desched_sleepdown 取消协程的睡眠状态（如果有）
	nty_schedule_desched_sleepdown(co);

	return co;
}

// 让协程等待某个文件描述符上的事件（如可读或可写），并设置超时
void nty_schedule_sched_wait(nty_coroutine *co, int fd, unsigned short events, uint64_t timeout)
{

	// 检查协程是否已经在等待读或写事件（NTY_COROUTINE_STATUS_WAIT_READ 或 NTY_COROUTINE_STATUS_WAIT_WRITE）
	if (co->status & BIT(NTY_COROUTINE_STATUS_WAIT_READ) ||
		co->status & BIT(NTY_COROUTINE_STATUS_WAIT_WRITE))
	{
		// 如果是，打印错误信息（协程 ID、文件描述符和状态），并调用 assert(0) 终止程序（表示逻辑错误）。
		/*
			协程一次只能等待一种事件（读或写），重复等待是错误的，调度器通过状态检查避免这种情况。
			防止协程重复等待同一个或不同的事件，确保状态一致性
		*/
		printf("Unexpected event. lt id %" PRIu64 " fd %" PRId32 " already in %" PRId32 " state\n",
			   co->id, co->fd, co->status);
		assert(0);
	}

	// 如果 events 包含 POLLIN（可读事件），设置协程状态为 NTY_COROUTINE_STATUS_WAIT_READ
	if (events & POLLIN)
	{
		co->status |= NTY_COROUTINE_STATUS_WAIT_READ;
	}
	// 如果包含 POLLOUT（可写事件），设置为 NTY_COROUTINE_STATUS_WAIT_WRITE
	else if (events & POLLOUT)
	{
		co->status |= NTY_COROUTINE_STATUS_WAIT_WRITE;
	}
	else
	{
		printf("events : %d\n", events);
		assert(0);
	}

	co->fd = fd;
	co->events = events;
	// 将协程插入 waiting 红黑树（按文件描述符排序）
	nty_coroutine *co_tmp = RB_INSERT(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);
	// 断言插入成功（co_tmp == NULL），如果失败（co_tmp != NULL），表示红黑树中已有相同的文件描述符，触发错误。
	assert(co_tmp == NULL);

	// printf("timeout --> %"PRIu64"\n", timeout);
	if (timeout == 1)
		return; // Error

	// 调用 nty_schedule_sched_sleepdown 让协程睡眠指定的 timeout 时间
	nty_schedule_sched_sleepdown(co, timeout);
}

/* 从等待红黑树中移除协程，取消其等待状态
当不再需要等待某个文件描述符（例如事件已处理或协程被终止），移除其等待状态。
这个函数是清理操作，通常在协程完成 I / O 或被取消时调用。
*/
void nty_schedule_cancel_wait(nty_coroutine *co)
{
	RB_REMOVE(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);
}

// 释放调度器及其相关资源
void nty_schedule_free(nty_schedule *sched)
{
	// 如果 poller_fd 大于 0（有效文件描述符），关闭它（可能是 epoll 的文件描述符）
	if (sched->poller_fd > 0)
	{
		close(sched->poller_fd);
	}
	// 如果 eventfd 大于 0，关闭它（可能是用于事件通知的文件描述符）。
	if (sched->eventfd > 0)
	{
		close(sched->eventfd);
	}
	// 如果 stack 不为空，释放其内存（可能是协程的共享栈）
	if (sched->stack != NULL)
	{
		free(sched->stack);
	}
	// 释放调度器结构体 sched 的内存
	free(sched);
	// 使用 pthread_setspecific 清除线程局部存储中的调度器指针（global_sched_key 是一个全局键）。
	assert(pthread_setspecific(global_sched_key, NULL) == 0);
}

// 创建并初始化一个新的调度器
int nty_schedule_create(int stack_size)
{
	// 如果 stack_size 为 0，使用默认值 NTY_CO_MAX_STACKSIZE
	/*协程需要自己的栈空间来保存执行上下文，调度器会为所有协程分配一个共享栈或单独栈，具体取决于实现。*/
	int sched_stack_size = stack_size ? stack_size : NTY_CO_MAX_STACKSIZE;

	// 使用 calloc 分配一个 nty_schedule 结构体的内存，并初始化为零
	nty_schedule *sched = (nty_schedule *)calloc(1, sizeof(nty_schedule));
	if (sched == NULL)
	{
		printf("Failed to initialize scheduler\n");
		return -1;
	}

	// 使用 pthread_setspecific 将调度器指针存储到线程局部存储（TLS）中，键为 global_sched_key。
	assert(pthread_setspecific(global_sched_key, sched) == 0); // 确保当前线程可以访问调度器实例，通常用于多线程环境中。

	// epoll_create
	sched->poller_fd = nty_epoller_create();
	if (sched->poller_fd == -1)
	{
		printf("Failed to initialize epoller\n");
		nty_schedule_free(sched);
		return -2;
	}

	// 调用 nty_epoller_ev_register_trigger，可能是注册一个触发事件的文件描述符（例如 eventfd）。
	nty_epoller_ev_register_trigger();
	// 设置调度器的栈大小为 sched_stack_size。
	sched->stack_size = sched_stack_size;
	// 获取系统页面大小（getpagesize），存储到 page_size。
	sched->page_size = getpagesize();

// 如果定义了 _USE_UCONTEXT（使用 ucontext 实现协程）
#ifdef _USE_UCONTEXT
	// 使用 posix_memalign 分配对齐的栈内存（对齐到页面大小，分配 stack_size 字节）
	int ret = posix_memalign(&sched->stack, sched->page_size, sched->stack_size);
	assert(ret == 0);
#else
	sched->stack = NULL;
	bzero(&sched->ctx, sizeof(nty_cpu_ctx));
#endif
	// 初始化设置调度器的初始状态，包括时间、队列和计数器

	// spawned_coroutines = 0：初始化已创建的协程计数为 0。
	sched->spawned_coroutines = 0;
	// default_timeout = 3000000u：设置默认超时为 3000000 微秒（3 秒）。
	sched->default_timeout = 3000000u;

	// 初始化睡眠和等待红黑树。
	RB_INIT(&sched->sleeping);
	RB_INIT(&sched->waiting);

	// 记录调度器创建时间（微秒）。
	sched->birth = nty_coroutine_usec_now();

	// 初始化就绪队列（ready）和延迟队列（defer），使用双向链表（TAILQ 是 BSD 队列宏）。
	/*ready 队列存储准备运行的协程，defer 队列可能用于延迟执行的协程，busy 队列存储正在运行的协程。*/
	TAILQ_INIT(&sched->ready);
	TAILQ_INIT(&sched->defer);
	LIST_INIT(&sched->busy);
}

// 找出需要唤醒的睡眠协程（睡眠时间已到）
static nty_coroutine *nty_schedule_expired(nty_schedule *sched)
{
	// 计算当前时间与调度器创建时间的差值（t_diff_usecs）
	uint64_t t_diff_usecs = nty_coroutine_diff_usecs(sched->birth, nty_coroutine_usec_now());
	// 使用 RB_MIN 查找睡眠红黑树中睡眠时间最小的协程（最早到期）
	nty_coroutine *co = RB_MIN(_nty_coroutine_rbtree_sleep, &sched->sleeping);
	if (co == NULL)
		return NULL;
	// 如果最早到期的协程的 sleep_usecs 小于或等于当前时间差，移除该协程并返回
	if (t_diff_usecs >= co->sleep_usecs) // co->sleep_usecs 表示该协程应该睡多久（相对调度器创建时间）
										 /*
											左边 co->sleep_usecs：协程应该睡多久
											右边 t_diff_usecs：已经过了多少时间
											如果已经过的时间 ≥ 协程要睡的时间 → 协程可以醒了。
										 */
	{
		RB_REMOVE(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		return co;
	}
	return NULL;
}
/*流程：
1.调度器维护一个 睡眠协程红黑树，按协程的睡眠结束时间排序。
2.每次调度循环，调用 nty_schedule_expired 检查是否有协程到期：
	* 先算出 调度器运行了多久。
	* 找出 最早应该醒来的协程。
	* 如果它已经到期，就从树中移除并返回它。
3.返回该协程，让调度器去 resume（恢复运行）。
*/

// 检查调度器是否没有待处理的协程（即所有工作完成）
/*
检查 waiting 红黑树是否为空。
检查 busy 链表是否为空。
检查 sleeping 红黑树是否为空。
检查 ready 队列是否为空。
如果所有条件都满足，返回 true（非 0），表示调度器已完成。
*/
static inline int nty_schedule_isdone(nty_schedule *sched)
{
	return (RB_EMPTY(&sched->waiting) &&
			LIST_EMPTY(&sched->busy) &&
			RB_EMPTY(&sched->sleeping) &&
			TAILQ_EMPTY(&sched->ready));
}

// 计算调度器下一次唤醒的最短超时时间
static uint64_t nty_schedule_min_timeout(nty_schedule *sched)
{
	// 计算当前时间与调度器创建时间的差值（t_diff_usecs）
	uint64_t t_diff_usecs = nty_coroutine_diff_usecs(sched->birth, nty_coroutine_usec_now());
	// 设置默认超时为 default_timeout（3 秒）
	uint64_t min = sched->default_timeout;

	// 查找睡眠红黑树中最早到期的协程（RB_MIN）
	nty_coroutine *co = RB_MIN(_nty_coroutine_rbtree_sleep, &sched->sleeping);
	// 取最早到期的 sleep_usecs 作为 min
	if (!co)
		return min;

	// 如果没有睡眠协程，返回默认超时
	min = co->sleep_usecs;
	// 如果 min > t_diff_usecs，返回剩余时间（min - t_diff_usecs）
	if (min > t_diff_usecs)
	{
		return min - t_diff_usecs;
	}
	// 否则，返回 0（表示立即唤醒）
	return 0;
}

// 记录轮询结果，准备处理事件
static int nty_schedule_epoll(nty_schedule *sched)
{
	// 清空新事件计数（num_new_events = 0）
	sched->num_new_events = 0;
	// 创建 timespec 结构体 t（用于 epoll_wait 的超时）
	struct timespec t = {0, 0};
	// 调用 nty_schedule_min_timeout 获取最短超时时间（usecs）
	uint64_t usecs = nty_schedule_min_timeout(sched);
	// 如果 usecs 非零且就绪队列为空
	if (usecs && TAILQ_EMPTY(&sched->ready))
	{
		t.tv_sec = usecs / 1000000u;
		if (t.tv_sec != 0)
		{
			t.tv_nsec = (usecs % 1000u) * 1000u;
		}
		else
		{
			t.tv_nsec = usecs * 1000u;
		}
	}
	else
	{
		return 0;
	}

	int nready = 0;
	while (1)
	{
		// 调用 nty_epoller_wait（包装 epoll_wait），传入超时时间 t，返回就绪事件数 nready
		nready = nty_epoller_wait(t);
		if (nready == -1)
		{
			if (errno == EINTR)
				continue;
			else
				assert(0);
		}
		break;
	}

	sched->nevents = 0;
	sched->num_new_events = nready; // 设置 num_new_events 为 nready（本次轮询的就绪事件数）

	return 0;
}

// 主调度循环
void nty_schedule_run(void)
{

	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
		return;

	// 主循环条件：只要 nty_schedule_isdone 返回 false（有协程需要处理），继续运行
	while (!nty_schedule_isdone(sched))
	{

		// 1. expired --> sleep rbtree
		// 检查睡眠红黑树中的过期协程（nty_schedule_expired）
		nty_coroutine *expired = NULL;
		while ((expired = nty_schedule_expired(sched)) != NULL)
		{
			// 对每个过期协程，调用 nty_coroutine_resume 恢复其执行(resume 是协程的核心操作，恢复协程的执行状态，从上次暂停的地方继续)
			nty_coroutine_resume(expired);
		}

		// 2. ready queue
		/*
		就绪队列存储可以立即运行的协程，FDEOF 表示文件描述符已关闭（如 socket 断开），需要清理协程。
		*/
		// 获取就绪队列的最后一个协程（last_co_ready）
		nty_coroutine *last_co_ready = TAILQ_LAST(&sched->ready, _nty_coroutine_queue);
		// 遍历就绪队列（ready）
		while (!TAILQ_EMPTY(&sched->ready))
		{
			// 取出队列头部协程（TAILQ_FIRST）
			nty_coroutine *co = TAILQ_FIRST(&sched->ready);
			// 从队列中移除（TAILQ_REMOVE）
			TAILQ_REMOVE(&co->sched->ready, co, ready_next);
			// 如果协程状态包含 NTY_COROUTINE_STATUS_FDEOF（文件描述符关闭），释放协程并退出循环
			if (co->status & BIT(NTY_COROUTINE_STATUS_FDEOF))
			{
				nty_coroutine_free(co);
				break;
			}
			// 否则，恢复协程执行（nty_coroutine_resume）
			nty_coroutine_resume(co);
			// 如果当前协程是最后一个（last_co_ready），退出循环
			if (co == last_co_ready)
				break;
		}

		// 3. wait rbtree
		// 检测到事件后唤醒相关协程继续处理
		nty_schedule_epoll(sched);
		while (sched->num_new_events)
		{
			int idx = --sched->num_new_events;
			struct epoll_event *ev = sched->eventlist + idx; // 获取事件数组中的事件（sched->eventlist + idx）

			int fd = ev->data.fd;
			int is_eof = ev->events & EPOLLHUP;
			// 如果是 EPOLLHUP（连接关闭），设置 errno 为 ECONNRESET（连接重置）
			if (is_eof)
				errno = ECONNRESET;
			// 调用 nty_schedule_search_wait 查找等待该 fd 的协程
			nty_coroutine *co = nty_schedule_search_wait(fd);
			if (co != NULL)
			{
				if (is_eof)
				{
					// 如果是 EPOLLHUP，设置 NTY_COROUTINE_STATUS_FDEOF 标志
					co->status |= BIT(NTY_COROUTINE_STATUS_FDEOF);
				}
				// 恢复协程执行
				nty_coroutine_resume(co);
			}

			is_eof = 0;
		}
	}
	// 主循环结束后，释放调度器（nty_schedule_free）
	nty_schedule_free(sched);

	return;
}
