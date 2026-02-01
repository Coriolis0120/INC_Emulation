#!/bin/bash
# 启动所有 soft_switch

echo "=== Starting soft_switch on all nodes ==="

# 先停止旧进程
ssh switch "pkill -9 soft_switch" 2>/dev/null
ssh vm1 "pkill -9 soft_switch" 2>/dev/null
ssh vm2 "pkill -9 soft_switch" 2>/dev/null

sleep 1

# 启动
ssh switch "cd ~ && nohup ./soft_switch spine > soft_switch.log 2>&1 &"
ssh vm1 "cd ~ && nohup ./soft_switch leaf1 > soft_switch.log 2>&1 &"
ssh vm2 "cd ~ && nohup ./soft_switch leaf2 > soft_switch.log 2>&1 &"

echo "Started. Check logs with:"
echo "  ssh switch 'cat ~/soft_switch.log'"
echo "  ssh vm1 'cat ~/soft_switch.log'"
echo "  ssh vm2 'cat ~/soft_switch.log'"
