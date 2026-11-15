#!/bin/sh
# Rebuilds the mingw-w64 sysroot used by tools/build_linux.sh (headers + kernel32/version/msvcrt import libs) from GitHub.
# Verified: rebuilding 1.1.0 with it gives a DLL identical to the shipped one except the PE timestamp.
set -e
M=/tmp/mw; S=${SYSROOT:-$HOME/toolchain/sysroot}
rm -rf $M && git clone --depth 1 --filter=blob:none --sparse https://github.com/mingw-w64/mingw-w64.git $M
(cd $M && git sparse-checkout set mingw-w64-headers/include mingw-w64-headers/crt mingw-w64-crt/lib32 mingw-w64-crt/lib-common mingw-w64-crt/def-include)
mkdir -p $S/include $S/lib
cp -r $M/mingw-w64-headers/include/. $S/include/; cp -r $M/mingw-w64-headers/crt/. $S/include/
sed 's/@DEFAULT_MSVCRT_VERSION@/0x700/; s/@DEFAULT_WIN32_WINNT@/0x601/' $M/mingw-w64-headers/crt/_mingw.h.in > $S/include/_mingw.h
cd $S/lib
llvm-dlltool -m i386 -k -d $M/mingw-w64-crt/lib32/kernel32.def -l libkernel32.a
llvm-dlltool -m i386 -k -d $M/mingw-w64-crt/lib32/version.def -l libversion.a
clang -E -P -x c -DDEF_I386 -I$M/mingw-w64-crt/def-include $M/mingw-w64-crt/lib-common/msvcrt.def.in -o msvcrt.def 2>/dev/null
llvm-dlltool -m i386 -k -d msvcrt.def -l libmsvcrt.a
