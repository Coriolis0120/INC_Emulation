#!/bin/bash
# Mode1 Simple Test - 单 Leaf 架构验证
# 1 Leaf + 2 Host

SWITCH_PORT=${1:-52400}
GID_IDX=${2:-1}
DATA_COUNT=${3:-1024}

LEAF_HOST="vm1"
LEAF_DATA_IP="10.1.1.25"
WORLD_SIZE=2

echo "=== Mode1 Simple Test (1 Leaf + 2 Host) ==="
echo "Leaf: $LEAF_HOST (data: $LEAF_DATA_IP)"
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
ssh $LEAF_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -f host_mode1" 2>/dev/null
ssh pku2 "pkill -f host_mode1" 2>/dev/null
sleep 1

# 部署程序
echo ""
echo "=== Deploying binaries ==="
scp ./repository/build/switch_mode1 $LEAF_HOST:/root/
scp ./repository/build/host_mode1 pku1:/root/
scp ./repository/build/host_mode1 pku2:/root/
echo "Deploy complete"

# 启动 Leaf Switch
echo ""
echo "=== Starting Leaf Switch ==="
ssh $LEAF_HOST "cd /root && stdbuf -oL ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX > /root/switch_mode1.log 2>&1" &
sleep 2

# 启动 Hosts
echo ""
echo "=== Starting Hosts ==="
echo "[1/2] Starting pku1 (rank=0)..."
ssh pku1 "cd /root && stdbuf -oL ./host_mode1 $LEAF_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE $DATA_COUNT > /root/host_mode1.log 2>&1" &
sleep 1

echo "[2/2] Starting pku2 (rank=1)..."
ssh pku2 "cd /root && stdbuf -oL ./host_mode1 $LEAF_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE $DATA_COUNT > /root/host_mode1.log 2>&1" &

echo "All started"

# 等待测试完成
echo ""
echo "=== Waiting for completion ==="
MAX_WAIT=20
ELAPSED=0
while [ $ELAPSED -lt $MAX_WAIT ]; do
    sleep 2
    ELAPSED=$((ELAPSED + 2))

    PKU1_DONE=$(ssh pku1 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")
    PKU2_DONE=$(ssh pku2 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")

    if [ "$PKU1_DONE" = "1" ] && [ "$PKU2_DONE" = "1" ]; then
        echo "All hosts completed after ${ELAPSED}s"
        break
    fi

    echo "  Waiting... ${ELAPSED}s (pku1=$PKU1_DONE pku2=$PKU2_DONE)"
done

if [ $ELAPSED -ge $MAX_WAIT ]; then
    echo "Timeout after ${MAX_WAIT}s"
fi

# 停止进程
echo ""
echo "=== Stopping processes ==="
ssh $LEAF_HOST "pkill -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -f host_mode1" 2>/dev/null
ssh pku2 "pkill -f host_mode1" 2>/dev/null

# 收集日志
echo ""
echo "=== Collecting Logs ==="
ssh $LEAF_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1/leaf.log
ssh pku1 "cat /root/host_mode1.log 2>/dev/null" > ./logs/mode1/pku1.log
ssh pku2 "cat /root/host_mode1.log 2>/dev/null" > ./logs/mode1/pku2.log

# 显示结果
echo ""
echo "--- Leaf Switch ---"
cat ./logs/mode1/leaf.log

echo ""
echo "--- pku1 (rank=0) ---"
cat ./logs/mode1/pku1.log

echo ""
echo "--- pku2 (rank=1) ---"
cat ./logs/mode1/pku2.log

echo ""
echo "=== Done ==="
