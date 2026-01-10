#!/bin/bash
# Manual test script - start components step by step

echo "=== Step 1: Starting Controller ==="
ssh controller "cd /root && ./controller 2>&1" &
CONTROLLER_PID=$!
sleep 3

echo ""
echo "=== Step 2: Starting Switch ==="
ssh switch "cd /root && ./switch 192.168.0.3 2>&1" &
SWITCH_PID=$!
sleep 3

echo ""
echo "=== Step 3: Starting VM1 (rank 0) ==="
ssh vm1 "cd /root && ./host 2 192.168.0.3 0 2>&1" &
VM1_PID=$!
sleep 2

echo ""
echo "=== Step 4: Starting VM2 (rank 1) ==="
ssh vm2 "cd /root && ./host 2 192.168.0.3 1 2>&1" &
VM2_PID=$!

echo ""
echo "=== Waiting for completion (30 seconds) ==="
sleep 30

echo ""
echo "=== Cleaning up ==="
kill $CONTROLLER_PID $SWITCH_PID $VM1_PID $VM2_PID 2>/dev/null
