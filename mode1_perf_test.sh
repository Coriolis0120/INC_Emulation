#!/bin/bash
# Mode1 Performance Test Script
# 1-2-4 拓扑: 1 Spine + 2 Leaf + 4 Host

SWITCH_PORT=${1:-52410}
GID_IDX=${2:-1}
SCRIPT_TIMEOUT=${3:-120}  # 脚本总超时时间（秒）

# 拓扑配置
SPINE_HOST="switch"
LEAF1_HOST="vm1"
LEAF2_HOST="vm2"

LEAF1_DATA_IP="10.1.1.25"
LEAF2_DATA_IP="10.2.1.18"
SPINE_DATA_IP_FOR_LEAF1="172.16.0.21"
SPINE_DATA_IP_FOR_LEAF2="10.10.0.14"

WORLD_SIZE=4
# 4KB到16MB，4倍递增: 4KB=1024, 16KB=4096, 64KB=16384, 256KB=65536, 1MB=262144, 4MB=1048576, 16MB=4194304
TEST_SIZES="4194304 16777216 67108864"
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

# 捕获信号
trap cleanup EXIT INT TERM

echo "=== Mode1 Performance Test ==="
echo "Port: $SWITCH_PORT, GID: $GID_IDX, Timeout: ${SCRIPT_TIMEOUT}s"
echo "Test sizes: $TEST_SIZES"
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

# 启动 Switches
echo ""
echo "=== Starting Switches ==="

echo "[1/3] Starting Spine Switch..."
ssh $SPINE_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -r -c 2 -d rxe_eth1,rxe_eth2 > /root/switch_mode1.log 2>&1" &
sleep 10  # 增加等待时间，让 ROOT 完成大缓冲区分配

echo "[2/3] Starting Leaf1 Switch..."
ssh $LEAF1_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF1 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 2

echo "[3/3] Starting Leaf2 Switch..."
ssh $LEAF2_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF2 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 3

echo "All switches started"

check_timeout

# 性能测试
echo ""
echo "=== Performance Tests ==="
echo ""
printf "%-10s | %-9s | %-9s | %-9s | %-9s | %-12s | %s\n" "Size(B)" "Host0(us)" "Host1(us)" "Host2(us)" "Host3(us)" "Throughput" "Status"
echo "-----------|-----------|-----------|-----------|-----------|--------------|-------"

for COUNT in $TEST_SIZES; do
    check_timeout
    BYTES=$((COUNT * 4))

    # 每次测试前重启 Switches（确保 host_count 正确）
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

    # 运行测试（每个 host 30秒超时）
    ssh pku1 "cd /root && timeout 30 ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE $COUNT" > /tmp/pku1.log 2>&1 &
    PID1=$!
    ssh pku2 "cd /root && timeout 30 ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE $COUNT" > /tmp/pku2.log 2>&1 &
    PID2=$!
    ssh pku3 "cd /root && timeout 30 ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 2 $WORLD_SIZE $COUNT" > /tmp/pku3.log 2>&1 &
    PID3=$!
    ssh pku4 "cd /root && timeout 30 ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 3 $WORLD_SIZE $COUNT" > /tmp/pku4.log 2>&1 &
    PID4=$!
    wait $PID1 $PID2 $PID3 $PID4

    # 提取结果
    TIME0=$(grep "AllReduce time:" /tmp/pku1.log 2>/dev/null | awk '{print $5}')
    TIME1=$(grep "AllReduce time:" /tmp/pku2.log 2>/dev/null | awk '{print $5}')
    TIME2=$(grep "AllReduce time:" /tmp/pku3.log 2>/dev/null | awk '{print $5}')
    TIME3=$(grep "AllReduce time:" /tmp/pku4.log 2>/dev/null | awk '{print $5}')

    PASS0=$(grep -c "PASS" /tmp/pku1.log 2>/dev/null || echo 0)
    PASS1=$(grep -c "PASS" /tmp/pku2.log 2>/dev/null || echo 0)
    PASS2=$(grep -c "PASS" /tmp/pku3.log 2>/dev/null || echo 0)
    PASS3=$(grep -c "PASS" /tmp/pku4.log 2>/dev/null || echo 0)

    if [ "$PASS0" = "1" ] && [ "$PASS1" = "1" ] && [ "$PASS2" = "1" ] && [ "$PASS3" = "1" ]; then
        STATUS="PASS"
        # 计算吞吐量：取最大时间（最慢的节点决定整体完成时间）
        MAX_TIME=$TIME0
        [ -n "$TIME1" ] && [ "$TIME1" -gt "$MAX_TIME" ] 2>/dev/null && MAX_TIME=$TIME1
        [ -n "$TIME2" ] && [ "$TIME2" -gt "$MAX_TIME" ] 2>/dev/null && MAX_TIME=$TIME2
        [ -n "$TIME3" ] && [ "$TIME3" -gt "$MAX_TIME" ] 2>/dev/null && MAX_TIME=$TIME3
        # 吞吐量 = 数据量(MB) / 时间(s) = BYTES / MAX_TIME * 1000000 / 1048576 MB/s
        if [ "$MAX_TIME" -gt 0 ] 2>/dev/null; then
            # 吞吐量 Mbps = BYTES * 8 / MAX_TIME(us) * 1000000 / 1000000 = BYTES * 8 / MAX_TIME
            THROUGHPUT=$(awk "BEGIN {printf \"%.2f Mbps\", $BYTES * 8.0 / $MAX_TIME}")
        else
            THROUGHPUT="N/A"
        fi
    else
        STATUS="FAIL"
        THROUGHPUT="N/A"
    fi

    printf "%-10s | %-9s | %-9s | %-9s | %-9s | %-12s | %s\n" \
           "$BYTES" "${TIME0:-N/A}" "${TIME1:-N/A}" "${TIME2:-N/A}" "${TIME3:-N/A}" "$THROUGHPUT" "$STATUS"
done

echo ""
echo "=== Done ==="
