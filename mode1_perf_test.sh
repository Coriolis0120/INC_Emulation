#!/bin/bash
# Mode1 Performance Test Script
# 1-2-4 拓扑: 1 Spine + 2 Leaf + 4 Host
#
# 用法: ./mode1_perf_test.sh [TEST_SIZES] [RUNS] [TIMEOUT] [PORT] [GID]
# 示例:
#   ./mode1_perf_test.sh                              # 默认测试 16MB 和 64MB，每个跑10次
#   ./mode1_perf_test.sh "33554432" 5                 # 测试 128MB，每个跑5次
#   ./mode1_perf_test.sh "16777216 33554432" 10 600   # 测试 64MB 和 128MB，每个跑10次，超时600秒
#
# 常用数据量 (int32 count):
#   1048576   = 4MB
#   4194304   = 16MB
#   16777216  = 64MB
#   33554432  = 128MB
#   67108864  = 256MB
#   134217728 = 512MB
#   268435456 = 1GB

# 参数解析
TEST_SIZES="${1:-4194304 16777216}"
NUM_RUNS=${2:-10}
SCRIPT_TIMEOUT=${3:-3600}
SWITCH_PORT=${4:-52410}
GID_IDX=${5:-1}

# 拓扑配置
SPINE_HOST="switch"
LEAF1_HOST="vm1"
LEAF2_HOST="vm2"

LEAF1_DATA_IP="10.1.1.25"
LEAF2_DATA_IP="10.2.1.18"
SPINE_DATA_IP_FOR_LEAF1="172.16.0.21"
SPINE_DATA_IP_FOR_LEAF2="10.10.0.14"

WORLD_SIZE=4
SCRIPT_START=$(date +%s)

# 清理函数
cleanup() {
    echo ""
    echo "=== Cleanup ==="
    ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh pku1 "pkill -9 -f host_mode1" 2>/dev/null
    ssh pku2 "pkill -9 -f host_mode1" 2>/dev/null
    ssh pku3 "pkill -9 -f host_mode1" 2>/dev/null
    ssh pku4 "pkill -9 -f host_mode1" 2>/dev/null

    # 收集日志
    mkdir -p ./logs/mode1_perf
    ssh $SPINE_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1_perf/spine.log
    ssh $LEAF1_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1_perf/leaf1.log
    ssh $LEAF2_HOST "cat /root/switch_mode1.log 2>/dev/null" > ./logs/mode1_perf/leaf2.log
    echo "Logs saved to ./logs/mode1_perf/"
}

# 检查超时
check_timeout() {
    local now=$(date +%s)
    local elapsed=$((now - SCRIPT_START))
    if [ $elapsed -ge $SCRIPT_TIMEOUT ]; then
        echo ""
        echo "!!! SCRIPT TIMEOUT after ${elapsed}s !!!"
        cleanup
        exit 1
    fi
}

# 启动 Switches
start_switches() {
    ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    sleep 1

    ssh $SPINE_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -r -c 2 -d rxe_eth1,rxe_eth2 > /root/switch_mode1.log 2>&1" &
    sleep 2
    ssh $LEAF1_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF1 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
    sleep 1
    ssh $LEAF2_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF2 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
    sleep 2
}

# 运行单次测试，返回吞吐量（Mbps）或空
run_single_test() {
    local COUNT=$1
    local BYTES=$((COUNT * 4))
    local HOST_TIMEOUT=$2

    # 运行测试
    ssh pku1 "cd /root && timeout $HOST_TIMEOUT ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE $COUNT" > /tmp/pku1.log 2>&1 &
    local PID1=$!
    ssh pku2 "cd /root && timeout $HOST_TIMEOUT ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE $COUNT" > /tmp/pku2.log 2>&1 &
    local PID2=$!
    ssh pku3 "cd /root && timeout $HOST_TIMEOUT ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 2 $WORLD_SIZE $COUNT" > /tmp/pku3.log 2>&1 &
    local PID3=$!
    ssh pku4 "cd /root && timeout $HOST_TIMEOUT ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 3 $WORLD_SIZE $COUNT" > /tmp/pku4.log 2>&1 &
    local PID4=$!
    wait $PID1 $PID2 $PID3 $PID4

    # 提取结果
    local TIME0=$(grep "AllReduce time:" /tmp/pku1.log 2>/dev/null | awk '{print $5}')
    local TIME1=$(grep "AllReduce time:" /tmp/pku2.log 2>/dev/null | awk '{print $5}')
    local TIME2=$(grep "AllReduce time:" /tmp/pku3.log 2>/dev/null | awk '{print $5}')
    local TIME3=$(grep "AllReduce time:" /tmp/pku4.log 2>/dev/null | awk '{print $5}')

    local PASS0=$(grep -c "PASS" /tmp/pku1.log 2>/dev/null || echo 0)
    local PASS1=$(grep -c "PASS" /tmp/pku2.log 2>/dev/null || echo 0)
    local PASS2=$(grep -c "PASS" /tmp/pku3.log 2>/dev/null || echo 0)
    local PASS3=$(grep -c "PASS" /tmp/pku4.log 2>/dev/null || echo 0)

    if [ "$PASS0" = "1" ] && [ "$PASS1" = "1" ] && [ "$PASS2" = "1" ] && [ "$PASS3" = "1" ]; then
        # 计算吞吐量：取最大时间
        local MAX_TIME=$TIME0
        [ -n "$TIME1" ] && [ "$TIME1" -gt "$MAX_TIME" ] 2>/dev/null && MAX_TIME=$TIME1
        [ -n "$TIME2" ] && [ "$TIME2" -gt "$MAX_TIME" ] 2>/dev/null && MAX_TIME=$TIME2
        [ -n "$TIME3" ] && [ "$TIME3" -gt "$MAX_TIME" ] 2>/dev/null && MAX_TIME=$TIME3

        if [ "$MAX_TIME" -gt 0 ] 2>/dev/null; then
            # 吞吐量 Mbps = BYTES * 8 / MAX_TIME(us)
            awk "BEGIN {printf \"%.2f\", $BYTES * 8.0 / $MAX_TIME}"
        fi
    fi
}

