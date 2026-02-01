#!/bin/bash
# 部署 soft_switch 到各节点

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Deploying soft_switch ==="

scp $SCRIPT_DIR/soft_switch switch:~/
scp $SCRIPT_DIR/soft_switch vm1:~/
scp $SCRIPT_DIR/soft_switch vm2:~/

echo "Done."
