cmake_minimum_required(VERSION 3.15)
project(nty_mysamples)

# 设置编译选项
set(CMAKE_C_STANDARD 11)

# 添加头文件
include_directories(${PROJECT_SOURCE_DIR}/..)

# 添加静态库路径
link_directories(${PROJECT_SOURCE_DIR}/../build)

# 添加可执行文件
add_executable(nty_server nty_server.c)
add_executable(client_mutlport client_mutlport_epoll.c)

# 链接静态库
target_link_libraries(nty_server nty_core)
target_link_libraries(client_mutlport nty_core)

