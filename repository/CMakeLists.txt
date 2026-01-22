cmake_minimum_required(VERSION 3.10)
project(INC C CXX)

# 设置全局静态链接
set(BUILD_SHARED_LIBS OFF)  # 禁用动态库
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)  # 优先查找静态库

# 设置静态链接标志
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include_directories(include third_party/C-Thread-Pool termination_switch/include)

set(COMMON_SRCS
    src/api.c
    src/util.c
    src/log.c
    src/rule.c
)

set(CONFIG_PARSER src/topo_parser.cpp)

# 查找静态库
find_library(YAML_CPP_STATIC libyaml-cpp.a)

# 优化编译选项
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(OPT_FLAGS -O3 -march=native -funroll-loops)
endif()

# termination_switch
add_executable(switch src/switch.c third_party/C-Thread-Pool/thpool.c ${COMMON_SRCS} ${CONFIG_PARSER})
target_link_libraries(switch ibverbs pthread pcap ${YAML_CPP_STATIC} )
if(OPT_FLAGS)
    target_compile_options(switch PRIVATE ${OPT_FLAGS})
endif()

# termination_switch_v2 (重构版本 - 测试用)
add_executable(test_switch_context
    termination_switch/src/test_context.c
    termination_switch/src/switch_context.c
    third_party/C-Thread-Pool/thpool.c
    src/rule.c)
target_link_libraries(test_switch_context pthread )
if(OPT_FLAGS)
    target_compile_options(test_switch_context PRIVATE ${OPT_FLAGS})
endif()

# termination_switch_v2 主程序
add_executable(switch_v2
    termination_switch/src/switch_main.c
    termination_switch/src/switch_context.c
    termination_switch/src/controller_comm.c
    third_party/C-Thread-Pool/thpool.c
    src/rule.c)
target_link_libraries(switch_v2 pthread pcap)
if(OPT_FLAGS)
    target_compile_options(switch_v2 PRIVATE ${OPT_FLAGS})
endif()

# 测试控制器
add_executable(test_controller termination_switch/src/test_controller.c)
target_link_libraries(test_controller pthread)



# non_termination_switch
add_executable(non_termination_switch src/non_termination_switch.c third_party/C-Thread-Pool/thpool.c ${COMMON_SRCS} ${CONFIG_PARSER})
target_link_libraries(non_termination_switch ibverbs pthread pcap ${YAML_CPP_STATIC} )
if(OPT_FLAGS)
    target_compile_options(non_termination_switch PRIVATE ${OPT_FLAGS})
endif()

# host
add_executable(host src/host.c ${COMMON_SRCS} ${CONFIG_PARSER})
target_link_libraries(host ibverbs pthread pcap ${YAML_CPP_STATIC})
if(OPT_FLAGS)
    target_compile_options(host PRIVATE ${OPT_FLAGS})
endif()

# controller
add_executable(controller src/controller.cpp ${COMMON_SRCS} ${CONFIG_PARSER})
target_link_libraries(controller ibverbs pthread pcap ${YAML_CPP_STATIC})
if(OPT_FLAGS)
    target_compile_options(controller PRIVATE ${OPT_FLAGS})
endif()