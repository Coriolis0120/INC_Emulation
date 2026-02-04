#!/bin/bash
# Mode1 Hierarchical Test - 1 Spine + 1 Leaf + 2 Host
# 验证分层聚合功能

SWITCH_PORT=${1:-52400}
GID_IDX=${2:-1}

SPINE_HOST="switch"
LEAF_HOST="vm1"
SPINE_DATA_IP="172.16.0.21"
LEAF_DATA_IP="10.1.1.25"
WORLD_SIZE=2

echo "=== Mode1 Hierarchical Test ==="
echo "Spine: $SPINE_HOST ($SPINE_DATA_IP)"
echo "Leaf: $LEAF_HOST ($LEAF_DATA_IP)"
echo "Port: $SWITCH_PORT, GID: $GID_IDX"
echo ""

# 编译
cd /root/NetLab/new/INC_Emulation/repository/build
cmake .. > /dev/null && make switch_mode1 host_mode1 -j4
cd /root/NetLab/new/INC_Emulation

mkdir -p ./logs/mode1
rm -f ./logs/mode1/*.log

# 停止旧进程
ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh $LEAF_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -f host_mode1" 2>/dev/null
ssh pku2 "pkill -f host_mode1" 2>/dev/null
sleep 1

# 部署
scp ./repository/build/switch_mode1 $SPINE_HOST:/root/
scp ./repository/build/switch_mode1 $LEAF_HOST:/root/
scp ./repository/build/host_mode1 pku1:/root/
scp ./repository/build/host_mode1 pku2:/root/

echo "=== Starting Spine ==="
ssh $SPINE_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -r > /root/switch_mode1.log 2>&1" &
sleep 2

echo "=== Starting Leaf ==="
ssh $LEAF_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -s $SPINE_DATA_IP -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 2

echo "=== Starting Hosts ==="
ssh pku1 "cd /root && ./host_mode1 $LEAF_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE > /root/host_mode1.log 2>&1" &
sleep 1
ssh pku2 "cd /root && ./host_mode1 $LEAF_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE > /root/host_mode1.log 2>&1" &

echo "=== Waiting ==="
for i in {1..15}; do
    sleep 2
    P1=$(ssh pku1 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")
    P2=$(ssh pku2 "grep -q 'Test complete' /root/host_mode1.log 2>/dev/null && echo 1 || echo 0")
    echo "  ${i}s: pku1=$P1 pku2=$P2"
    [ "$P1" = "1" ] && [ "$P2" = "1" ] && break
done

# 停止
ssh $SPINE_HOST "pkill -f switch_mode1" 2>/dev/null
ssh $LEAF_HOST "pkill -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -f host_mode1" 2>/dev/null
ssh pku2 "pkill -f host_mode1" 2>/dev/null

# 收集日志
echo ""
echo "=== Spine Log ==="
ssh $SPINE_HOST "cat /root/switch_mode1.log"

echo ""
echo "=== Leaf Log ==="
ssh $LEAF_HOST "cat /root/switch_mode1.log"

echo ""
echo "=== pku1 Log ==="
ssh pku1 "cat /root/host_mode1.log"

echo ""
echo "=== pku2 Log ==="
ssh pku2 "cat /root/host_mode1.log"
