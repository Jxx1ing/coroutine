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

// 定义了一个线程局部存储（TLS）的键，用于存储当前线程的调度器（scheduler）对象。TLS允许每个线程有自己的调度器实例
pthread_key_t global_sched_key;
// 使用pthread_once确保global_sched_key只被初始化一次，避免多线程竞争问题
static pthread_once_t sched_key_once = PTHREAD_ONCE_INIT;

// https://github.com/halayli/lthread/blob/master/src/lthread.c#L58

/*
代码通过宏 _USE_UCONTEXT 决定使用哪种上下文切换方式：
如果定义了 _USE_UCONTEXT，使用标准库的ucontext.h提供的上下文切换功能（跨平台，易于理解）。
如果未定义 _USE_UCONTEXT，使用自定义的汇编代码（针对x86或x86_64架构）实现上下文切换，性能更高但依赖具体硬件架构。
*/

// 第一种方式 -- 使用 ucontext 的实现
#ifdef _USE_UCONTEXT

// 保存当前协程的栈内容
static void
_save_stack(nty_coroutine *co)
{
	// 计算调度器栈的顶部地址（co->sched->stack 是调度器的栈基址，stack_size 是栈大小）
	char *top = co->sched->stack + co->sched->stack_size;
	// 在当前栈上创建一个临时变量dummy，其地址表示当前栈的“底部”（当前栈指针附近）。
	char dummy = 0;
	// 确保栈使用量不超过最大限制（NTY_CO_MAX_STACKSIZE）
	assert(top - &dummy <= NTY_CO_MAX_STACKSIZE);
	// 检查当前协程的栈空间是否不足以保存从dummy到栈顶的数据
	if (co->stack_size < top - &dummy)
	{
		// 如果栈空间不足，重新分配更大的内存
		co->stack = realloc(co->stack, top - &dummy);
		assert(co->stack != NULL);
	}
	// 更新栈大小为实际使用的栈空间
	co->stack_size = top - &dummy;
	// 将当前栈的内容复制到协程的私有栈（co->stack）中保存
	memcpy(co->stack, &dummy, co->stack_size);
}

// 恢复协程的栈内容
static void
_load_stack(nty_coroutine *co)
{
	// 将之前保存的协程栈内容（co->stack）复制回调度器的栈（co->sched->stack）的正确位置，准备恢复协程的执行
	memcpy(co->sched->stack + co->sched->stack_size - co->stack_size, co->stack, co->stack_size);
}

// 协程的执行入口，运行用户提供的协程函数
static void _exec(void *lt)
{
	// 将传入的指针转换为协程对象
	nty_coroutine *co = (nty_coroutine *)lt;
	// 调用用户指定的协程函数（如proc_coroutine类型），传入参数arg
	co->func(co->arg);
	// 协程执行结束后，设置状态标志，表示协程已退出（EXITED）、文件描述符结束（FDEOF）和分离状态（DETACH）
	co->status |= (BIT(NTY_COROUTINE_STATUS_EXITED) | BIT(NTY_COROUTINE_STATUS_FDEOF) | BIT(NTY_COROUTINE_STATUS_DETACH));
	// 让出控制权，切换回调度器
	nty_coroutine_yield(co);
}

// 第二种方式 -- 使用 ucontext 的实现

#else
/*
功能：声明上下文切换函数_switch，用于在当前协程和目标协程之间切换上下文。
解释：_switch函数通过汇编代码实现，保存当前协程的寄存器状态到cur_ctx，并恢复目标协程的寄存器状态从new_ctx。
这里列举了汇编代码（x86 和 x86_64）
 */
int _switch(nty_cpu_ctx *new_ctx, nty_cpu_ctx *cur_ctx);

