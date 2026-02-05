#!/bin/bash
# Throughput Benchmark Script - with progress reporting
# Tests from 4KB to 1GB, 4x increments, 10 iterations each

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
MAIN_BUILD_DIR="$SCRIPT_DIR/../build"

WORLD_SIZE=4
CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"
ROOT_RANK=0
ITERATIONS=10

# Data sizes: 4KB, 16KB, 64KB, 256KB, 1MB, 4MB, 16MB, 64MB, 256MB, 1GB
declare -a SIZES=(1024 4096 16384 65536 262144 1048576 4194304 16777216 67108864 268435456)
declare -a SIZE_NAMES=("4KB" "16KB" "64KB" "256KB" "1MB" "4MB" "16MB" "64MB" "256MB" "1GB")
declare -a TIMEOUTS=(30 30 30 30 60 60 120 180 300 600)

RESULT_FILE="$SCRIPT_DIR/benchmark_results.txt"
PROGRESS_FILE="$SCRIPT_DIR/benchmark_progress.txt"

# Initialize files
echo "INC Reduce Throughput Benchmark" > "$RESULT_FILE"
echo "Date: $(date)" >> "$RESULT_FILE"
echo "Size,Iteration,Time(ms),Throughput(Mbps),Status" >> "$RESULT_FILE"

echo "BENCHMARK STARTED: $(date)" > "$PROGRESS_FILE"

# Function to kill all processes
kill_all() {
    for host in controller switch vm1 vm2; do
        ssh $host "pkill -9 switch_reduce; pkill -9 controller" 2>/dev/null &
    done
    for host in pku1 pku2 pku3 pku4; do
        ssh $host "pkill -9 host_reduce_test_v2" 2>/dev/null &
    done
    wait
    sleep 2
}

# Function to run single test
run_test() {
    local DATA_COUNT=$1
    local TIMEOUT=$2

    ssh controller "cd /root && ./controller > /root/controller.log 2>&1" &
    sleep 2
    ssh switch "cd /root && ./switch_reduce $CONTROLLER_IP 0 > /root/switch.log 2>&1" &
    sleep 1
    ssh vm1 "cd /root && ./switch_reduce $CONTROLLER_IP 1 > /root/switch.log 2>&1" &
    sleep 1
    ssh vm2 "cd /root && ./switch_reduce $CONTROLLER_IP 2 > /root/switch.log 2>&1" &
    sleep 2

    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 0 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &
    sleep 2
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 1 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 2 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 $WORLD_SIZE $MASTER_IP 3 $ROOT_RANK $DATA_COUNT > /root/host.log 2>&1" &

    local ELAPSED=0
    while [ $ELAPSED -lt $TIMEOUT ]; do
        sleep 2
        ELAPSED=$((ELAPSED + 2))
        local H0=$(ssh pku1 "grep -c 'Test Complete' /root/host.log 2>/dev/null" 2>/dev/null || echo 0)
        local H1=$(ssh pku2 "grep -c 'Test Complete' /root/host.log 2>/dev/null" 2>/dev/null || echo 0)
        local H2=$(ssh pku3 "grep -c 'Test Complete' /root/host.log 2>/dev/null" 2>/dev/null || echo 0)
        local H3=$(ssh pku4 "grep -c 'Test Complete' /root/host.log 2>/dev/null" 2>/dev/null || echo 0)
        if [ "$H0" -ge 1 ] && [ "$H1" -ge 1 ] && [ "$H2" -ge 1 ] && [ "$H3" -ge 1 ]; then
            break
        fi
    done

    ssh pku1 "grep 'Reduce completed' /root/host.log | tail -1; grep -E 'PASS|FAIL' /root/host.log | tail -1" 2>/dev/null
}

# Summary arrays
declare -a AVG_THROUGHPUTS
declare -a PASS_COUNTS

# Main loop
for i in "${!SIZES[@]}"; do
    SIZE=${SIZES[$i]}
    SIZE_NAME=${SIZE_NAMES[$i]}
    TIMEOUT=${TIMEOUTS[$i]}

    echo "" >> "$PROGRESS_FILE"
    echo "=== Testing $SIZE_NAME ($(($i+1))/10) ===" >> "$PROGRESS_FILE"

    TOTAL_THROUGHPUT=0
    PASS_COUNT=0

    for iter in $(seq 1 $ITERATIONS); do
        echo "  $SIZE_NAME iter $iter/$ITERATIONS - $(date +%H:%M:%S)" >> "$PROGRESS_FILE"

        kill_all > /dev/null 2>&1
        RESULT=$(run_test $SIZE $TIMEOUT)
        kill_all > /dev/null 2>&1

        TIME=$(echo "$RESULT" | grep -oP 'Time: \K[0-9]+')
        THROUGHPUT=$(echo "$RESULT" | grep -oP 'Throughput: \K[0-9.]+')
        STATUS="FAIL"
        if echo "$RESULT" | grep -q "PASS"; then
            STATUS="PASS"
            PASS_COUNT=$((PASS_COUNT + 1))
            if [ -n "$THROUGHPUT" ]; then
                TOTAL_THROUGHPUT=$(echo "$TOTAL_THROUGHPUT + $THROUGHPUT" | bc)
            fi
        fi

        echo "$SIZE_NAME,$iter,$TIME,$THROUGHPUT,$STATUS" >> "$RESULT_FILE"
        echo "    => $STATUS ${TIME}ms ${THROUGHPUT}Mbps" >> "$PROGRESS_FILE"
    done

    if [ $PASS_COUNT -gt 0 ]; then
        AVG=$(echo "scale=2; $TOTAL_THROUGHPUT / $PASS_COUNT" | bc)
    else
        AVG="N/A"
    fi
    AVG_THROUGHPUTS[$i]=$AVG
    PASS_COUNTS[$i]=$PASS_COUNT

    echo "  => $SIZE_NAME AVG: $AVG Mbps ($PASS_COUNT/$ITERATIONS)" >> "$PROGRESS_FILE"
    echo "$SIZE_NAME,AVG,,$AVG,$PASS_COUNT/$ITERATIONS" >> "$RESULT_FILE"
done

# Final summary
echo "" >> "$PROGRESS_FILE"
echo "========== FINAL SUMMARY ==========" >> "$PROGRESS_FILE"
for i in "${!SIZES[@]}"; do
    echo "${SIZE_NAMES[$i]}: ${AVG_THROUGHPUTS[$i]} Mbps (${PASS_COUNTS[$i]}/$ITERATIONS)" >> "$PROGRESS_FILE"
done
echo "COMPLETED: $(date)" >> "$PROGRESS_FILE"
