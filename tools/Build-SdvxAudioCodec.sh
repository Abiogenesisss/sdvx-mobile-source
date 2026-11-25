#!/usr/bin/env bash
set -euo pipefail

source_dir=$1
build_dir=$2
ndk_dir=$3
abi=$4
make=$5
case "$abi" in
    arm64-v8a) arch=aarch64; target=aarch64-linux-android26 ;;
    x86_64) arch=x86_64; target=x86_64-linux-android26 ;;
    *) exit 2 ;;
esac

toolchain="$ndk_dir/toolchains/llvm/prebuilt/windows-x86_64/bin"
mkdir -p "$build_dir"
cd "$build_dir"
export TMPDIR="$build_dir"
MSYS2_ARG_CONV_EXCL='*' bash "$source_dir/configure" \
    --target-os=android --arch="$arch" --enable-cross-compile \
    --host-cc=cl.exe --host-ld=link.exe \
    --cc="$toolchain/clang.exe --target=$target" \
    --ar="$toolchain/llvm-ar.exe" --ranlib="$toolchain/llvm-ranlib.exe" \
    --nm="$toolchain/llvm-nm.exe" --strip="$toolchain/llvm-strip.exe" \
    --enable-shared --disable-static --enable-pic --disable-x86asm \
    --disable-everything --disable-autodetect --disable-network \
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale \
    --enable-demuxer=asf --enable-decoder=wmapro,wmav1,wmav2 --enable-swresample \
    --extra-ldflags=-Wl,-z,max-page-size=16384 --prefix="$build_dir/install"
"$make" -j8
"$make" install