#ifdef __i386__
__asm__(
	"    .text                                  \n"
	"    .p2align 2,,3                          \n"
	".globl _switch                             \n"
	"_switch:                                   \n"
	"__switch:                                  \n"
	"movl 8(%esp), %edx      # fs->%edx         \n"
	"movl %esp, 0(%edx)      # save esp         \n"
	"movl %ebp, 4(%edx)      # save ebp         \n"
	"movl (%esp), %eax       # save eip         \n"
	"movl %eax, 8(%edx)                         \n"
	"movl %ebx, 12(%edx)     # save ebx,esi,edi \n"
	"movl %esi, 16(%edx)                        \n"
	"movl %edi, 20(%edx)                        \n"
	"movl 4(%esp), %edx      # ts->%edx         \n"
	"movl 20(%edx), %edi     # restore ebx,esi,edi      \n"
	"movl 16(%edx), %esi                                \n"
	"movl 12(%edx), %ebx                                \n"
	"movl 0(%edx), %esp      # restore esp              \n"
	"movl 4(%edx), %ebp      # restore ebp              \n"
	"movl 8(%edx), %eax      # restore eip              \n"
	"movl %eax, (%esp)                                  \n"
	"ret                                                \n");
#elif defined(__x86_64__)

__asm__(
	"    .text                                  \n"
	"       .p2align 4,,15                                   \n"
	".globl _switch                                          \n"
	".globl __switch                                         \n"
	"_switch:                                                \n"
	"__switch:                                               \n"
	"       movq %rsp, 0(%rsi)      # save stack_pointer     \n"
	"       movq %rbp, 8(%rsi)      # save frame_pointer     \n"
	"       movq (%rsp), %rax       # save insn_pointer      \n"
	"       movq %rax, 16(%rsi)                              \n"
	"       movq %rbx, 24(%rsi)     # save rbx,r12-r15       \n"
	"       movq %r12, 32(%rsi)                              \n"
	"       movq %r13, 40(%rsi)                              \n"
	"       movq %r14, 48(%rsi)                              \n"
	"       movq %r15, 56(%rsi)                              \n"
	"       movq 56(%rdi), %r15                              \n"
	"       movq 48(%rdi), %r14                              \n"
	"       movq 40(%rdi), %r13     # restore rbx,r12-r15    \n"
	"       movq 32(%rdi), %r12                              \n"
	"       movq 24(%rdi), %rbx                              \n"
	"       movq 8(%rdi), %rbp      # restore frame_pointer  \n"
	"       movq 0(%rdi), %rsp      # restore stack_pointer  \n"
	"       movq 16(%rdi), %rax     # restore insn_pointer   \n"
	"       movq %rax, (%rsp)                                \n"
	"       ret                                              \n");
#endif

/*
功能：汇编实现的上下文切换，保存和恢复CPU寄存器。
解释（以x86为例）：
movl 8(%esp), %edx：获取参数cur_ctx（当前上下文）的指针。
movl %esp, 0(%edx)：保存当前栈指针（esp）到cur_ctx。
movl %ebp, 4(%edx)：保存帧指针（ebp）。
movl (%esp), %eax：保存指令指针（eip）。
movl %eax, 8(%edx)：将eip保存到cur_ctx。
保存其他寄存器（ebx, esi, edi）。
movl 4(%esp), %edx：获取目标上下文new_ctx的指针。
恢复目标上下文的寄存器（edi, esi, ebx, esp, ebp, eip）。
ret：跳转到目标上下文的指令指针。



x86_64的汇编代码类似，但使用了64位寄存器（如rsp, rbp, rbx, r12-r15）。
*/

/*
功能：与ucontext版本的_exec类似，但适配汇编实现的上下文切换。
解释：
对于特定环境（__lvm__ 和 x86_64），通过汇编从栈帧中获取lt参数。
其余逻辑与ucontext版本相同，调用协程函数并在结束时切换上下文。
*/
static void _exec(void *lt)
{
#if defined(__lvm__) && defined(__x86_64__)
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r"(lt));
#endif

	nty_coroutine *co = (nty_coroutine *)lt;
	co->func(co->arg);
	co->status |= (BIT(NTY_COROUTINE_STATUS_EXITED) | BIT(NTY_COROUTINE_STATUS_FDEOF) | BIT(NTY_COROUTINE_STATUS_DETACH));
#if 1
	nty_coroutine_yield(co);
#else
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

// 优化协程栈的内存使用
static inline void nty_coroutine_madvise(nty_coroutine *co)
{
	// 计算当前栈的使用量（current_stack），即栈顶到当前栈指针的距离。
	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp;
	assert(current_stack <= co->stack_size);

	// 如果当前栈使用量小于上一次记录的栈使用量（last_stack_size）且大于页面大小，释放未使用的栈内存。
	if (current_stack < co->last_stack_size &&
		co->last_stack_size > co->sched->page_size)
	{
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1));
		// 使用madvise系统调用通知内核，某些内存页面（MADV_DONTNEED）可以被丢弃，以减少内存占用。
		assert(madvise(co->stack, co->stack_size - tmp, MADV_DONTNEED) == 0);
	}
	// 更新last_stack_size为当前栈使用量。
	co->last_stack_size = current_stack;
}

