#!/bin/bash
# Non-Termination Switch Test Script - Mode 1 (Reduce)
# 支持多次测试和平均吞吐量计算

# 参数
TIMEOUT=${1:-20}
DATA_COUNT=${2:-1024}
ITERATIONS=${3:-10}  # 测试次数，默认10次

# 配置
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
MAIN_BUILD_DIR="$SCRIPT_DIR/../build"
LOG_BASE_DIR="$SCRIPT_DIR/logs/$(date +%Y%m%d_%H%M%S)"

WORLD_SIZE=4
CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"  # pku1 管理网 IP

mkdir -p "$LOG_BASE_DIR"

# 数据大小计算
DATA_SIZE_BYTES=$((DATA_COUNT * 4))
if [ $DATA_SIZE_BYTES -ge $((1024*1024*1024)) ]; then
    DATA_SIZE_STR="$((DATA_SIZE_BYTES / 1024 / 1024 / 1024)) GB"
elif [ $DATA_SIZE_BYTES -ge $((1024*1024)) ]; then
    DATA_SIZE_STR="$((DATA_SIZE_BYTES / 1024 / 1024)) MB"
elif [ $DATA_SIZE_BYTES -ge 1024 ]; then
    DATA_SIZE_STR="$((DATA_SIZE_BYTES / 1024)) KB"
else
    DATA_SIZE_STR="$DATA_SIZE_BYTES B"
fi

echo "=========================================="
echo "Non-Termination Switch Test - Mode 1"
echo "Timeout: ${TIMEOUT}s"
echo "Data count: $DATA_COUNT ($DATA_SIZE_STR)"
echo "Iterations: $ITERATIONS"
echo "Log dir: $LOG_BASE_DIR"
echo "=========================================="

# 存储每次测试的吞吐量
declare -a THROUGHPUTS
declare -a TIMES
PASS_COUNT=0
FAIL_COUNT=0

# ============================================
# 步骤 1: 编译和部署（只执行一次）
# ============================================
echo ""
echo "[Step 1/2] Compiling and deploying..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. > /dev/null
make -j4

cd "$MAIN_BUILD_DIR"
make host_simple_test_v2 -j4

# 部署文件
for host in switch vm1 vm2; do
    scp "$BUILD_DIR/switch_reduce" "$host:/root/" &
done
wait
for host in pku1 pku2 pku3 pku4; do
    scp "$MAIN_BUILD_DIR/host_simple_test_v2" "$host:/root/" &
done
wait
echo "Compilation and deployment complete."

