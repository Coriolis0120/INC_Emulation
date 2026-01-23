#!/bin/bash
# Manual test script - start components step by step
./stop_inc.sh
echo "Building and transporting binaries..."
./build_and_transport.sh

echo "=== Step 1: Starting Controller ==="
ssh controller "cd /root && ./controller > /root/controller.log" &
sleep 2

echo ""
echo "=== Step 2: Starting Switch ==="
ssh switch "cd /root && ./switch_v2 > /root/switch.log" &
sleep 2

echo ""
echo "=== Step 3: Starting VM1 (rank 0) ==="
ssh vm1 "cd /root && export CONTROLLER_IP='192.168.0.3' && ./host_multi_ops 2 192.168.0.6 0 > /root/vm1.log" &
sleep 2

echo ""
echo "=== Step 4: Starting VM2 (rank 1) ==="
ssh vm2 "cd /root && export CONTROLLER_IP='192.168.0.3' && ./host_multi_ops 2 192.168.0.6 1 > /root/vm2.log" &

echo ""
echo "=== Waiting for completion (30 seconds) ==="
sleep 10

echo ""
echo "=== Cleaning up ==="
./stop_inc.sh
echo "All components stopped."


echo "logs:"
echo "---- Controller Log ----" > ./logs/controller.log
ssh controller "cat /root/controller.log" >> ./logs/controller.log


echo "---- Switch Log ----" > ./logs/switch.log
ssh switch "cat /root/switch.log" >> ./logs/switch.log

echo "---- VM1 Log ----" > ./logs/vm1.log
ssh vm1 "cat /root/vm1.log" >> ./logs/vm1.log

echo "---- VM2 Log ----" > ./logs/vm2.log
ssh vm2 "cat /root/vm2.log" >> ./logs/vm2.log

sleep 5

cat ./logs/controller.log
cat ./logs/switch.log
cat ./logs/vm1.log
cat ./logs/vm2.log