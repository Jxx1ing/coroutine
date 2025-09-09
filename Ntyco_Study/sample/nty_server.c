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

// 包含自定义的协程库头文件，提供了协程相关的函数（如 nty_coroutine_create 和 nty_schedule_run）以及可能封装的网络操作函数（如 nty_socket、nty_accept 等）
#include "nty_coroutine.h"
// 包含网络编程所需的头文件，提供 TCP/IP 协议相关的函数和结构（如 socket、bind、listen、htons 等）
#include <arpa/inet.h>

#define MAX_CLIENT_NUM 1000000
#define TIME_SUB_MS(tv1, tv2) ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// 回调函数，server函数中创建协程后被调用：nty_coroutine_create(&read_co, server_reader, arg);
// server_reader 是为每个客户端连接创建的协程函数，负责处理客户端的读写操作。参数 arg 是一个指向整数的文件描述符（fd）的指针
void server_reader(void *arg)
{
	int fd = *(int *)arg;
	free(arg);
	int ret = 0;

	struct pollfd fds;
	fds.fd = fd;
	fds.events = POLLIN; // 设置关注的事件为 POLLIN，表示关注文件描述符是否可读

	while (1)
	{

		char buf[1024] = {0};
		// 调用 nty_recv 从客户端 socket 读取数据到 buf，最大读取 1024 字节。第四个参数 0 表示无特殊标志。ret 存储读取的字节数或错误码。
		ret = nty_recv(fd, buf, 1024, 0);
		if (ret > 0)
		{
			if (fd > MAX_CLIENT_NUM)
				printf("read from server: %.*s\n", ret, buf);

			// 调用 nty_send 将接收到的数据回传给客户端，实现简单的回显（echo）功能。
			ret = nty_send(fd, buf, strlen(buf), 0);
			if (ret == -1)
			{
				nty_close(fd);
				break;
			}
		}
		else if (ret == 0)
		{
			// 调用 nty_close 关闭客户端 socket
			nty_close(fd);
			break;
		}
	}
}
/*
总结：
server_reader 函数是一个协程，负责处理单个客户端连接的读写操作。
它从客户端读取数据，然后将数据回传给客户端。如果连接断开或出错，协程关闭 socket 并退出。
*/

// server 是为每个监听端口创建的协程函数，负责创建 TCP 服务器并接受客户端连接。参数 arg 是一个指向端口号的指针
void server(void *arg)
{

	unsigned short port = *(unsigned short *)arg;
	free(arg);

	// 调用 nty_socket 创建一个 TCP socket。AF_INET 表示 IPv4，SOCK_STREAM 表示 TCP 协议，第三个参数 0 表示使用默认协议。
	int fd = nty_socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	// 定义两个 sockaddr_in 结构，local 用于服务器地址，remote 用于客户端地址
	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr *)&local, sizeof(struct sockaddr_in));

	listen(fd, 20); // 允许最多 20 个待处理的连接请求
	printf("listen port : %d\n", port);

	struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

	while (1)
	{
		socklen_t len = sizeof(struct sockaddr_in);
		// 用 nty_accept 接受客户端连接，返回客户端的文件描述符 cli_fd，并填充客户端地址信息到 remote。
		int cli_fd = nty_accept(fd, (struct sockaddr *)&remote, &len);
		if (cli_fd % 1000 == 999)
		{

			struct timeval tv_cur;
			memcpy(&tv_cur, &tv_begin, sizeof(struct timeval));

			gettimeofday(&tv_begin, NULL);
			int time_used = TIME_SUB_MS(tv_begin, tv_cur);

			printf("client fd : %d, time_used: %d\n", cli_fd, time_used);
		}
		printf("new client comming\n");

		// 定义一个协程指针 read_co，用于为新客户端创建协程
		nty_coroutine *read_co;
		int *arg = malloc(sizeof(int));
		*arg = cli_fd; // 将客户端文件描述符存储到动态分配的内存中
		// 为新客户端创建协程，指定 server_reader 函数和参数 arg。该协程负责处理客户端的读写操作。
		nty_coroutine_create(&read_co, server_reader, arg);
	}
}
/*
总结：
server 函数是一个协程，
负责创建一个 TCP 服务器，监听指定端口，接受客户端连接，并为每个客户端创建 server_reader 协程处理数据交互。它还记录每 1000 个连接的时间消耗。
*/

// 创建 100 个 server 协程，监听 100 个端口（9096 到 9195），然后启动协程调度器运行所有协程。
int main(int argc, char *argv[])
{
	nty_coroutine *co = NULL;

	int i = 0;
	// 监听端口 9096 到 9195
	unsigned short base_port = 9096;
	for (i = 0; i < 100; i++)
	{
		unsigned short *port = calloc(1, sizeof(unsigned short));
		*port = base_port + i;

		// 为每个端口创建 server 协程，传入端口号作为参数
		nty_coroutine_create(&co, server, port); ////////no run
	}

	// 启动协程调度器，运行所有创建的协程。协程库会管理 server 和 server_reader 协程的执行，通过非阻塞 I/O 和协程切换实现的并发。
	nty_schedule_run(); // run

	return 0;
}
