#!/bin/bash
# INC System Stop Script

echo "=== Stopping INC System ==="
echo ""

echo "[1/4] Stopping host on vm1..."
ssh vm1 "pkill -f './host'"

echo "[2/4] Stopping host on vm2..."
ssh vm2 "pkill -f './host'"

echo "[3/4] Stopping switch..."
ssh switch "pkill -f './switch'"

echo "[4/4] Stopping controller..."
ssh controller "pkill -f './controller'"

echo ""
echo "=== All processes stopped ==="
