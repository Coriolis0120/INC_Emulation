# Mode1: Connection Termination Mode
# 连接终结模式 - 使用硬件 RoCE

set(MODE1_SOURCES
    qp_manager.c
    inc_engine.c
    hierarchical_ps.c
    host_api.c
)

# 创建静态库
add_library(mode1 STATIC ${MODE1_SOURCES})

target_include_directories(mode1 PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(mode1
    ibverbs
    pthread
)