# 捕获信号
trap cleanup EXIT INT TERM

echo "=== Mode1 Performance Test (Multi-Run) ==="
echo "Port: $SWITCH_PORT, GID: $GID_IDX, Timeout: ${SCRIPT_TIMEOUT}s"
echo "Test sizes: $TEST_SIZES"
echo "Runs per size: $NUM_RUNS"
echo ""

# 编译
echo "=== Building ==="
cd /root/NetLab/new/INC_Emulation/repository/build
cmake .. > /dev/null 2>&1
make switch_mode1 host_mode1 -j4 > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi
echo "Build successful"
cd /root/NetLab/new/INC_Emulation

check_timeout

# 停止旧进程
echo ""
echo "=== Stopping old processes ==="
ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
ssh pku1 "pkill -9 -f host_mode1" 2>/dev/null
ssh pku2 "pkill -9 -f host_mode1" 2>/dev/null
ssh pku3 "pkill -9 -f host_mode1" 2>/dev/null
ssh pku4 "pkill -9 -f host_mode1" 2>/dev/null
sleep 2

# 部署程序
echo ""
echo "=== Deploying binaries ==="
scp ./repository/build/switch_mode1 $SPINE_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF1_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF2_HOST:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku1:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku2:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku3:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku4:/root/ 2>/dev/null
echo "Deploy complete"

check_timeout

# 性能测试
echo ""
echo "=== Performance Tests ==="
echo ""

for COUNT in $TEST_SIZES; do
    BYTES=$((COUNT * 4))

    # 根据数据量设置超时
    if [ $BYTES -ge 536870912 ]; then
        HOST_TIMEOUT=120
    elif [ $BYTES -ge 134217728 ]; then
        HOST_TIMEOUT=60
    else
        HOST_TIMEOUT=30
    fi

    # 转换为人类可读格式
    if [ $BYTES -ge 1073741824 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1073741824}")GB"
    elif [ $BYTES -ge 1048576 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1048576}")MB"
    else
        SIZE_STR="${BYTES}B"
    fi

    echo "Testing $SIZE_STR ($BYTES bytes), $NUM_RUNS runs..."
    echo "------------------------------------------------------------------------"

    THROUGHPUTS=""
    PASS_COUNT=0
    FAIL_COUNT=0

    for RUN in $(seq 1 $NUM_RUNS); do
        check_timeout

        # 重启 Switches
        start_switches

        # 运行测试
        THROUGHPUT=$(run_single_test $COUNT $HOST_TIMEOUT)

        if [ -n "$THROUGHPUT" ]; then
            printf "  Run %2d: %s Mbps (PASS)\n" $RUN "$THROUGHPUT"
            THROUGHPUTS="$THROUGHPUTS $THROUGHPUT"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            printf "  Run %2d: FAIL\n" $RUN
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    done

    echo "------------------------------------------------------------------------"

    # 计算平均值
    if [ $PASS_COUNT -gt 0 ]; then
        AVG=$(echo $THROUGHPUTS | awk '{sum=0; for(i=1;i<=NF;i++) sum+=$i; printf "%.2f", sum/NF}')
        MIN=$(echo $THROUGHPUTS | awk '{min=$1; for(i=2;i<=NF;i++) if($i<min) min=$i; printf "%.2f", min}')
        MAX=$(echo $THROUGHPUTS | awk '{max=$1; for(i=2;i<=NF;i++) if($i>max) max=$i; printf "%.2f", max}')
        echo "  Summary: $SIZE_STR"
        echo "    Pass: $PASS_COUNT/$NUM_RUNS"
        echo "    Avg:  $AVG Mbps"
        echo "    Min:  $MIN Mbps"
        echo "    Max:  $MAX Mbps"
    else
        echo "  Summary: $SIZE_STR - ALL FAILED"
    fi
    echo ""
done

echo "=== Done ==="
