#!/bin/sh
# Testes de host (no PC) da fila persistente. Usa o primeiro compilador C que achar.
set -eu
cd "$(dirname "$0")"
CC="${CC:-$(command -v cc || command -v gcc || command -v clang || echo "python -m ziglang cc")}"
$CC -std=gnu11 -Wall -Wno-unused-function -Istubs -o test_event_queue test_event_queue.c
./test_event_queue
$CC -std=gnu11 -Wall -o test_display test_display.c ../../main/display.c
./test_display

# O prazo da placa tem que ficar acima do TIMEOUT_APROVACAO_S do servidor (120 s).
grep -A2 "config VEND_APPROVAL_TIMEOUT_SEC" ../../main/Kconfig.projbuild | grep -q "default 130"
grep -q "^CONFIG_VEND_APPROVAL_TIMEOUT_SEC=130$" ../../sdkconfig
echo "prazo da aprovacao: 130 s no Kconfig e no sdkconfig"
