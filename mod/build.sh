#!/usr/bin/env bash
# sky-blueprint 构建脚本（避开 msys make 的临时目录问题）
set -e
cd "$(dirname "$0")"

[ -d /c/msys64/ucrt64/bin ] && export PATH="/c/msys64/ucrt64/bin:$PATH"

# msys make 的临时目录问题：改用一个干净的 Windows 临时目录
if command -v cygpath >/dev/null 2>&1; then
  _TMPWIN="$(cygpath -w "$(mktemp -d)")"
  export TMPDIR="$_TMPWIN" TMP="$_TMPWIN" TEMP="$_TMPWIN"
fi

CC=gcc
CXX=g++
OUT=dist
DEFS=""
if [ "$CN" = "1" ]; then
  OUT=dist_cn
  DEFS="-DBP_CN_DIAG=1"
elif [ "$CN" = "2" ]; then
  OUT=dist_cn_live
  DEFS="-DBP_CN_LIVE=1"
fi
INC="-I./src -I./libraries/htmodloader/includes/imgui-1.92.2b -I./libraries/htmodloader/includes/imgui-1.92.2b/backends -I./libraries/htmodloader/includes/htmodloader"
CFLAGS="-O2 -ffunction-sections -fdata-sections -static -s -mavx -msse -Wall -Wno-unused-const-variable -Wno-unused-function -Wno-missing-braces $DEFS"
LFLAGS="-Wl,--gc-sections,-O2,--as-needed,--version-script=src/exports.txt -L./libraries/htmodloader/lib -lhtmodloader -lshell32"

mkdir -p "$OUT"
rm -f "$OUT"/*.o "$OUT"/sky-blueprint.dll

OBJS=""
for src in src/*.c src/*.cpp src/*/*.c src/*/*.cpp; do
  [ -e "$src" ] || continue
  base=$(basename "$src")
  obj="$OUT/${base%.*}.o"
  case "$src" in
    *.c)   $CC  $CFLAGS $INC -c "$src" -o "$obj" ;;
    *.cpp) $CXX $CFLAGS $INC -c "$src" -o "$obj" ;;
  esac
  OBJS="$OBJS $obj"
done

# 按目标文件展开（避免重复 basename 冲突）
$CXX $CFLAGS $OBJS -shared -o "$OUT/sky-blueprint.dll" $LFLAGS
echo "OK -> $OUT/sky-blueprint.dll"
