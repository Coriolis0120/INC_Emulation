#!/bin/bash
# Mode1 Test Script - Connection Termination Mode
# 1-2-4 拓扑: 1 Spine + 2 Leaf + 4 Host
#
# 拓扑结构:
#                    Switch0 (Spine)
#                   /              \
#           Switch1 (Leaf1)    Switch2 (Leaf2)
#           /        \          /        \
#        pku1      pku2      pku3      pku4
#       rank=0    rank=1    rank=2    rank=3

SWITCH_PORT=${1:-52400}
GID_IDX=${2:-1}

# 拓扑配置
SPINE_HOST="switch"
LEAF1_HOST="vm1"
LEAF2_HOST="vm2"

# Host 连接的 Leaf Switch 数据网 IP
LEAF1_DATA_IP="10.1.1.25"    # Switch1 eth2
LEAF2_DATA_IP="10.2.1.18"    # Switch2 eth2

# Spine Switch 数据网 IP (Leaf 连接用)
SPINE_DATA_IP_FOR_LEAF1="172.16.0.21"  # Spine eth1 -> vm1
SPINE_DATA_IP_FOR_LEAF2="10.10.0.14"   # Spine eth2 -> vm2

WORLD_SIZE=4

echo "=== Mode1 Test (1-2-4 Topology) ==="
echo "Spine: $SPINE_HOST"
echo "Leaf1: $LEAF1_HOST (data: $LEAF1_DATA_IP)"
echo "Leaf2: $LEAF2_HOST (data: $LEAF2_DATA_IP)"
echo "Port: $SWITCH_PORT, GID: $GID_IDX"
echo "World size: $WORLD_SIZE"
echo ""

# 编译
echo "=== Building ==="
cd /root/NetLab/new/INC_Emulation/repository/build
cmake .. > /dev/null
make switch_mode1 host_mode1 -j4
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi
echo "Build successful"
cd /root/NetLab/new/INC_Emulation

