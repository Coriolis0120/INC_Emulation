#!/bin/bash
# Broadcast 自动化测试脚本 - 1-2-4 多层树形拓扑
# 用法: ./test_broadcast.sh [DATA_COUNT] [TIMEOUT]

set -e

# 参数
DATA_COUNT=${1:-8192}
TIMEOUT=${2:-60}

# 拓扑配置
WORLD_SIZE=4
CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"

# 节点配置
CONTROLLER_HOST="controller"
SPINE_HOST="switch"
LEAF1_HOST="vm1"
LEAF2_HOST="vm2"
HOST0="pku1"
HOST1="pku2"
HOST2="pku3"
HOST3="pku4"

# 日志目录
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="./logs/broadcast_${TIMESTAMP}"
mkdir -p "$LOG_DIR"

echo "=========================================="
echo "  Broadcast Test - 1-2-4 Topology"
echo "=========================================="
echo "Data count: $DATA_COUNT ($(($DATA_COUNT * 4 / 1024)) KB)"
echo "Timeout: ${TIMEOUT}s"
echo "Log dir: $LOG_DIR"
echo ""

# 停止旧进程
stop_all() {
    echo "[STOP] Stopping old processes..."
    for host in $CONTROLLER_HOST $SPINE_HOST $LEAF1_HOST $LEAF2_HOST $HOST0 $HOST1 $HOST2 $HOST3; do
        ssh $host "pkill -9 controller 2>/dev/null; pkill -9 switch_broadcast 2>/dev/null; pkill -9 host_broadcast 2>/dev/null; pkill -9 switch 2>/dev/null" 2>/dev/null || true
    done
    sleep 2
    # 再次确认进程已停止
    for host in $SPINE_HOST $LEAF1_HOST $LEAF2_HOST; do
        ssh $host "pkill -9 -f switch_broadcast" 2>/dev/null || true
    done
    sleep 1
}

# 清空远程日志
clear_logs() {
    echo "[CLEAR] Clearing remote logs..."
    for host in $CONTROLLER_HOST $SPINE_HOST $LEAF1_HOST $LEAF2_HOST $HOST0 $HOST1 $HOST2 $HOST3; do
        ssh $host "rm -f /root/*.log" 2>/dev/null || true
    done
}

# 编译和部署
build_and_deploy() {
    echo "[BUILD] Compiling..."
    cd /root/NetLab/new/INC_Emulation/repository/non_termination_switch
    mkdir -p build && cd build
    cmake .. > /dev/null 2>&1
    make switch_broadcast -j4 > /dev/null 2>&1

    echo "[DEPLOY] Deploying binaries..."
    scp switch_broadcast $SPINE_HOST:/root/ || { echo "ERROR: Failed to deploy to $SPINE_HOST"; return 1; }
    scp switch_broadcast $LEAF1_HOST:/root/ || { echo "ERROR: Failed to deploy to $LEAF1_HOST"; return 1; }
    scp switch_broadcast $LEAF2_HOST:/root/ || { echo "ERROR: Failed to deploy to $LEAF2_HOST"; return 1; }

    # 部署 controller 和 host 程序
    cd /root/NetLab/new/INC_Emulation/repository/build
    scp controller $CONTROLLER_HOST:/root/ || { echo "ERROR: Failed to deploy controller"; return 1; }
    scp host_broadcast_test_v2 $HOST0:/root/host_broadcast || { echo "ERROR: Failed to deploy to $HOST0"; return 1; }
    scp host_broadcast_test_v2 $HOST1:/root/host_broadcast || { echo "ERROR: Failed to deploy to $HOST1"; return 1; }
    scp host_broadcast_test_v2 $HOST2:/root/host_broadcast || { echo "ERROR: Failed to deploy to $HOST2"; return 1; }
    scp host_broadcast_test_v2 $HOST3:/root/host_broadcast || { echo "ERROR: Failed to deploy to $HOST3"; return 1; }
}

# 启动组件
start_components() {
    echo ""
    echo "[START] Starting components..."

    # 1. Controller
    echo "  [1/7] Controller..."
    ssh $CONTROLLER_HOST "cd /root && stdbuf -oL ./controller > /root/controller.log 2>&1" &
    sleep 2

    # 2. Spine Switch
    echo "  [2/7] Spine Switch..."
    ssh $SPINE_HOST "cd /root && stdbuf -oL ./switch_broadcast $CONTROLLER_IP 0 > /root/switch.log 2>&1" &
    sleep 2

    # 3. Leaf1 Switch
    echo "  [3/7] Leaf1 Switch (vm1)..."
    ssh $LEAF1_HOST "cd /root && stdbuf -oL ./switch_broadcast $CONTROLLER_IP 1 > /root/switch.log 2>&1" &
    sleep 2

    # 4. Leaf2 Switch
    echo "  [4/7] Leaf2 Switch (vm2)..."
    ssh $LEAF2_HOST "cd /root && stdbuf -oL ./switch_broadcast $CONTROLLER_IP 2 > /root/switch.log 2>&1" &
    sleep 3
}

