#!/bin/sh
# Testes de host (no PC) da fila persistente. Usa o primeiro compilador C que achar.
set -eu
cd "$(dirname "$0")"
CC="${CC:-$(command -v cc || command -v gcc || command -v clang || echo "python -m ziglang cc")}"
$CC -std=gnu11 -Wall -Wno-unused-function -Istubs -o test_event_queue test_event_queue.c
./test_event_queue
