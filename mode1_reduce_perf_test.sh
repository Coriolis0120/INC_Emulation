#!/bin/bash
# Mode1 Reduce Performance Test Script
# 用法: ./mode1_reduce_perf_test.sh [TEST_SIZES] [RUNS] [ROOT_RANK]

TEST_SIZES="${1:-1024 4194304}"
NUM_RUNS=${2:-10}
ROOT_RANK=${3:-0}
SWITCH_PORT=52410
GID_IDX=1

SPINE_HOST="switch"
LEAF1_HOST="vm1"
LEAF2_HOST="vm2"
LEAF1_DATA_IP="10.1.1.25"
LEAF2_DATA_IP="10.2.1.18"
SPINE_DATA_IP_FOR_LEAF1="172.16.0.21"
SPINE_DATA_IP_FOR_LEAF2="10.10.0.14"
WORLD_SIZE=4

cleanup() {
    ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh pku1 "pkill -9 -f host_reduce_mode1" 2>/dev/null
    ssh pku2 "pkill -9 -f host_reduce_mode1" 2>/dev/null
    ssh pku3 "pkill -9 -f host_reduce_mode1" 2>/dev/null
    ssh pku4 "pkill -9 -f host_reduce_mode1" 2>/dev/null
}

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

run_single_test() {
    local COUNT=$1
    local BYTES=$((COUNT * 4))
    local HOST_TIMEOUT=$2

    ssh pku1 "cd /root && timeout $HOST_TIMEOUT ./host_reduce_mode1 $LEAF1_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE $ROOT_RANK $COUNT" > /tmp/pku1.log 2>&1 &
    local PID1=$!
    ssh pku2 "cd /root && timeout $HOST_TIMEOUT ./host_reduce_mode1 $LEAF1_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE $ROOT_RANK $COUNT" > /tmp/pku2.log 2>&1 &
    local PID2=$!
    ssh pku3 "cd /root && timeout $HOST_TIMEOUT ./host_reduce_mode1 $LEAF2_DATA_IP $SWITCH_PORT 2 $WORLD_SIZE $ROOT_RANK $COUNT" > /tmp/pku3.log 2>&1 &
    local PID3=$!
    ssh pku4 "cd /root && timeout $HOST_TIMEOUT ./host_reduce_mode1 $LEAF2_DATA_IP $SWITCH_PORT 3 $WORLD_SIZE $ROOT_RANK $COUNT" > /tmp/pku4.log 2>&1 &
    local PID4=$!
    wait $PID1 $PID2 $PID3 $PID4

    # 从 root 节点提取时间
    local TIME=$(grep "Reduce time:" /tmp/pku${ROOT_RANK}.log 2>/dev/null | tail -1 | awk '{print $(NF-1)}')
    local PASS=$(grep -c "PASS" /tmp/pku${ROOT_RANK}.log 2>/dev/null || echo 0)

    if [ "$PASS" -ge "1" ] && [ -n "$TIME" ] && [ "$TIME" -gt 0 ] 2>/dev/null; then
        awk "BEGIN {printf \"%.2f\", $BYTES * 8.0 / $TIME}"
    fi
}

trap cleanup EXIT INT TERM

echo "=== Mode1 Reduce Performance Test ==="
echo "Root rank: $ROOT_RANK, Runs: $NUM_RUNS"
echo ""

# 编译部署
cd /root/NetLab/new/INC_Emulation/repository/build
cmake .. > /dev/null 2>&1 && make switch_mode1 host_reduce_mode1 -j4 > /dev/null 2>&1
cd /root/NetLab/new/INC_Emulation

cleanup
scp ./repository/build/switch_mode1 $SPINE_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF1_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF2_HOST:/root/ 2>/dev/null
scp ./repository/build/host_reduce_mode1 pku1:/root/ 2>/dev/null
scp ./repository/build/host_reduce_mode1 pku2:/root/ 2>/dev/null
scp ./repository/build/host_reduce_mode1 pku3:/root/ 2>/dev/null
scp ./repository/build/host_reduce_mode1 pku4:/root/ 2>/dev/null

for COUNT in $TEST_SIZES; do
    BYTES=$((COUNT * 4))
    HOST_TIMEOUT=30
    [ $BYTES -ge 134217728 ] && HOST_TIMEOUT=60
    [ $BYTES -ge 536870912 ] && HOST_TIMEOUT=120

    if [ $BYTES -ge 1073741824 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1073741824}")GB"
    elif [ $BYTES -ge 1048576 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1048576}")MB"
    else
        SIZE_STR="${BYTES}B"
    fi

    echo "Testing $SIZE_STR, $NUM_RUNS runs..."
    echo "------------------------------------------------------------------------"

    THROUGHPUTS=""
    PASS_COUNT=0

    for RUN in $(seq 1 $NUM_RUNS); do
        start_switches
        THROUGHPUT=$(run_single_test $COUNT $HOST_TIMEOUT)
        if [ -n "$THROUGHPUT" ]; then
            printf "  Run %2d: %s Mbps (PASS)\n" $RUN "$THROUGHPUT"
            THROUGHPUTS="$THROUGHPUTS $THROUGHPUT"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            printf "  Run %2d: FAIL\n" $RUN
        fi
    done

    echo "------------------------------------------------------------------------"
    if [ $PASS_COUNT -gt 0 ]; then
        AVG=$(echo $THROUGHPUTS | awk '{sum=0; for(i=1;i<=NF;i++) sum+=$i; printf "%.2f", sum/NF}')
        MIN=$(echo $THROUGHPUTS | awk '{min=$1; for(i=2;i<=NF;i++) if($i<min) min=$i; printf "%.2f", min}')
        MAX=$(echo $THROUGHPUTS | awk '{max=$1; for(i=2;i<=NF;i++) if($i>max) max=$i; printf "%.2f", max}')
        echo "  Summary: $SIZE_STR - Pass: $PASS_COUNT/$NUM_RUNS, Avg: $AVG Mbps, Min: $MIN, Max: $MAX"
    else
        echo "  Summary: $SIZE_STR - ALL FAILED"
    fi
    echo ""
done

echo "=== Done ==="
