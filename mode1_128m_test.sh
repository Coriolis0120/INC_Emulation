#!/bin/bash
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
# 128MB = 33554432 int32
COUNT=33554432

cleanup() {
    ssh $SPINE_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF1_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh $LEAF2_HOST "pkill -9 -f switch_mode1" 2>/dev/null
    ssh pku1 "pkill -9 -f host_mode1" 2>/dev/null
    ssh pku2 "pkill -9 -f host_mode1" 2>/dev/null
    ssh pku3 "pkill -9 -f host_mode1" 2>/dev/null
    ssh pku4 "pkill -9 -f host_mode1" 2>/dev/null
}
trap cleanup EXIT

echo "=== 128MB Test ==="
cleanup
sleep 2

# Deploy
scp ./repository/build/switch_mode1 $SPINE_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF1_HOST:/root/ 2>/dev/null
scp ./repository/build/switch_mode1 $LEAF2_HOST:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku1:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku2:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku3:/root/ 2>/dev/null
scp ./repository/build/host_mode1 pku4:/root/ 2>/dev/null

# Start switches
ssh $SPINE_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -r -c 2 -d rxe_eth1,rxe_eth2 > /root/switch_mode1.log 2>&1" &
sleep 3
ssh $LEAF1_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF1 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 2
ssh $LEAF2_HOST "cd /root && ./switch_mode1 -p $SWITCH_PORT -g $GID_IDX -n 2 -s $SPINE_DATA_IP_FOR_LEAF2 -u rxe_eth1 > /root/switch_mode1.log 2>&1" &
sleep 3

echo "Switches started, running 128MB test..."

# Run hosts
ssh pku1 "cd /root && timeout 120 ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 0 $WORLD_SIZE $COUNT" > /tmp/pku1.log 2>&1 &
ssh pku2 "cd /root && timeout 120 ./host_mode1 $LEAF1_DATA_IP $SWITCH_PORT 1 $WORLD_SIZE $COUNT" > /tmp/pku2.log 2>&1 &
ssh pku3 "cd /root && timeout 120 ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 2 $WORLD_SIZE $COUNT" > /tmp/pku3.log 2>&1 &
ssh pku4 "cd /root && timeout 120 ./host_mode1 $LEAF2_DATA_IP $SWITCH_PORT 3 $WORLD_SIZE $COUNT" > /tmp/pku4.log 2>&1 &
wait

echo ""
echo "=== Results ==="
for h in pku1 pku2 pku3 pku4; do
    echo "--- $h ---"
    grep -E "(PASS|FAIL|AllReduce time|ERROR|timeout)" /tmp/$h.log 2>/dev/null || echo "No result"
done
