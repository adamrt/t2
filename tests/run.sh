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

assemble() {
    $VM build/asm.bin -o "$@"
}

for t in tests/isa/*.asm; do
    name=$(basename $t .asm)
    if ! assemble $OUT/$name.bin $t; then
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
    if ! assemble $OUT/$name.bin $t; then
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
    if ! $VM build/tc.bin -L system/lib -o $OUT/$name.asm $t; then
        report "compile failed" $t
        continue
    fi
    if ! assemble $OUT/$name.bin system/lib/rt.asm $OUT/$name.asm; then
        report "assembly failed" $t
        continue
    fi
    got=$($VM $OUT/$name.bin 2>&1)
    status=$?
    if [ $status -eq 0 ] && [ "$got" = ok ]; then report ok $t; else report "exit $status: $got" $t; fi
done

# Each library test tests the module it's named after.
for t in tests/lib/*.tr; do
    name=$(basename $t .tr)
    if ! $VM build/tc.bin -L system/lib -o $OUT/lib-$name.asm $t; then
        report "compile failed" $t
        continue
    fi
    if ! assemble $OUT/lib-$name.bin system/lib/rt.asm $OUT/lib-$name.asm; then
        report "assembly failed" $t
        continue
    fi
    got=$($VM $OUT/lib-$name.bin 2>&1)
    status=$?
    if [ $status -eq 0 ] && [ "$got" = ok ]; then report ok $t; else report "exit $status: $got" $t; fi
done

# The C tools in bootstrap/ must agree with the Terrarium ones byte for byte:
# tc0 compiles everything the build compiled, and asm0 assembles it.
differs=""
agree() {
    cmp -s "$1" "$2" || differs="$differs $1"
}
build/tc0 -L system/lib -o $OUT/c-tc.asm system/tc/*.tr && agree build/tc.asm $OUT/c-tc.asm
build/tc0 -L system/lib -o $OUT/c-kernel.asm system/kernel/*.tr && agree build/kernel.asm $OUT/c-kernel.asm
for f in system/bin/*.tr; do
    n=$(basename $f .tr)
    build/tc0 -L system/lib -o $OUT/c-$n.asm $f && agree build/bin/$n.asm $OUT/c-$n.asm
done
for f in system/tools/*.tr; do
    n=$(basename $f .tr)
    build/tc0 -L system/lib -o $OUT/c-$n.asm $f && agree build/$n.asm $OUT/c-$n.asm
done
for t in tests/lang/*.tr; do
    n=$(basename $t .tr)
    build/tc0 -L system/lib -o $OUT/c-$n.asm $t && agree $OUT/$n.asm $OUT/c-$n.asm
done
if [ -z "$differs" ]; then report ok tc0; else report "differs from tc:$differs" tc0; fi

differs=""
build/asm0 -o $OUT/c-asm.bin -s $OUT/c-asm.sym system/asm/asm.asm
agree build/asm.bin $OUT/c-asm.bin
agree build/asm.sym $OUT/c-asm.sym
build/asm0 -o $OUT/c-tc.bin -s $OUT/c-tc.sym system/lib/rt.asm build/tc.asm
agree build/tc.bin $OUT/c-tc.bin
agree build/tc.sym $OUT/c-tc.sym
build/asm0 -o $OUT/c-kernel.bin -s $OUT/c-kernel.sym system/kernel/krt.asm build/kernel.asm
agree build/kernel.bin $OUT/c-kernel.bin
agree build/kernel.sym $OUT/c-kernel.sym
for f in build/bin/*.asm; do
    n=$(basename $f .asm)
    build/asm0 -o $OUT/c-$n.bin -s $OUT/c-$n.sym system/lib/rt.asm $f
    agree build/bin/$n.bin $OUT/c-$n.bin
    agree build/bin/$n.sym $OUT/c-$n.sym
done
for f in build/mk*.asm; do
    n=$(basename $f .asm)
    build/asm0 -o $OUT/c-$n.bin system/lib/rt.asm $f && agree build/$n.bin $OUT/c-$n.bin
done
for t in tests/isa/*.asm tests/faults/*.asm; do
    n=$(basename $t .asm)
    build/asm0 -o $OUT/c-$n.bin $t && agree $OUT/$n.bin $OUT/c-$n.bin
done
if [ -z "$differs" ]; then report ok asm0; else report "differs from asm:$differs" asm0; fi

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