#endif

extern int nty_schedule_create(int stack_size);

// 释放协程对象的内存
void nty_coroutine_free(nty_coroutine *co)
{
	// 如果co为空，直接返回
	if (co == NULL)
		return;
	// 减少调度器的协程计数（spawned_coroutines）
	co->sched->spawned_coroutines--;
#if 1
	if (co->stack)
	{
		// 释放协程的私有栈（co->stack）
		free(co->stack);
		co->stack = NULL;
	}
#endif
	if (co)
	{
		// 释放协程对象本身
		free(co);
	}
}

// 初始化协程的上下文和状态
static void nty_coroutine_init(nty_coroutine *co)
{
// A-ucontext 方式
#ifdef _USE_UCONTEXT
	// 获取当前上下文
	getcontext(&co->ctx);
	// 设置协程的栈（uc_stack.ss_sp 和 ss_size）为调度器的共享栈
	co->ctx.uc_stack.ss_sp = co->sched->stack;
	co->ctx.uc_stack.ss_size = co->sched->stack_size;
	// 设置上下文的返回链接（uc_link）为调度器上下文
	co->ctx.uc_link = &co->sched->ctx;
	// printf("TAG21\n");
	makecontext(&co->ctx, (void (*)(void))_exec, 1, (void *)co); // 使用makecontext设置协程的执行入口为_exec，并传入参数co
																 // printf("TAG22\n");

#else
	// B-ucontext 方式
	// 计算栈顶地址（co->stack + co->stack_size）
	void **stack = (void **)(co->stack + co->stack_size);
	// 在栈上预留空间并设置参数（co）和返回地址（NULL）
	stack[-3] = NULL;
	stack[-2] = (void *)co;
	// 初始化上下文的栈指针（esp）、帧指针（ebp）和指令指针（eip）
	co->ctx.esp = (void *)stack - (4 * sizeof(void *));
	co->ctx.ebp = (void *)stack - (3 * sizeof(void *));
	co->ctx.eip = (void *)_exec;
#endif

	// 设置协程状态为READY
	co->status = BIT(NTY_COROUTINE_STATUS_READY);
}

