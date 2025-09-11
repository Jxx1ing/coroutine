# coroutine
## [NtyCo](https://github.com/wangbojing/NtyCo)

```cpp
//创建静态库
cd build
cmake ..
make 
// -> build目录下生成静态库libnty_core.a
```



```cpp
//生成服务端和客户端的可执行文件
cd sample
cmake .
make
// -> ./nty_server ./client_mutlport
```



```cpp
//运行
//服务端
./nty_server
//客户端
./client_mutlport 192.168.65.128 9096
```



## 日志

### 2025/09/09

>初步看了`nty_coroutine.c` / `nty_socket.c` / `nty_schedule.c`/ `nty_epoll.c`
>
>运行了服务端`nty_server.c` / 客户端`client_mutlport_epoll.c`（使用协程方式实现百万并发测试）
>
>**TODO**：
>
>* 队列/红黑树 的实现`nty_tree.h` / `nty_queue`
>
>* NtyCo协程库整体还需要再理一遍（目前只是有一个印象）。
>* 自己实现一个协程库C++（参考 知识星球）



### 2025/09/10

>画了**nty_server.c**的UML图，初步理清函数调用关系
>
>`nty_coroutine_create` / `nty_coroutine_init` / `nty_coroutine_resume` / `_exec` / `nty_coroutine_yield` 
>
>`nty_schedule_run` / `server` / `server_reader` / `nty_poll_inner` / `nty_accept`
>
>**TODO**：
>
>* 就绪集合(ready queue) / 睡眠集合(sleep rbtree) / 等待集合(wait rbtree) 在协程中的关系，比如线程使用信号量、锁等机制，协程既然不使用那么是如何完成类似功能的
>
>* 协程监听sockfd 和 eventfd，具体怎么实现的还需要理一遍（目前只是有一个印象，参考笔记nty_server.c）
>
>* 目前的模型是**单线程 + 多协程**（涉及到一些数据结构、类型不是很清楚，比如`pthread_key global_sched_key` / `pthread_setspecific(global_sched_key, sched)` / `pthread_getspecific(global_sched_key)`）。
>
>  此外，可以看下**多核线程 + 多协程**（M:N 协程模型），代码可能是`nty_server_mulcore.c`: [NtyCo/sample/nty_server_mulcore.c at master · wangbojing/NtyCo](https://github.com/wangbojing/NtyCo/blob/master/sample/nty_server_mulcore.c)

