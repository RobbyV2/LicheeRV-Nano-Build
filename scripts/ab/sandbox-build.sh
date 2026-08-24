#!/bin/bash
# Build u-boot for the sandbox target so the A/B boot policy in uEnv.txt.in can
# be executed instead of only read. The cvitek fork has drifted from mainline in
# ways that break the sandbox target; the source-side repairs live in the tree
# (all guarded by CONFIG_SANDBOX, so the riscv build is unaffected) and the
# config deltas below turn off cvitek and k210 drivers that assume real silicon.
#
# The deltas keep the two symbols the policy depends on aligned with the device:
# CMD_ITEST off and CMD_SETEXPR_FMT off, so the tested script cannot use a
# builtin the device build does not have.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
UBOOT=$TOP/u-boot-2021.10
OUT=${OUT:-/work/ota/sb}

command -v sdl2-config >/dev/null || {
  echo "need libsdl2-dev (this fork lost the CONFIG_SANDBOX_SDL symbol, so sdl.o always builds)" >&2
  exit 1
}

DISABLE="
CMD_CVI_UPDATE CMD_CVI_SD_BOOT CMD_CVI_SD_UPDATE ENABLE_ALIOS_UPDATE
VIDEO_CVITEK DISPLAY_CVITEK_MIPI
CMD_SF CMD_SF_TEST SPI_FLASH_MTD CMD_SPI
FS_BTRFS CMD_BTRFS ZSTD BZIP2 LZO
CLK_K210 CLK_K210_SET_RATE
UNIT_TEST CMD_UT UT_LIB UT_COMPRESSION UT_DM UT_ENV UT_OVERLAY UT_TIME UT_UNICODE
CMD_ITEST CMD_SETEXPR_FMT
"
ENABLE="CMD_FAT FAT_WRITE CMD_SETEXPR CMD_ECHO CMD_IMPORTENV"

mkdir -p "$OUT"
make -C "$UBOOT" O="$OUT" sandbox_defconfig >/dev/null
for k in $DISABLE; do
  sed -i "s/^CONFIG_$k=y/# CONFIG_$k is not set/" "$OUT/.config"
done
for k in $ENABLE; do
  grep -q "^CONFIG_$k=y" "$OUT/.config" || \
    sed -i "s/^# CONFIG_$k is not set/CONFIG_$k=y/" "$OUT/.config"
done
make -C "$UBOOT" O="$OUT" olddefconfig >/dev/null

# olddefconfig re-applies anything still selected; assert the ones that matter.
for k in $ENABLE; do
  grep -q "^CONFIG_$k=y" "$OUT/.config" || { echo "CONFIG_$k did not stick"; exit 1; }
done
for k in CMD_ITEST CMD_SETEXPR_FMT; do
  grep -q "^CONFIG_$k=y" "$OUT/.config" && { echo "CONFIG_$k must stay off to match the device"; exit 1; }
done

make -C "$UBOOT" O="$OUT" DEVICE_TREE=sandbox -j"$(nproc)" >"$OUT/build.log" 2>&1 || true
[ -x "$OUT/u-boot" ] || { echo "sandbox build failed, see $OUT/build.log"; exit 1; }
[ -f "$OUT/arch/sandbox/dts/sandbox.dtb" ] || { echo "sandbox.dtb missing"; exit 1; }
echo "sandbox u-boot: $OUT/u-boot"
