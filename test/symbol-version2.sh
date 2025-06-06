#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -o $t/a.o -c -xc -
void foo() {}
void bar1() {}

__asm__(".symver foo, foo@TEST");
__asm__(".symver bar1, bar@TEST");
EOF

cat <<EOF > $t/b.version
TEST { global: foo; };
EOF

$CC -B. -o $t/c.so -shared $t/a.o -Wl,--version-script=$t/b.version
readelf -W --dyn-syms $t/c.so > $t/log

grep ' foo@TEST' $t/log
grep ' bar@TEST' $t/log
grep ' bar1' $t/log
not grep ' foo@@TEST' $t/log
