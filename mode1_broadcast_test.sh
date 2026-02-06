#!/bin/bash
# Mode1 Broadcast Test Script
# 1-2-4 拓扑: 1 Spine + 2 Leaf + 4 Host
#
# 用法: ./mode1_broadcast_test.sh [COUNT] [ROOT_RANK]
# 示例:
#   ./mode1_broadcast_test.sh              # 默认测试 1024 元素，root=0
#   ./mode1_broadcast_test.sh 1048576 2    # 测试 4MB，root=2

COUNT=${1:-1024}
ROOT_RANK=${2:-0}
SWITCH_PORT=52410
GID_IDX=1

# 拓扑配置
SPINE_HOST="switch"
LEAF1_HOST="vm1"
LEAF2_HOST="vm2"

LEAF1_DATA_IP="10.1.1.25"
LEAF2_DATA_IP="10.2.1.18"
SPINE_DATA_IP_FOR_LEAF1="172.16.0.21"
SPINE_DATA_IP_FOR_LEAF2="10.10.0.14"

WORLD_SIZE=4

cleanup() {
    echo ""
    echo "=== Cleanup ==="
    ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh pku1 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
    ssh pku2 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
    ssh pku3 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
    ssh pku4 "pkill -9 -f host_broadcast_mode1" 2>/dev/null

    # 收集日志
    mkdir -p ./logs/mode1_broadcast
    ssh $SPINE_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1_broadcast/spine.log
    ssh $LEAF1_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1_broadcast/leaf1.log
    ssh $LEAF2_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1_broadcast/leaf2.log
    echo "Logs saved to ./logs/mode1_broadcast/"
}

trap cleanup EXIT INT TERM

echo "=== Mode1 Broadcast Test ==="
echo "Count: $COUNT ($(($COUNT * 4)) bytes)"
echo "Root rank: $ROOT_RANK"
echo "Port: $SWITCH_PORT, GID: $GID_IDX"
echo ""

# 编译
echo "=== Building ==="
cd /root/NetLab/new/INC_Emulation/repository/build
cmake .. > /dev/null 2>&1
make switch_mode1 host_broadcast_mode1 -j4 > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi
echo "Build successful"
cd /root/NetLab/new/INC_Emulation

# 停止旧进程
echo ""
echo "=== Stopping old processes ==="
ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
ssh pku2 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
ssh pku3 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
ssh pku4 "pkill -9 -f host_broadcast_mode1" 2>/dev/null
sleep 2

# 部署程序
echo ""
echo "=== Deploying binaries ==="
scp ./repository/build/switch_mode1 $SPINE_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF1_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF2_HOST:/root/ 2>/dev/null
scp ./repository/build/host_broadcast_mode1 pku1:/root/ 2>/dev/null
scp ./repository/build/host_broadcast_mode1 pku2:/root/ 2>/dev/null
scp ./repository/build/host_broadcast_mode1 pku3:/root/ 2>/dev/null
scp ./repository/build/host_broadcast_mode1 pku4:/root/ 2>/dev/null
echo "Deploy complete"

# 启动 Switches
echo ""
echo "=== Starting Switches ==="

# Spine (ROOT)
ssh $SPINE_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -r -c 2 -d rxe_eth1,rxe_eth2 > /root/switch_mode1.log 2>&1" &
sleep 2

# Leaf1
ssh $LEAF1_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF1 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 1

# Leaf2
ssh $LEAF2_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF2 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 2

echo "Switches started"

# 启动 Hosts
echo ""
echo "=== Starting Hosts (Broadcast test, root=$ROOT_RANK) ==="

# pku1 (rank 0)
ssh pku1 "cd /root && ./host_broadcast_mode1 $LEAF1_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE $ROOT_RANK $COUNT" &
PID1=$!

# pku2 (rank 1)
ssh pku2 "cd /root && ./host_broadcast_mode1 $LEAF1_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE $ROOT_RANK $COUNT" &
PID2=$!

# pku3 (rank 2)
ssh pku3 "cd /root && ./host_broadcast_mode1 $LEAF2_DATA_IP $SWITCH_PORT 2 $WORLD_SIZE $ROOT_RANK $COUNT" &
PID3=$!

# pku4 (rank 3)
ssh pku4 "cd /root && ./host_broadcast_mode1 $LEAF2_DATA_IP $SWITCH_PORT 3 $WORLD_SIZE $ROOT_RANK $COUNT" &
PID4=$!

# 等待所有 Host 完成
wait $PID1 $PID2 $PID3 $PID4

echo ""
echo "=== Test Complete ==="
