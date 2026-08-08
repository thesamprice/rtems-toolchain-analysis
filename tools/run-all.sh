#!/bin/bash
#
# Run every built RTEMS test executable under QEMU and classify the result.
#
# The point of this script is that both compilers get *identical* treatment:
# same machine, same timeout, same classification strings, same test set.  The
# earlier numbers in 05 and 06 compared two runs that did not share those, and
# were not meaningful as a comparison.  Run it once per build tree and diff the
# two results.txt files.
#
# Usage:
#   tools/run-all.sh <build-dir> <output-dir> [jobs] [timeout-seconds]
#
#   <build-dir>   a BSP build directory, e.g. <builddir>/build/riscv/mbv
#   <output-dir>  created; receives results.txt and logs/
#
# Environment:
#   QEMU          qemu-system-riscv32 to use (default: from PATH)
#
# Example:
#   tools/run-all.sh gcc-tree/build/riscv/mbv   out-gcc
#   tools/run-all.sh clang-tree/build/riscv/mbv out-clang
#   diff out-gcc/results.txt out-clang/results.txt
#
set -u

B=${1:?build directory required}
OUT=${2:?output directory required}
J=${3:-8}
TMO=${4:-25}
Q=${QEMU:-qemu-system-riscv32}

mkdir -p "$OUT/logs"
: > "$OUT/results.txt"

one() {
    local exe=$1 n L v
    n=$(basename "$exe" .exe)
    L="$OUT/logs/$n.txt"
    rm -f "$L"; : > "$L"

    "$Q" -M amd-microblaze-v-generic -m 256m \
         -display none -monitor none -serial "file:$L" -no-reboot \
         -icount shift=0,sleep=off \
         -device loader,file="$exe",cpu-num=0 >/dev/null 2>&1 &
    local qp=$!

    # Poll rather than sleeping the full timeout: most tests finish in under a
    # second and a flat sleep would make a full run take hours.
    for _ in $(seq 1 "$TMO"); do
        sleep 1
        grep -q "END OF TEST\|RTEMS shutdown\|FATAL\|TEST STATE: USER_INPUT\|TEST STATE: BENCHMARK" \
             "$L" 2>/dev/null && break
    done
    kill -9 $qp 2>/dev/null; wait $qp 2>/dev/null

    if   grep -q "TEST STATE: EXPECTED_FAIL" "$L" 2>/dev/null; then v=XFAIL
    elif grep -q "TEST STATE: USER_INPUT"    "$L" 2>/dev/null; then v=SKIP
    elif grep -q "TEST STATE: BENCHMARK"     "$L" 2>/dev/null; then v=SKIP
    elif grep -q "END OF TEST"               "$L" 2>/dev/null; then v=PASS
    elif [ ! -s "$L" ];                                        then v=NO-OUTPUT
    else                                                            v=FAIL
    fi
    echo "$v $n" >> "$OUT/results.txt"
}

n=0
for e in $(find "$B/testsuites" -name '*.exe' ! -name '*.norun.exe' | sort); do
    one "$e" &
    n=$((n + 1))
    [ $((n % J)) -eq 0 ] && wait
done
wait

sort -o "$OUT/results.txt" "$OUT/results.txt"
awk '{c[$1]++} END {for (k in c) printf "%6d %s\n", c[k], k}' "$OUT/results.txt" | sort -rn
echo "total: $(wc -l < "$OUT/results.txt")"
