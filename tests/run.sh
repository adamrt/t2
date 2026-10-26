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
    if ! $VM build/tc.bin -o $OUT/$name.asm system/lib/core.tr $t; then
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

# Each library test is compiled with the library it's named after.
for t in tests/lib/*.tr; do
    name=$(basename $t .tr)
    if ! $VM build/tc.bin -o $OUT/lib-$name.asm system/lib/core.tr system/lib/$name.tr $t; then
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
