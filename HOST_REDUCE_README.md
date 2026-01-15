# Reduce操作测试程序使用说明

## 文件说明

- **host_reduce.c** - Reduce操作测试程序
- 基于原始的host.c修改，专门用于测试Reduce集合通信原语

## 编译方法

### 1. 修改CMakeLists.txt

在 `repository/CMakeLists.txt` 中添加host_reduce目标：

```cmake
# 添加host_reduce可执行文件
add_executable(host_reduce src/host_reduce.c src/api.c src/util.c)
target_link_libraries(host_reduce ${LIBS})
```

### 2. 编译

```bash
cd repository/build
cmake ..
make host_reduce
```

编译成功后会生成 `build/host_reduce` 可执行文件。

