#!/bin/bash
. $(dirname $0)/common.inc

# OneTBB isn't tsan-clean
nm mold | grep '__tsan_init' && skip

cat <<EOF | $CC -c -o $t/a.o -xc - -g
#include <stdio.h>
void hello() { printf("Hello world\n"); }
EOF

cat <<EOF | $CC -c -o $t/b.o -xc - -g
void hello();
int main() { hello(); }
EOF

# It looks like objdump prints out a warning message for
# object files compiled with Clang.
$OBJDUMP --dwarf=info $t/a.o $t/b.o |& grep 'Warning: DIE at offset' && skip

./mold --relocatable -o $t/c.o $t/a.o $t/b.o

$CC -B. -o $t/exe $t/c.o
$QEMU $t/exe | grep 'Hello world'

$OBJDUMP --dwarf=info $t/c.o > /dev/null |& not grep Warning