# ============================================
# 单次测试函数
# ============================================
run_single_test() {
    local ITER=$1
    local LOG_DIR="$LOG_BASE_DIR/iter_$ITER"
    mkdir -p "$LOG_DIR"

    echo ""
    echo "--- Iteration $ITER/$ITERATIONS ---"

    # 停止旧进程
    for host in controller switch vm1 vm2; do
        ssh $host "pkill -9 switch_reduce; pkill -9 controller" 2>/dev/null &
    done
    for host in pku1 pku2 pku3 pku4; do
        ssh $host "pkill -9 host_simple_test_v2" 2>/dev/null &
    done
    wait
    sleep 1

    # 启动程序
    ssh controller "cd /root && stdbuf -oL ./controller > /root/controller.log 2>&1" &
    sleep 2

    ssh switch "cd /root && stdbuf -oL ./switch_reduce $CONTROLLER_IP 0 > /root/switch.log 2>&1" &
    sleep 1

    ssh vm1 "cd /root && stdbuf -oL ./switch_reduce $CONTROLLER_IP 1 > /root/switch.log 2>&1" &
    sleep 1

    ssh vm2 "cd /root && stdbuf -oL ./switch_reduce $CONTROLLER_IP 2 > /root/switch.log 2>&1" &
    sleep 2

    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test_v2 $WORLD_SIZE $MASTER_IP 0 $DATA_COUNT > /root/host.log 2>&1" &
    sleep 1
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test_v2 $WORLD_SIZE $MASTER_IP 1 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test_v2 $WORLD_SIZE $MASTER_IP 2 $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && stdbuf -oL ./host_simple_test_v2 $WORLD_SIZE $MASTER_IP 3 $DATA_COUNT > /root/host.log 2>&1" &

    # 等待完成
    local ELAPSED=0
    local COMPLETED=0
    while [ $ELAPSED -lt $TIMEOUT ]; do
        sleep 2
        ELAPSED=$((ELAPSED + 2))

        H0=$(ssh pku1 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")
        H1=$(ssh pku2 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")
        H2=$(ssh pku3 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")
        H3=$(ssh pku4 "grep -q 'Test Complete' /root/host.log 2>/dev/null && echo 1 || echo 0")

        if [ "$H0" = "1" ] && [ "$H1" = "1" ] && [ "$H2" = "1" ] && [ "$H3" = "1" ]; then
            COMPLETED=1
            break
        fi
    done

    # 停止进程
    jobs -p | xargs -r kill -9 2>/dev/null
    for host in controller switch vm1 vm2; do
        ssh $host "pkill -9 switch_reduce; pkill -9 controller" 2>/dev/null
    done
    for host in pku1 pku2 pku3 pku4; do
        ssh $host "pkill -9 host_simple_test_v2" 2>/dev/null
    done

    # 收集日志
    ssh pku1 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host0.log"
    ssh pku2 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host1.log"
    ssh pku3 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host2.log"
    ssh pku4 "cat /root/host.log 2>/dev/null" > "$LOG_DIR/host3.log"

    # 提取吞吐量 (从 host0.log 提取)
    local THROUGHPUT=""
    local TIME_MS=""
    if [ $COMPLETED -eq 1 ]; then
        # 尝试提取 Throughput 行
        THROUGHPUT=$(grep -oP 'Throughput:\s*\K[0-9.]+' "$LOG_DIR/host0.log" | tail -1)
        TIME_MS=$(grep -oP 'Time:\s*\K[0-9]+' "$LOG_DIR/host0.log" | tail -1)

        if [ -n "$THROUGHPUT" ]; then
            echo "  PASS - Time: ${TIME_MS}ms, Throughput: ${THROUGHPUT} Mbps"
            THROUGHPUTS+=("$THROUGHPUT")
            TIMES+=("$TIME_MS")
            ((PASS_COUNT++))
        else
            echo "  PASS - but could not extract throughput"
            ((PASS_COUNT++))
        fi
    else
        echo "  FAIL - Timeout"
        ((FAIL_COUNT++))
    fi
}

# ============================================
# 步骤 2: 运行多次测试
# ============================================
echo ""
echo "[Step 2/2] Running $ITERATIONS iterations..."

for i in $(seq 1 $ITERATIONS); do
    run_single_test $i
done

# ============================================
# 计算并打印统计结果
# ============================================
echo ""
echo "=========================================="
echo "TEST SUMMARY"
echo "=========================================="
echo "Data size: $DATA_SIZE_STR"
echo "Iterations: $ITERATIONS"
echo "Passed: $PASS_COUNT"
echo "Failed: $FAIL_COUNT"
echo ""

if [ ${#THROUGHPUTS[@]} -gt 0 ]; then
    # 计算平均吞吐量
    SUM=0
    for tp in "${THROUGHPUTS[@]}"; do
        SUM=$(echo "$SUM + $tp" | bc)
    done
    AVG=$(echo "scale=2; $SUM / ${#THROUGHPUTS[@]}" | bc)

    # 计算平均时间
    TIME_SUM=0
    for t in "${TIMES[@]}"; do
        TIME_SUM=$((TIME_SUM + t))
    done
    TIME_AVG=$((TIME_SUM / ${#TIMES[@]}))

    # 找最大最小值
    MAX_TP=$(printf '%s\n' "${THROUGHPUTS[@]}" | sort -rn | head -1)
    MIN_TP=$(printf '%s\n' "${THROUGHPUTS[@]}" | sort -n | head -1)

    echo "Throughput Statistics:"
    echo "  Average: $AVG Mbps"
    echo "  Max:     $MAX_TP Mbps"
    echo "  Min:     $MIN_TP Mbps"
    echo ""
    echo "Time Statistics:"
    echo "  Average: ${TIME_AVG} ms"
    echo ""
    echo "Individual Results:"
    for i in "${!THROUGHPUTS[@]}"; do
        echo "  Iter $((i+1)): ${THROUGHPUTS[$i]} Mbps (${TIMES[$i]} ms)"
    done
else
    echo "No successful tests to calculate statistics."
fi

echo ""
echo "Logs saved to: $LOG_BASE_DIR"
echo "=========================================="