# 启动 Host
start_hosts() {
    echo ""
    echo "[START] Starting hosts..."

    # Host0 (rank 0, root)
    echo "  [5/7] Host0 (pku1, rank 0, root)..."
    ssh $HOST0 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 0 0 $DATA_COUNT > /root/host.log 2>&1" &
    sleep 1

    # Host1 (rank 1)
    echo "  [6/7] Host1 (pku2, rank 1)..."
    ssh $HOST1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 1 0 $DATA_COUNT > /root/host.log 2>&1" &
    sleep 1

    # Host2, Host3
    echo "  [7/7] Host2,3 (pku3,4, rank 2,3)..."
    ssh $HOST2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 2 0 $DATA_COUNT > /root/host.log 2>&1" &
    ssh $HOST3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_broadcast $WORLD_SIZE $MASTER_IP 3 0 $DATA_COUNT > /root/host.log 2>&1" &
}

# 轮询检测完成
wait_completion() {
    echo ""
    echo "[WAIT] Waiting for completion (timeout: ${TIMEOUT}s)..."

    ELAPSED=0
    while [ $ELAPSED -lt $TIMEOUT ]; do
        sleep 2
        ELAPSED=$((ELAPSED + 2))

        H0=$(ssh $HOST0 "grep -qE 'Test Complete|PASS' /root/host.log 2>/dev/null && echo 1 || echo 0")
        H1=$(ssh $HOST1 "grep -qE 'Test Complete|PASS' /root/host.log 2>/dev/null && echo 1 || echo 0")
        H2=$(ssh $HOST2 "grep -qE 'Test Complete|PASS' /root/host.log 2>/dev/null && echo 1 || echo 0")
        H3=$(ssh $HOST3 "grep -qE 'Test Complete|PASS' /root/host.log 2>/dev/null && echo 1 || echo 0")

        if [ "$H0" = "1" ] && [ "$H1" = "1" ] && [ "$H2" = "1" ] && [ "$H3" = "1" ]; then
            echo "  All hosts completed after ${ELAPSED}s"
            return 0
        fi

        if [ $((ELAPSED % 10)) -eq 0 ]; then
            echo "  ${ELAPSED}s: H0=$H0 H1=$H1 H2=$H2 H3=$H3"
        fi
    done

    echo "  TIMEOUT after ${TIMEOUT}s"
    return 1
}

# 收集日志
collect_logs() {
    echo ""
    echo "[COLLECT] Collecting logs to $LOG_DIR..."

    # 确保回到脚本目录
    cd /root/NetLab/new/INC_Emulation/repository/non_termination_switch

    ssh $CONTROLLER_HOST "cat /root/controller.log 2>/dev/null" > "$LOG_DIR/controller.log" || true
    ssh $SPINE_HOST "cat /root/switch.log 2>/dev/null" > "$LOG_DIR/spine.log" || true
    ssh $LEAF1_HOST "cat /root/switch.log 2>/dev/null" > "$LOG_DIR/leaf1.log" || true
    ssh $LEAF2_HOST "cat /root/switch.log 2>/dev/null" > "$LOG_DIR/leaf2.log" || true
    ssh $HOST0 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host0.log" || true
    ssh $HOST1 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host1.log" || true
    ssh $HOST2 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host2.log" || true
    ssh $HOST3 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host3.log" || true
}

# 显示结果
show_results() {
    echo ""
    echo "=========================================="
    echo "  Test Results"
    echo "=========================================="

    for i in 0 1 2 3; do
        echo ""
        echo "--- Host$i ---"
        tail -10 "$LOG_DIR/host$i.log" 2>/dev/null || echo "(no log)"
    done
}

# 主流程
main() {
    stop_all
    clear_logs
    build_and_deploy
    start_components
    start_hosts

    if wait_completion; then
        RESULT="PASS"
    else
        RESULT="FAIL"
    fi

    stop_all
    collect_logs
    show_results

    echo ""
    echo "=========================================="
    echo "  Final Result: $RESULT"
    echo "  Logs: $LOG_DIR"
    echo "=========================================="
}

main
