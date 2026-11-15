#!/bin/sh
# Cross-builds PhysXLoader.dll on Linux with clang + lld (no Visual Studio needed).
# Needs the mingw-w64 sysroot made by tools/make_sysroot.sh (default location $HOME/toolchain/sysroot, or set SYSROOT).
# Output: build-check/PhysXLoader.dll. The released DLLs were built this way.
set -e
cd "$(dirname "$0")/.."
SYSROOT=${SYSROOT:-$HOME/toolchain/sysroot}
mkdir -p build-check
for f in proxy fluid clothfast clothspy; do
  clang++ --target=i686-w64-windows-gnu -nostdinc -isystem "$SYSROOT/include" -isystem "$(clang -print-resource-dir)/include" \
    -std=c++14 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti \
    -c src/$f.cpp -o build-check/$f.o
done
llvm-rc -I "$SYSROOT/include" -fo build-check/version.res src/version.rc
llvm-cvtres -machine:x86 -out:build-check/version.obj build-check/version.res >/dev/null
ld.lld -m i386pe --dll -e DllMain@12 --dynamicbase --nxcompat --kill-at build-check/proxy.o build-check/fluid.o \
  build-check/clothfast.o build-check/clothspy.o build-check/version.obj src/PhysXLoader.def \
  -L"$SYSROOT/lib" -lkernel32 -lversion -lmsvcrt -o build-check/PhysXLoader.dll
echo "built build-check/PhysXLoader.dll"
