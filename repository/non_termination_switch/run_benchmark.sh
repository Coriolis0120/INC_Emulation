#!/bin/bash
# Simple benchmark - runs tests sequentially with output
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

CONTROLLER_IP="192.168.0.3"
MASTER_IP="192.168.0.14"

# Test sizes
declare -a SIZES=(1024 4096 16384 65536 262144 1048576 4194304 16777216 67108864 268435456)
declare -a NAMES=("4KB" "16KB" "64KB" "256KB" "1MB" "4MB" "16MB" "64MB" "256MB" "1GB")
declare -a TIMEOUTS=(30 30 30 30 60 60 120 180 300 600)

ITERATIONS=10
OUTFILE="$SCRIPT_DIR/benchmark_$(date +%Y%m%d_%H%M%S).csv"

echo "Size,Iter,Time_ms,Throughput_Mbps,Status" > "$OUTFILE"

kill_all() {
    for h in controller switch vm1 vm2 pku1 pku2 pku3 pku4; do
        ssh $h "pkill -9 switch_reduce; pkill -9 controller; pkill -9 host_reduce" 2>/dev/null &
    done
    wait
    sleep 2
}

run_test() {
    local SIZE=$1
    local TIMEOUT=$2

    kill_all >/dev/null 2>&1

    ssh controller "cd /root && ./controller >/dev/null 2>&1" &
    sleep 2
    ssh switch "cd /root && ./switch_reduce $CONTROLLER_IP 0 >/dev/null 2>&1" &
    sleep 1
    ssh vm1 "cd /root && ./switch_reduce $CONTROLLER_IP 1 >/dev/null 2>&1" &
    sleep 1
    ssh vm2 "cd /root && ./switch_reduce $CONTROLLER_IP 2 >/dev/null 2>&1" &
    sleep 2

    ssh pku1 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 4 $MASTER_IP 0 0 $SIZE >/root/host.log 2>&1" &
    sleep 2
    ssh pku2 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 4 $MASTER_IP 1 0 $SIZE >/root/host.log 2>&1" &
    ssh pku3 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 4 $MASTER_IP 2 0 $SIZE >/root/host.log 2>&1" &
    ssh pku4 "cd /root && export CONTROLLER_IP='$CONTROLLER_IP' && ./host_reduce_test_v2 4 $MASTER_IP 3 0 $SIZE >/root/host.log 2>&1" &

    local E=0
    while [ $E -lt $TIMEOUT ]; do
        sleep 2
        E=$((E+2))
        local D=$(ssh pku1 "grep -c 'Test Complete' /root/host.log" 2>/dev/null || echo 0)
        [ "$D" -ge 1 ] && break
    done

    ssh pku1 "grep 'Reduce completed' /root/host.log | tail -1" 2>/dev/null
}

echo "=== INC Reduce Benchmark ==="
echo "Output: $OUTFILE"
echo ""

for i in "${!SIZES[@]}"; do
    NAME=${NAMES[$i]}
    SIZE=${SIZES[$i]}
    TIMEOUT=${TIMEOUTS[$i]}

    echo "Testing $NAME..."
    TOTAL=0
    PASS=0

    for iter in $(seq 1 $ITERATIONS); do
        RES=$(run_test $SIZE $TIMEOUT)
        TM=$(echo "$RES" | grep -oP 'Time: \K[0-9]+' || echo "")
        TP=$(echo "$RES" | grep -oP 'Throughput: \K[0-9.]+' || echo "")

        if [ -n "$TP" ] && [ "$TP" != "0" ]; then
            STATUS="PASS"
            TOTAL=$(echo "$TOTAL + $TP" | bc)
            PASS=$((PASS+1))
            echo "  #$iter: ${TM}ms ${TP}Mbps"
        else
            STATUS="FAIL"
            TM="0"
            TP="0"
            echo "  #$iter: FAIL"
        fi
        echo "$NAME,$iter,$TM,$TP,$STATUS" >> "$OUTFILE"
    done

    if [ $PASS -gt 0 ]; then
        AVG=$(echo "scale=2; $TOTAL / $PASS" | bc)
    else
        AVG="N/A"
    fi
    echo "  => $NAME AVG: $AVG Mbps ($PASS/$ITERATIONS)"
    echo ""
done

kill_all >/dev/null 2>&1
echo "Done! Results in $OUTFILE"
