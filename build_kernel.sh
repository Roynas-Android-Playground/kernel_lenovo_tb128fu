#/bin/bash
set -e

[ ! -e "toolchain" ] && echo "Make toolchain avaliable at $(pwd)/toolchain" && exit

export KBUILD_BUILD_USER=Royna
export KBUILD_BUILD_HOST=GrassLand

PATH=$PWD/toolchain/bin:$PATH

rm -rf out
make O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LLVM=1 -j$(nproc) grass-perf_defconfig
make O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LLVM=1 -j$(nproc)
