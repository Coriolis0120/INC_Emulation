#!/bin/bash
# Multi-size AllReduce Test Script
# 从 4KB 到 1GB，每次 4 倍增，每个数据量测试 10 次

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
MAIN_BUILD_DIR="$SCRIPT_DIR/../build"
LOG_BASE_DIR="$SCRIPT_DIR/logs/multi_size_$(date +%Y%m%d_%H%M%S)"

WORLD_SIZE=4
CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"

ITERATIONS=10

mkdir -p "$LOG_BASE_DIR"

# 数据量列表 (int 数量): 4KB, 16KB, 64KB, 256KB, 1MB, 4MB, 16MB, 64MB, 256MB, 1GB
# 4KB = 1024 ints, 16KB = 4096 ints, ...
DATA_COUNTS=(1024 4096 16384 65536 262144 1048576 4194304 16777216 67108864 268435456)
DATA_NAMES=("4KB" "16KB" "64KB" "256KB" "1MB" "4MB" "16MB" "64MB" "256MB" "1GB")
TIMEOUTS=(20 20 20 30 30 60 60 120 180 300)

echo "=========================================="
echo "Multi-Size AllReduce Test"
echo "Iterations per size: $ITERATIONS"
echo "Log dir: $LOG_BASE_DIR"
echo "=========================================="

# ============================================
# 编译和部署
# ============================================
echo ""
echo "[Compiling and deploying...]"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. > /dev/null 2>&1
make -j4 2>&1 | tail -5

cd "$MAIN_BUILD_DIR"
make host_simple_test_v2 -j4 2>&1 | tail -3

for host in switch vm1 vm2; do
    scp "$BUILD_DIR/switch_reduce" "$host:/root/" &
done
wait
for host in pku1 pku2 pku3 pku4; do
    scp "$MAIN_BUILD_DIR/host_simple_test_v2" "$host:/root/" &
done
wait
echo "Deployment complete."

# ============================================
# 单次测试函数
# ============================================
run_single_test() {
    local DATA_COUNT=$1
    local TIMEOUT=$2
    local LOG_DIR=$3

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

    # 返回结果
    if [ $COMPLETED -eq 1 ]; then
        local THROUGHPUT=$(grep -oP 'Throughput:\s*\K[0-9.]+' "$LOG_DIR/host0.log" | tail -1)
        local TIME_MS=$(grep -oP 'Time:\s*\K[0-9]+' "$LOG_DIR/host0.log" | tail -1)
        echo "$THROUGHPUT $TIME_MS"
    else
        echo "FAIL 0"
    fi
}

# ============================================
# 结果存储
# ============================================
declare -A RESULTS

# ============================================
# 运行测试
# ============================================
for idx in "${!DATA_COUNTS[@]}"; do
    DATA_COUNT=${DATA_COUNTS[$idx]}
    DATA_NAME=${DATA_NAMES[$idx]}
    TIMEOUT=${TIMEOUTS[$idx]}

    echo ""
    echo "=========================================="
    echo "Testing $DATA_NAME (timeout: ${TIMEOUT}s)"
    echo "=========================================="

    SIZE_LOG_DIR="$LOG_BASE_DIR/$DATA_NAME"
    mkdir -p "$SIZE_LOG_DIR"

    declare -a SIZE_THROUGHPUTS
    declare -a SIZE_TIMES
    PASS=0
    FAIL=0

    for iter in $(seq 1 $ITERATIONS); do
        ITER_LOG_DIR="$SIZE_LOG_DIR/iter_$iter"
        mkdir -p "$ITER_LOG_DIR"

        RESULT=$(run_single_test $DATA_COUNT $TIMEOUT "$ITER_LOG_DIR")
        TP=$(echo $RESULT | awk '{print $1}')
        TM=$(echo $RESULT | awk '{print $2}')

        if [ "$TP" != "FAIL" ] && [ -n "$TP" ]; then
            echo "  Iter $iter: ${TP} Mbps (${TM} ms)"
            SIZE_THROUGHPUTS+=("$TP")
            SIZE_TIMES+=("$TM")
            ((PASS++))
        else
            echo "  Iter $iter: FAIL"
            ((FAIL++))
        fi
    done

    # 计算统计
    if [ ${#SIZE_THROUGHPUTS[@]} -gt 0 ]; then
        SUM=0
        for tp in "${SIZE_THROUGHPUTS[@]}"; do
            SUM=$(echo "$SUM + $tp" | bc)
        done
        AVG=$(echo "scale=2; $SUM / ${#SIZE_THROUGHPUTS[@]}" | bc)

        TIME_SUM=0
        for t in "${SIZE_TIMES[@]}"; do
            TIME_SUM=$((TIME_SUM + t))
        done
        TIME_AVG=$((TIME_SUM / ${#SIZE_TIMES[@]}))

        RESULTS[$DATA_NAME]="$AVG $TIME_AVG $PASS $FAIL"
        echo "  --- $DATA_NAME Summary: Avg ${AVG} Mbps, Avg Time ${TIME_AVG} ms, Pass $PASS, Fail $FAIL ---"
    else
        RESULTS[$DATA_NAME]="0 0 0 $ITERATIONS"
        echo "  --- $DATA_NAME Summary: All tests failed ---"
    fi

    unset SIZE_THROUGHPUTS
    unset SIZE_TIMES
done

# ============================================
# 最终汇总
# ============================================
echo ""
echo "=========================================="
echo "FINAL SUMMARY"
echo "=========================================="
printf "%-10s %12s %12s %8s %8s\n" "Size" "Throughput" "Time(ms)" "Pass" "Fail"
printf "%-10s %12s %12s %8s %8s\n" "----" "----------" "--------" "----" "----"

for DATA_NAME in "${DATA_NAMES[@]}"; do
    if [ -n "${RESULTS[$DATA_NAME]}" ]; then
        read AVG TIME_AVG PASS FAIL <<< "${RESULTS[$DATA_NAME]}"
        printf "%-10s %10s Mbps %10s ms %8s %8s\n" "$DATA_NAME" "$AVG" "$TIME_AVG" "$PASS" "$FAIL"
    fi
done

echo ""
echo "Logs saved to: $LOG_BASE_DIR"
echo "=========================================="
