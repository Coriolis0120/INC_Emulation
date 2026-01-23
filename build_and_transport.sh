#!/bin/bash
set -e

cmake --build build --target switch_v2
cmake --build build --target controller
cmake --build build --target host
cmake --build build --target host_reduce
cmake --build build --target host_multi_ops

rsync -av build/switch_v2 switch:/root
rsync -av build/controller controller:/root

rsync -av build/host_multi_ops vm1:/root
rsync -av build/host_multi_ops vm2:/root
rsync -av build/host_reduce vm1:/root
rsync -av build/host_reduce vm2:/root
rsync -av build/host vm1:/root
rsync -av build/host vm2:/root
