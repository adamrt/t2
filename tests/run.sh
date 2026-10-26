#!/bin/sh
# Runs every test. Usage: tests/run.sh ASSEMBLER...
# where ASSEMBLER is a command that takes "-o out.bin files...".

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

for t in tests/isa/*.asm; do
    name=$(basename $t .asm)
    if ! "$@" -o $OUT/$name.bin $t; then
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
    if ! "$@" -o $OUT/$name.bin $t; then
        report "assembly failed" $t
        continue
    fi
    got=$($VM $OUT/$name.bin 2>&1 | head -1)
    case "$got" in
        "fault: $want at"*) report ok $t ;;
        *) report "got '$got'" $t ;;
    esac
done

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
