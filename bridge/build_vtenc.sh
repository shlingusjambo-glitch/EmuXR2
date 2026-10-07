#!/bin/sh
# builds the stream's native frame path (macOS): bridge/vtenc.c -> input/out/vtenc
cd "$(dirname "$0")" && mkdir -p ../input/out && exec clang -O2 -Wall vtenc.c -o ../input/out/vtenc \
    -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation -framework Accelerate
