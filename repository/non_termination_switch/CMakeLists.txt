cmake_minimum_required(VERSION 3.10)
project(INC_NonTermSwitch C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 优化编译选项
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(OPT_FLAGS -O3 -march=native -funroll-loops)
endif()

# 头文件目录
include_directories(include)

# 查找静态 yaml-cpp
find_library(YAML_CPP_STATIC libyaml-cpp.a)

# 公共源文件
set(COMMON_SRCS
    switch_context.c
    controller_comm.cpp
    util.c
    rule.c
)

# switch_reduce
add_executable(switch_reduce switch_reduce.c ${COMMON_SRCS})
target_link_libraries(switch_reduce pthread pcap ${YAML_CPP_STATIC})
if(OPT_FLAGS)
    target_compile_options(switch_reduce PRIVATE ${OPT_FLAGS})
endif()

# switch_broadcast
add_executable(switch_broadcast switch_broadcast.c ${COMMON_SRCS})
target_link_libraries(switch_broadcast pthread pcap ${YAML_CPP_STATIC})
if(OPT_FLAGS)
    target_compile_options(switch_broadcast PRIVATE ${OPT_FLAGS})
endif()
