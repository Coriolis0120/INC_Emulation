#!/bin/bash
set -e

cmake --build build --target switch
cmake --build build --target controller
cmake --build build --target host

rsync -av build/switch switch:/root
rsync -av build/controller controller:/root
rsync -av build/host vm1:/root
rsync -av build/host vm2:/root
