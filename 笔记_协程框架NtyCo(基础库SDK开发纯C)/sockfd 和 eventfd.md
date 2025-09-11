# sockfd 和 eventfd

ps:  印象中，源码应该有一部分是计数的，不知道和eventfd有没有关系。具体可以看一下代码`nty_coroutine.c` 和 `nty_schedule.c`。 这里把源码直接喂给了chatgpt, 以此来理解sockfd和eventfd。

结合源码，梳理**eventfd** 和 **sockfd** 在这个协程库中的使用方式



## 基本区分

<img src="sockfd 和 eventfd.assets/image-20250910214701142.png" alt="image-20250910214701142" style="zoom:80%;" />

## 代码

### eventfd

<img src="sockfd 和 eventfd.assets/image-20250910214800666.png" alt="image-20250910214800666" style="zoom:80%;" />

### sockfd

<img src="sockfd 和 eventfd.assets/image-20250910214828742.png" alt="image-20250910214828742" style="zoom:80%;" />

### epoll_wait

<img src="sockfd 和 eventfd.assets/image-20250910214903746.png" alt="image-20250910214903746" style="zoom:80%;" />



<img src="sockfd 和 eventfd.assets/image-20250910214936455.png" alt="image-20250910214936455" style="zoom:67%;" />