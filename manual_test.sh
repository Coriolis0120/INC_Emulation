#!/bin/bash
# INC Test Script - 1-2-4 Topology
# Supports AllReduce, Reduce, Broadcast, Barrier, and ReduceScatter

# 参数: TEST_TYPE DATA_COUNT
# TEST_TYPE: allreduce (默认), reduce, broadcast, barrier, reducescatter, allgather
TEST_TYPE=${1:-allreduce}
DATA_COUNT=${2:-8192}

# 1-2-4 拓扑配置
WORLD_SIZE=4
CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"  # pku1 的管理网 IP 作为 master（跨网段可达）

echo "=== INC Test (1-2-4 Topology) ==="
echo "Test type: $TEST_TYPE"
echo "Data count: $DATA_COUNT ($(($DATA_COUNT * 4 / 1024)) KB)"
echo "World size: $WORLD_SIZE"
echo ""

# 停止旧进程
./stop_inc.sh

# 编译和传输
echo "Building and deploying..."
./build_and_transport.sh

# 清空日志
rm -f ./logs/*.log
mkdir -p ./logs

echo ""
echo "=== Starting Components ==="

# 1. 启动 Controller
echo "[1/7] Starting Controller..."
ssh controller "cd /root && stdbuf -oL ./controller > /root/controller.log 2>&1" &
sleep 2

# 2. 启动 Top Switch (switch, ID=0)
echo "[2/7] Starting Top Switch (ID=0)..."
ssh switch "cd /root && stdbuf -oL ./switch_v2 $CONTROLLER_IP > /root/switch.log 2>&1" &
sleep 2

# 3. 启动 Leaf Switch 1 (vm1, ID=1)
echo "[3/7] Starting Leaf Switch 1 (vm1, ID=1)..."
ssh vm1 "cd /root && stdbuf -oL ./switch_v2 $CONTROLLER_IP > /root/switch.log 2>&1" &
sleep 2

# 4. 启动 Leaf Switch 2 (vm2, ID=2)
echo "[4/7] Starting Leaf Switch 2 (vm2, ID=2)..."
ssh vm2 "cd /root && stdbuf -oL ./switch_v2 $CONTROLLER_IP > /root/switch.log 2>&1" &
sleep 2

echo ""
echo "=== Waiting for Switches to be ready ==="
sleep 3

echo ""
echo "=== Starting Hosts ==="

# 根据测试类型选择程序和参数
# AllReduce: host_simple_test <world_size> <master_ip> <rank> <data_count>
# Reduce:    host_reduce <world_size> <master_ip> <rank> <root_rank> <data_count>
# Broadcast: host_broadcast <world_size> <master_ip> <rank> <root_rank> <data_count>

# 5. 启动 pku1 (rank 0)
echo "[5/7] Starting pku1 (rank 0)..."
if [ "$TEST_TYPE" = "reduce" ]; then
    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce $WORLD_SIZE $MASTER_IP 0 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "broadcast" ]; then
    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 0 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "barrier" ]; then
    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_barrier $WORLD_SIZE $MASTER_IP 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "reducescatter" ]; then
    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reducescatter $WORLD_SIZE $MASTER_IP 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "allgather" ]; then
    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_allgather $WORLD_SIZE $MASTER_IP 0 $DATA_COUNT > /root/host.log 2>&1" &
else
    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test $WORLD_SIZE $MASTER_IP 0 $DATA_COUNT > /root/host.log 2>&1" &
fi
sleep 1

# 6. 启动 pku2 (rank 1)
echo "[6/7] Starting pku2 (rank 1)..."
if [ "$TEST_TYPE" = "reduce" ]; then
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce $WORLD_SIZE $MASTER_IP 1 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "broadcast" ]; then
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 1 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "barrier" ]; then
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_barrier $WORLD_SIZE $MASTER_IP 1 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "reducescatter" ]; then
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reducescatter $WORLD_SIZE $MASTER_IP 1 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "allgather" ]; then
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_allgather $WORLD_SIZE $MASTER_IP 1 $DATA_COUNT > /root/host.log 2>&1" &
else
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test $WORLD_SIZE $MASTER_IP 1 $DATA_COUNT > /root/host.log 2>&1" &
fi
sleep 1

# 7. 启动 pku3 (rank 2) 和 pku4 (rank 3)
echo "[7/7] Starting pku3 (rank 2) and pku4 (rank 3)..."
if [ "$TEST_TYPE" = "reduce" ]; then
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce $WORLD_SIZE $MASTER_IP 2 0 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reduce $WORLD_SIZE $MASTER_IP 3 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "broadcast" ]; then
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 2 0 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 3 0 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "barrier" ]; then
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_barrier $WORLD_SIZE $MASTER_IP 2 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_barrier $WORLD_SIZE $MASTER_IP 3 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "reducescatter" ]; then
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reducescatter $WORLD_SIZE $MASTER_IP 2 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_reducescatter $WORLD_SIZE $MASTER_IP 3 $DATA_COUNT > /root/host.log 2>&1" &
elif [ "$TEST_TYPE" = "allgather" ]; then
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_allgather $WORLD_SIZE $MASTER_IP 2 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_allgather $WORLD_SIZE $MASTER_IP 3 $DATA_COUNT > /root/host.log 2>&1" &
else
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test $WORLD_SIZE $MASTER_IP 2 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test $WORLD_SIZE $MASTER_IP 3 $DATA_COUNT > /root/host.log 2>&1" &
fi

echo ""
echo "=== Waiting for completion ==="

# 轮询检测测试是否完成
MAX_WAIT=300
ELAPSED=0
while [ $ELAPSED -lt $MAX_WAIT ]; do
    sleep 2
    ELAPSED=$((ELAPSED + 2))

    # 检查所有 Host 日志是否都包含完成标志
    PKU1_DONE=$(ssh pku1 "grep -qE 'Test Complete|Test completed' /root/host.log 2>/dev/null && echo 1 || echo 0")
    PKU2_DONE=$(ssh pku2 "grep -qE 'Test Complete|Test completed' /root/host.log 2>/dev/null && echo 1 || echo 0")
    PKU3_DONE=$(ssh pku3 "grep -qE 'Test Complete|Test completed' /root/host.log 2>/dev/null && echo 1 || echo 0")
    PKU4_DONE=$(ssh pku4 "grep -qE 'Test Complete|Test completed' /root/host.log 2>/dev/null && echo 1 || echo 0")

    if [ "$PKU1_DONE" = "1" ] && [ "$PKU2_DONE" = "1" ] && [ "$PKU3_DONE" = "1" ] && [ "$PKU4_DONE" = "1" ]; then
        echo "All hosts completed after ${ELAPSED}s"
        break
    fi

    # 每10秒打印一次进度
    if [ $((ELAPSED % 10)) -eq 0 ]; then
        echo "  Waiting... ${ELAPSED}s (pku1=$PKU1_DONE pku2=$PKU2_DONE pku3=$PKU3_DONE pku4=$PKU4_DONE)"
    fi
done

if [ $ELAPSED -ge $MAX_WAIT ]; then
    echo "Timeout after ${MAX_WAIT}s"
fi

# 停止进程
./stop_inc.sh

# 收集日志
echo ""
echo "=== Collecting Logs ==="

ssh controller "cat /root/controller.log 2>/dev/null" > ./logs/controller.log
ssh switch "cat /root/switch.log 2>/dev/null" > ./logs/switch0.log
ssh vm1 "cat /root/switch.log 2>/dev/null" > ./logs/switch1.log
ssh vm2 "cat /root/switch.log 2>/dev/null" > ./logs/switch2.log
ssh pku1 "cat /root/host.log 2>/dev/null" > ./logs/pku1.log
ssh pku2 "cat /root/host.log 2>/dev/null" > ./logs/pku2.log
ssh pku3 "cat /root/host.log 2>/dev/null" > ./logs/pku3.log
ssh pku4 "cat /root/host.log 2>/dev/null" > ./logs/pku4.log

echo "Logs saved to ./logs/"

echo ""
echo "=== Test Results ==="
echo ""
echo "--- pku1 (rank 0) ---"
tail -15 ./logs/pku1.log
echo ""
echo "--- pku2 (rank 1) ---"
tail -15 ./logs/pku2.log
echo ""
echo "--- pku3 (rank 2) ---"
tail -15 ./logs/pku3.log
echo ""
echo "--- pku4 (rank 3) ---"
tail -15 ./logs/pku4.log
echo ""
echo "=== Done ==="
