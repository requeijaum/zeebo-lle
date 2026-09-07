# OKL4 2.1.1 ARM kernel build — python2-cml2 BYPASS recipe (proven 2026-09-07)
# Source: /tmp/okl4_rochus (clone rochus-keller/OKL4); kernel in
#   tools/magpie/test/fullsystem/pistachio/kernel
# Toolchain: arm-none-eabi-gcc 14.2 (apt gcc-arm-none-eabi, installed in HLE session)
# No python2 needed: config.h is hand-built (see config.h.arm here).

K=/tmp/okl4_rochus/tools/magpie/test/fullsystem/pistachio/kernel
B=/tmp/okl4_build/kernel
rm -rf $B && mkdir -p $B/config $B/include && cp -r $K/config/template/. $B/
cat > $B/Makeconf.local <<EOF
ARCH = arm
CPU = sa1100
PLATFORM = pleb2
TOOLPREFIX = arm-none-eabi-
NO_CCACHE = 1
SRCDIR	= $K
BUILDDIR = $B
EOF
# do NOT let cml2 regenerate config.h: copy the hand-built one
cp $K/config/config.h.arm $B/config/config.h
cp $K/config/config.out.arm $B/config/config.out
touch $B/config/config.h $B/config/config.out $B/Makeconf.local
cd $B && make
# Result: src/generic/lib.o + kmemory.o compile for ARM (chain proven).
# Remaining errors are pleb2/SA1100-platform header gaps (IODEVICE_VADDR,
# arm_cache::cache_invalidate_d) - fix in platform files or make an MSM7201A board.