// 让当前协程暂停，切换回调度器
/*
ucontext 方式：
如果协程未退出，保存当前栈。
使用swapcontext切换到调度器的上下文。
--------------------------------------------------------------------------
汇编方式：
直接调用_switch切换到调度器上下文。
*/
void nty_coroutine_yield(nty_coroutine *co)
{
	co->ops = 0;
#ifdef _USE_UCONTEXT
	if ((co->status & BIT(NTY_COROUTINE_STATUS_EXITED)) == 0)
	{
		_save_stack(co);
	}
	swapcontext(&co->ctx, &co->sched->ctx);
#else
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

// 恢复执行指定的协程
int nty_coroutine_resume(nty_coroutine *co)
{
	// 如果协程是新建的（NEW状态），调用nty_coroutine_init初始化
	if (co->status & BIT(NTY_COROUTINE_STATUS_NEW))
	{
		nty_coroutine_init(co);
	}
// A-ucontext 方式
#ifdef _USE_UCONTEXT
	else
	{
		// 恢复协程的栈（_load_stack）
		_load_stack(co);
	}
#endif
	// 设置当前线程为目标协程（sched->curr_thread = co）
	nty_schedule *sched = nty_coroutine_get_sched();
	sched->curr_thread = co;
#ifdef _USE_UCONTEXT
	// 切换到协程上下文（swapcontext 或 _switch）
	swapcontext(&sched->ctx, &co->ctx);

// B-汇编方式
#else
	// 在切换后优化栈内存（nty_coroutine_madvise）
	_switch(&co->ctx, &co->sched->ctx);
	nty_coroutine_madvise(co);
#endif
	sched->curr_thread = NULL;

#if 1
	// 如果协程已退出且标记为分离（DETACH），释放协程资源
	if (co->status & BIT(NTY_COROUTINE_STATUS_EXITED))
	{
		if (co->status & BIT(NTY_COROUTINE_STATUS_DETACH))
		{
			nty_coroutine_free(co);
		}
		return -1;
	}
#endif
	return 0;
}

// 调整协程优先级，防止某个协程长时间占用CPU
void nty_coroutine_renice(nty_coroutine *co)
{
	// 增加操作计数（co->ops）
	co->ops++;
#if 1
	// 如果计数达到5，将协程加入就绪队列（ready）并让出控制权
	if (co->ops < 5)
		return;
#endif
	TAILQ_INSERT_TAIL(&nty_coroutine_get_sched()->ready, co, ready_next);
	nty_coroutine_yield(co);
}

// 让协程休眠指定时间
void nty_coroutine_sleep(uint64_t msecs)
{
	// 获取当前协程
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;

	// 如果休眠时间为0，立即加入就绪队列并让出控制权
	if (msecs == 0)
	{
		TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);
		nty_coroutine_yield(co);
	}
	else
	{
		// 否则，调用调度器的nty_schedule_sched_sleepdown函数安排休眠
		nty_schedule_sched_sleepdown(co, msecs);
	}
}

// 将当前协程标记为分离状态，结束后自动释放
void nty_coroutine_detach(void)
{
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
	// 设置DETACH标志(通知调度器在协程退出时自动调用nty_coroutine_free)
	co->status |= BIT(NTY_COROUTINE_STATUS_DETACH);
}

// 初始化线程局部存储的调度器键
// 释放TLS中的调度器对象
static void nty_coroutine_sched_key_destructor(void *data)
{
	free(data);
}
// 在程序加载时自动调用（constructor属性），创建TLS键并初始化为空。
static void __attribute__((constructor(1000))) nty_coroutine_sched_key_creator(void)
{
	assert(pthread_key_create(&global_sched_key, nty_coroutine_sched_key_destructor) == 0);
	assert(pthread_setspecific(global_sched_key, NULL) == 0);

	return;
}

// coroutine -->
// create
// 创建新的协程
int nty_coroutine_create(nty_coroutine **new_co, proc_coroutine func, void *arg)
{

	// 确保TLS键已初始化（pthread_once）
	assert(pthread_once(&sched_key_once, nty_coroutine_sched_key_creator) == 0);
	// 获取当前线程的调度器，若不存在则创建（nty_schedule_create）
	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL)
	{
		nty_schedule_create(0);

		sched = nty_coroutine_get_sched();
		if (sched == NULL)
		{
			printf("Failed to create scheduler\n");
			return -1;
		}
	}
	// 分配协程对象（calloc）
	nty_coroutine *co = calloc(1, sizeof(nty_coroutine));
	if (co == NULL)
	{
		printf("Failed to allocate memory for new coroutine\n");
		return -2;
	}
// A-ucontext 方式：不分配私有栈（使用调度器的共享栈）
#ifdef _USE_UCONTEXT
	co->stack = NULL;
	co->stack_size = 0;
// B-汇编方式：使用posix_memalign分配页面对齐的栈内存
#else
	int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size);
	if (ret)
	{
		printf("Failed to allocate stack for new coroutine\n");
		free(co);
		return -3;
	}
	co->stack_size = sched->stack_size;
#endif
	// 初始化协程的调度器、状态（NEW）、ID、函数、参数、创建时间等
	co->sched = sched;
	co->status = BIT(NTY_COROUTINE_STATUS_NEW); //
	co->id = sched->spawned_coroutines++;
	co->func = func;
#if CANCEL_FD_WAIT_UINT64
	co->fd = -1;
	co->events = 0;
#else
	co->fd_wait = -1;
#endif
	co->arg = arg;
	co->birth = nty_coroutine_usec_now();
	*new_co = co;
	// 将协程加入就绪队列（ready）
	TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);

	return 0;
}
