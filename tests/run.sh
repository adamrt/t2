#!/bin/sh
# Runs every test with the tools in build/. With --update, it instead saves
# what each system session prints as its expected output.

VM=build/terrarium
OUT=build/tests
mkdir -p $OUT
pass=0
fail=0

report() {
    if [ "$1" = ok ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $2: $1"
    fi
}

# Builds a program with tc: from .tr files with the library, or from .asm
# files alone.
build() {
    $VM build/tc.bin -L system/lib -o "$@"
}

for t in tests/isa/*.asm; do
    name=$(basename $t .asm)
    if ! build $OUT/$name.bin $t; then
        report "assembly failed" $t
        continue
    fi
    got=$($VM $OUT/$name.bin 2>&1)
    status=$?
    if [ $status -eq 0 ] && [ "$got" = ok ]; then report ok $t; else report "exit $status: $got" $t; fi
done

for t in tests/faults/*.asm; do
    name=$(basename $t .asm)
    want=$(sed -n '1s/^; fault: //p' $t)
    if ! build $OUT/$name.bin $t; then
        report "assembly failed" $t
        continue
    fi
    got=$($VM $OUT/$name.bin 2>&1 | head -1)
    case "$got" in
        "fault: $want at"*) report ok $t ;;
        *) report "got '$got'" $t ;;
    esac
done

for t in tests/lang/*.tr; do
    name=$(basename $t .tr)
    if ! build $OUT/$name.bin $t; then
        report "compile failed" $t
        continue
    fi
    got=$($VM $OUT/$name.bin 2>&1)
    status=$?
    if [ $status -eq 0 ] && [ "$got" = ok ]; then report ok $t; else report "exit $status: $got" $t; fi
done

# Each library test tests the module it's named after.
for t in tests/lib/*.tr; do
    name=$(basename $t .tr)
    if ! build $OUT/lib-$name.bin $t; then
        report "compile failed" $t
        continue
    fi
    got=$($VM $OUT/lib-$name.bin 2>&1)
    status=$?
    if [ $status -eq 0 ] && [ "$got" = ok ]; then report ok $t; else report "exit $status: $got" $t; fi
done

# The C compiler in bootstrap/ must agree with the Terrarium one byte for
# byte: tc0 builds everything the build and the tests built, and the
# programs and their labels must be the same.
differs=""
agree() {
    cmp -s "$1" "$2" || differs="$differs $1"
}
tc0() {
    build/tc0 -L system/lib "$@"
}
tc0 -o $OUT/c-tc.bin -s $OUT/c-tc.sym system/tc/*.tr
agree build/tc.bin $OUT/c-tc.bin
agree build/tc.sym $OUT/c-tc.sym
tc0 -r system/kernel/kruntime.asm -o $OUT/c-kernel.bin -s $OUT/c-kernel.sym system/kernel/*.tr
agree build/kernel.bin $OUT/c-kernel.bin
agree build/kernel.sym $OUT/c-kernel.sym
for f in system/bin/*.tr; do
    n=$(basename $f .tr)
    tc0 -o $OUT/c-$n.bin -s $OUT/c-$n.sym $f
    agree build/bin/$n.bin $OUT/c-$n.bin
    agree build/bin/$n.sym $OUT/c-$n.sym
done
for f in system/tools/*.tr; do
    n=$(basename $f .tr)
    tc0 -o $OUT/c-$n.bin $f && agree build/$n.bin $OUT/c-$n.bin
done
for t in tests/lang/*.tr tests/isa/*.asm tests/faults/*.asm; do
    n=$(basename $t); n=${n%.*}
    tc0 -o $OUT/c-$n.bin $t && agree $OUT/$n.bin $OUT/c-$n.bin
done
for t in tests/lang/*.tr; do
    n=$(basename $t .tr)
    build/tc0 -S -L system/lib -o $OUT/c-$n.asm $t
    $VM build/tc.bin -S -L system/lib -o $OUT/$n.asm $t && agree $OUT/$n.asm $OUT/c-$n.asm
done
if [ -z "$differs" ]; then report ok tc0; else report "differs from tc:$differs" tc0; fi

# Each session boots a fresh copy of the disk, with the clock stopped at one
# moment, and compares what it prints.
for t in tests/os/*.txt; do
    name=$(basename $t .txt)
    cp build/disk.img $OUT/disk.img
    if [ "$1" = --update ]; then
        timeout 60 $VM boot --headless --date 1790000000 $OUT/disk.img < $t > tests/os/$name.out 2>&1
        echo "updated tests/os/$name.out"
        continue
    fi
    got=$(timeout 60 $VM boot --headless --date 1790000000 $OUT/disk.img < $t 2>&1)
    if [ "$got" = "$(cat tests/os/$name.out)" ]; then report ok $t; else report "output differs" $t; fi
done

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
