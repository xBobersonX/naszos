#!/bin/sh
# Buduje tools/jstest/jstest.exe - silnik JavaScript NaszOS jako program dla Windows
# (do szybkich testow bez QEMU). Uzycie (Git Bash): sh tools/jstest/build.sh
set -e
cd "$(dirname "$0")/../.."
LLVM="/c/Program Files/LLVM/bin"
OUT=build/jstest
mkdir -p $OUT
FLAGS="--target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
 -mno-stack-arg-probe -O2 -g -Wall -Wextra -Isrc -c"
cat > $OUT/kernel32.def <<EOF
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ReadFile
CreateFileA
GetFileSize
CloseHandle
VirtualAlloc
ExitProcess
GetTickCount
GetLocalTime
GetCommandLineA
Sleep
EOF
"$LLVM/llvm-dlltool.exe" -m i386:x86-64 -d $OUT/kernel32.def -l $OUT/kernel32.lib
OBJS=""
for f in src/js_*.c src/dom.c src/css.c src/html.c src/string.c src/utf8.c src/arena.c src/http.c src/layout.c src/font.c src/font_data.c \
         tools/jstest/shim.c tools/jstest/main.c tools/jstest/stubs.c; do
  o=$OUT/$(basename $f .c).o
  "$LLVM/clang.exe" $FLAGS $f -o $o
  OBJS="$OBJS $o"
done
"$LLVM/clang.exe" $FLAGS -Drtc_read=rtc_read_hw -Drtc_write=rtc_write_hw -include tools/jstest/io_stub.h src/rtc.c -o $OUT/rtc.o
"$LLVM/clang.exe" --target=x86_64-pc-windows-msvc -c src/js_asm.S -o $OUT/js_asm.o
"$LLVM/lld-link.exe" -nologo -entry:start -subsystem:console -stack:16777216 -out:tools/jstest/jstest.exe \
  $OBJS $OUT/rtc.o $OUT/js_asm.o $OUT/kernel32.lib 2>&1 || true
