#!/bin/sh
# Freestanding 32-bit Linux build of fluid.cpp + fake PhysX scene. Needs clang only (no 32-bit libc).
set -e
cd "$(dirname "$0")"
FL="-m32 -ffreestanding -fno-builtin -nostdlib -static -fno-exceptions -fno-rtti -fno-pic -msse2 -mfpmath=sse -O2 -Ishim -isystem $(clang -print-resource-dir)/include -Wno-c99-extensions -Wno-unused-parameter -Wno-unsequenced"
for v in st mt; do
  D=""; [ $v = mt ] && D="-DSHIM_MT"
  clang++ $FL $D -c ../src/fluid.cpp -o fluid_$v.o
  clang++ $FL $D -c test.cpp -o test_$v.o
  clang++ $FL fluid_$v.o test_$v.o -o fluidtest_$v -Wl,-e,_start
  ./fluidtest_$v | grep -E "PASS|FAIL|CHECKSUM|RESULT"
done
# cloth collision pre-check (src/clothfast.cpp) against a fake PhysXCore image
clothfl="-m32 -ffreestanding -fno-builtin -nostdlib -static -fno-exceptions -fno-rtti -fno-pic -msse2 -mfpmath=sse -O2 -Ishim -isystem $(clang -print-resource-dir)/include -Wno-c99-extensions -Wno-unused-parameter"
clang++ $clothfl -c clothfast_test.cpp -o clothfast_test.o
clang++ $clothfl clothfast_test.o -o clothfasttest -Wl,-e,_start
./clothfasttest | grep -E "PASS|FAIL|RESULT"
# NxCloth census thunks (src/clothspy.cpp)
clang++ $clothfl -c clothspy_test.cpp -o clothspy_test.o
clang++ $clothfl clothspy_test.o -o clothspytest -Wl,-e,_start
./clothspytest | grep -E "PASS|FAIL|RESULT"
