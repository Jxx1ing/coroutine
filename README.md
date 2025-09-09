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



* **2025/09/09：**

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

