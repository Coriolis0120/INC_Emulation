#!/bin/bash
# Reduce 吞吐量测试脚本 (1-2-4 拓扑)
# 从 4KB 到 1GB，4倍增，每个数据量测试 10 次

SIZES="1024 4096 16384 65536 262144 1048576 4194304 16777216 67108864 268435456"
NUM_RUNS=10

LEAF1_IP="10.1.1.25"
LEAF2_IP="10.2.1.18"
SWITCH_PORT=52400

echo "=== Reduce Throughput Test (1-2-4 Topology) ==="
echo "Sizes: 4KB 16KB 64KB 256KB 1MB 4MB 16MB 64MB 256MB 1GB"
echo "Runs per size: $NUM_RUNS"
echo ""

for COUNT in $SIZES; do
    BYTES=$((COUNT * 4))

    if [ $BYTES -ge 1073741824 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1073741824}")GB"
        TIMEOUT=180
    elif [ $BYTES -ge 1048576 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1048576}")MB"
        TIMEOUT=120
    elif [ $BYTES -ge 1024 ]; then
        SIZE_STR="$(awk "BEGIN {printf \"%.0f\", $BYTES / 1024}")KB"
        TIMEOUT=30
    else
        SIZE_STR="${BYTES}B"
        TIMEOUT=30
    fi

    echo "=== Testing $SIZE_STR ($COUNT int32) ==="

    TIMES=""
    PASS=0

    for i in $(seq 1 $NUM_RUNS); do
        ssh pku1 "cd ~ && timeout $TIMEOUT ./host_reduce_mode1 $LEAF1_IP $SWITCH_PORT 0 4 0 $COUNT" > /tmp/h0.log 2>&1 &
        ssh pku2 "cd ~ && timeout $TIMEOUT ./host_reduce_mode1 $LEAF1_IP $SWITCH_PORT 1 4 0 $COUNT" > /tmp/h1.log 2>&1 &
        ssh pku3 "cd ~ && timeout $TIMEOUT ./host_reduce_mode1 $LEAF2_IP $SWITCH_PORT 2 4 0 $COUNT" > /tmp/h2.log 2>&1 &
        ssh pku4 "cd ~ && timeout $TIMEOUT ./host_reduce_mode1 $LEAF2_IP $SWITCH_PORT 3 4 0 $COUNT" > /tmp/h3.log 2>&1 &
        wait

        T=$(grep "Reduce time:" /tmp/h0.log 2>/dev/null | awk '{print $4}')
        R=$(grep -c "PASS" /tmp/h0.log 2>/dev/null || echo 0)

        if [ "$R" -ge 1 ] && [ -n "$T" ]; then
            echo "  Run $i: ${T} us"
            TIMES="$TIMES $T"
            PASS=$((PASS + 1))
        else
            echo "  Run $i: FAIL"
        fi
        sleep 0.3
    done

    if [ $PASS -gt 0 ]; then
        AVG=$(echo $TIMES | awk '{s=0; for(i=1;i<=NF;i++) s+=$i; printf "%.0f", s/NF}')
        THROUGHPUT=$(awk "BEGIN {printf \"%.2f\", $BYTES / $AVG}")
        echo "  Summary: Pass=$PASS/$NUM_RUNS, Avg=${AVG}us, Throughput=${THROUGHPUT}MB/s"
    else
        echo "  Summary: ALL FAILED"
    fi
    echo ""
done

echo "=== Done ==="