# 创建日志目录
mkdir -p ./logs/mode1
rm -f ./logs/mode1/*.log

# 停止旧进程
echo ""
echo "=== Stopping old processes ==="
ssh $SPINE_HOST "pkill -f switch_mode1" 2>/dev/null
ssh $LEAF1_HOST "pkill -f switch_mode1" 2>/dev/null
ssh $LEAF2_HOST "pkill -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -f host_mode1" 2>/dev/null
ssh pku2 "pkill -f host_mode1" 2>/dev/null
ssh pku3 "pkill -f host_mode1" 2>/dev/null
ssh pku4 "pkill -f host_mode1" 2>/dev/null
sleep 1

# 部署程序
echo ""
echo "=== Deploying binaries ==="
scp ./repository/build/switch_mode1 $SPINE_HOST:/root/
scp ./repository/build/switch_mode1 $LEAF1_HOST:/root/
scp ./repository/build/switch_mode1 $LEAF2_HOST:/root/
scp ./repository/build/host_mode1 pku1:/root/
scp ./repository/build/host_mode1 pku2:/root/
scp ./repository/build/host_mode1 pku3:/root/
scp ./repository/build/host_mode1 pku4:/root/
echo "Deploy complete"

# 启动 Switches (先启动 Spine，再启动 Leaf)
echo ""
echo "=== Starting Switches ==="

# Spine Switch (ROOT)
echo "[1/3] Starting Spine Switch on $SPINE_HOST..."
ssh $SPINE_HOST "cd /root && stdbuf -oL ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -r > /root/switch_mode1.log 2>&1" &
sleep 2

# Leaf1 Switch
echo "[2/3] Starting Leaf1 Switch on $LEAF1_HOST..."
ssh $LEAF1_HOST "cd /root && stdbuf -oL ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -s $SPINE_DATA_IP_FOR_LEAF1 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 2

# Leaf2 Switch
echo "[3/3] Starting Leaf2 Switch on $LEAF2_HOST..."
ssh $LEAF2_HOST "cd /root && stdbuf -oL ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -s $SPINE_DATA_IP_FOR_LEAF2 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 2

echo "All switches started"

# 启动 Hosts
echo ""
echo "=== Starting Hosts ==="

# pku1, pku2 连接到 Leaf1
echo "[1/4] Starting pku1 (rank=0) -> Leaf1..."
ssh pku1 "cd /root && stdbuf -oL ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE > /root/host_mode1.log 2>&1" &
sleep 1

echo "[2/4] Starting pku2 (rank=1) -> Leaf1..."
ssh pku2 "cd /root && stdbuf -oL ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE > /root/host_mode1.log 2>&1" &
sleep 1

# pku3, pku4 连接到 Leaf2
echo "[3/4] Starting pku3 (rank=2) -> Leaf2..."
ssh pku3 "cd /root && stdbuf -oL ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 2 $WORLD_SIZE > /root/host_mode1.log 2>&1" &
sleep 1

echo "[4/4] Starting pku4 (rank=3) -> Leaf2..."
ssh pku4 "cd /root && stdbuf -oL ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 3 $WORLD_SIZE > /root/host_mode1.log 2>&1" &

echo "All hosts started"

# 等待测试完成
echo ""
echo "=== Waiting for completion ==="
MAX_WAIT=30
ELAPSED=0
while [ $ELAPSED -lt $MAX_WAIT ]; do
    sleep 2
    ELAPSED=$((ELAPSED + 2))

    # 检查所有 Host 日志
    PKU1_DONE=$(ssh pku1 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")
    PKU2_DONE=$(ssh pku2 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")
    PKU3_DONE=$(ssh pku3 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")
    PKU4_DONE=$(ssh pku4 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")

    if [ "$PKU1_DONE" = "1" ] && [ "$PKU2_DONE" = "1" ] && \
       [ "$PKU3_DONE" = "1" ] && [ "$PKU4_DONE" = "1" ]; then
        echo "All hosts completed after ${ELAPSED}s"
        break
    fi

    echo "  Waiting... ${ELAPSED}s (pku1=$PKU1_DONE pku2=$PKU2_DONE pku3=$PKU3_DONE pku4=$PKU4_DONE)"
done

if [ $ELAPSED -ge $MAX_WAIT ]; then
    echo "Timeout after ${MAX_WAIT}s"
fi

# 停止进程
echo ""
echo "=== Stopping processes ==="
ssh $SPINE_HOST "pkill -f switch_mode1" 2>/dev/null
ssh $LEAF1_HOST "pkill -f switch_mode1" 2>/dev/null
ssh $LEAF2_HOST "pkill -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -f host_mode1" 2>/dev/null
ssh pku2 "pkill -f host_mode1" 2>/dev/null
ssh pku3 "pkill -f host_mode1" 2>/dev/null
ssh pku4 "pkill -f host_mode1" 2>/dev/null

# 收集日志
echo ""
echo "=== Collecting Logs ==="
ssh $SPINE_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1/spine.log
ssh $LEAF1_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1/leaf1.log
ssh $LEAF2_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1/leaf2.log
ssh pku1 "cat /root/host_mode1.log 2>/dev/null" > ./logs/mode1/pku1.log
ssh pku2 "cat /root/host_mode1.log 2>/dev/null" > ./logs/mode1/pku2.log
ssh pku3 "cat /root/host_mode1.log 2>/dev/null" > ./logs/mode1/pku3.log
ssh pku4 "cat /root/host_mode1.log 2>/dev/null" > ./logs/mode1/pku4.log
echo "Logs saved to ./logs/mode1/"

# 显示结果
echo ""
echo "=== Test Results ==="

echo ""
echo "--- Spine Switch ---"
tail -15 ./logs/mode1/spine.log

echo ""
echo "--- Leaf1 Switch ---"
tail -10 ./logs/mode1/leaf1.log

echo ""
echo "--- Leaf2 Switch ---"
tail -10 ./logs/mode1/leaf2.log

echo ""
echo "--- pku1 (rank=0) ---"
tail -10 ./logs/mode1/pku1.log

echo ""
echo "--- pku2 (rank=1) ---"
tail -10 ./logs/mode1/pku2.log

echo ""
echo "--- pku3 (rank=2) ---"
tail -10 ./logs/mode1/pku3.log

echo ""
echo "--- pku4 (rank=3) ---"
tail -10 ./logs/mode1/pku4.log

echo ""
echo "=== Done ==="
