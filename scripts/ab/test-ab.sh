#!/bin/bash
# A/B boot policy tests against u-boot sandbox.
# Build the rig first with scripts/ab/sandbox-build.sh.
#
# The device compiled-in environment (include/configs/mars-asic.h) is
# reproduced here with mmc->host and cvi_sd_boot dropped, so the uEnv.txt
# under test is the text that ships, except that @BOOTM@ renders as an echo
# (sandbox cannot bootm a riscv FIT). Transcripts are trimmed to the part
# after "run bootcmd" so the echoed setenv lines cannot satisfy assertions.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SB=${SB:-/work/ota/sb}
WORK=${WORK:-/work/ota/abtest}
UB=$SB/u-boot
DTB=$SB/arch/sandbox/dts/sandbox.dtb
ADDR=0x1000000
pass=0
fail=0

[ -x "$UB" ] || { echo "no sandbox u-boot at $UB (run sandbox-build.sh)"; exit 1; }
mkdir -p "$WORK"

# mkdisk <img> <good|-> <alt|-> <cnt|-> <uenvfile|->
mkdisk() {
  local img=$1 good=$2 alt=$3 cnt=$4 uenv=$5
  local p=$WORK/p1.img
  rm -f "$img" "$p"
  dd if=/dev/zero of="$p" bs=1M count=16 status=none
  mkfs.vfat -F 16 -n BOOT "$p" >/dev/null 2>&1
  if [ "$good" != - ]; then printf '%s' "$good" > "$WORK/.g"; mcopy -i "$p" -o "$WORK/.g" ::boot.sd; fi
  if [ "$alt" != - ]; then printf '%s' "$alt" > "$WORK/.a"; mcopy -i "$p" -o "$WORK/.a" ::boot.alt; fi
  if [ "$cnt" != - ]; then
    python3 -c "import sys,struct;open(sys.argv[1],'wb').write(struct.pack('<I',int(sys.argv[2])))" "$WORK/.c" "$cnt"
    mcopy -i "$p" -o "$WORK/.c" ::bootcnt
  fi
  if [ "$uenv" != - ]; then mcopy -i "$p" -o "$uenv" ::uEnv.txt; fi
  dd if=/dev/zero of="$img" bs=1M count=24 status=none
  parted -s "$img" mklabel msdos mkpart primary fat16 1MiB 17MiB set 1 boot on >/dev/null 2>&1
  dd if="$p" of="$img" bs=1M seek=1 conv=notrunc status=none
}

# runboot <img> [pre-command run before bootcmd]
runboot() {
  local img=$1 pre=${2:-}
  cat > "$WORK/in.txt" <<'UBOOT'

host bind 0 @IMG@
setenv sddev 0
setenv uImage_addr @ADDR@
setenv reserved_mem ""
setenv root "root=/dev/mmcblk0p2 rootwait rw"
setenv mtdparts ""
setenv consoledev ttyS0
setenv baudrate 115200
setenv othbootargs "earlycon=sbi"
setenv loadenvcmd 'load host 0:1 ${uImage_addr} uEnv.txt; if test $? -eq 0; then env import ${uImage_addr} - ; fi;'
setenv sdboot 'setenv bootargs ${reserved_mem} ${root} ${mtdparts} console=${consoledev},${baudrate} ${othbootargs}; fatload host 0:1 ${uImage_addr} boot.sd; if test $? -eq 0; then echo STOCKBOOT; fi;'
setenv sdbootauto 'setenv bootargs ${reserved_mem} ${root} ${mtdparts} console=${consoledev},${baudrate} ${othbootargs}; fatload host 0:1 ${uImage_addr} boot.sd; if test $? -eq 0; then echo AUTOBOOT; else echo AUTOFAILED; fi;'
setenv bootcmd 'run loadenvcmd ; run sdboot || run sdbootauto'
@PRE@
run bootcmd
echo "FINAL_BOOTARGS=[${bootargs}]"
if fatload host 0:1 0x2000000 bootcnt; then setexpr FC *0x2000000; echo "FINAL_BOOTCNT=${FC}"; else echo FINAL_BOOTCNT=absent; fi
exit
UBOOT
  sed -i -e "s|@IMG@|$img|" -e "s|@ADDR@|$ADDR|" -e "s|@PRE@|$pre|" "$WORK/in.txt"
  timeout 40 "$UB" -d "$DTB" < "$WORK/in.txt" 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | sed -n '/=> run bootcmd/,$p'
}

check() { # check <name> <file> <regex> <want 1|0>
  local name=$1 f=$2 re=$3 want=$4 got=0
  grep -qE -- "$re" "$f" && got=1
  if [ "$got" = "$want" ]; then
    pass=$((pass+1)); echo "  ok   $name"
  else
    fail=$((fail+1)); echo "  FAIL $name (wanted match=$want for /$re/)"
  fi
}

checkfirst() { # checkfirst <name> <file> <expected slot>
  local name=$1 f=$2 want=$3
  local got
  got=$(grep -oE 'AB_BOOTM (good|trial)' "$f" | head -1 | awk '{print $2}')
  if [ "${got:-none}" = "$want" ]; then
    pass=$((pass+1)); echo "  ok   $name"
  else
    fail=$((fail+1)); echo "  FAIL $name (first boot attempt was '${got:-none}', wanted '$want')"
  fi
}

uenv() { "$HERE/mkuenv.sh" "$1" "$2" host 0:1 'echo AB_PRE' 'echo AB_BOOTM ${ab_slot}'; }

T=$WORK
G="GOOD-KERNEL-SLOT"
A="TRIAL-KERNEL-SLOT"

echo "== 1. committed, both slots present: boots the committed slot, no write =="
uenv committed "$T/u.committed"
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.committed"
runboot "$T/d.img" > "$T/o1"
checkfirst "first attempt is good" "$T/o1" good
check "never tried trial"    "$T/o1" 'AB_BOOTM trial' 0
check "slot=good in bootargs" "$T/o1" 'nanokvm_slot=good' 1
check "counter untouched"    "$T/o1" 'FINAL_BOOTCNT=0' 1
check "no stock fallback"    "$T/o1" 'STOCKBOOT|AUTOBOOT|AUTOFAILED' 0
check "# survived hush"      "$T/o1" 'AB_BOOTM good .*#config-sg2002_licheervnano_sd' 1

echo "== 2. trial, cnt=0: boots the trial slot, counter -> 1 =="
uenv trial "$T/u.trial"
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.trial"
runboot "$T/d.img" > "$T/o2"
checkfirst "first attempt is trial" "$T/o2" trial
check "counter incremented"  "$T/o2" 'FINAL_BOOTCNT=1' 1
check "no rollback"          "$T/o2" 'AB rollback' 0

echo "== 3. trial, cnt == limit: rolls back to the committed slot, counter -> 2 =="
mkdisk "$T/d.img" "$G" "$A" 1 "$T/u.trial"
runboot "$T/d.img" > "$T/o3"
check "rollback announced"   "$T/o3" 'AB rollback' 1
checkfirst "first attempt is good" "$T/o3" good
check "never tried trial"    "$T/o3" 'AB_BOOTM trial' 0
check "slot=good in bootargs" "$T/o3" 'nanokvm_slot=good' 1
check "counter -> 2"         "$T/o3" 'FINAL_BOOTCNT=2' 1

echo "== 4. trial but the trial slot is missing: falls through to committed =="
mkdisk "$T/d.img" "$G" - 0 "$T/u.trial"
runboot "$T/d.img" > "$T/o4"
check "committed slot booted" "$T/o4" 'AB_BOOTM good' 1
check "ends on good slot"    "$T/o4" 'nanokvm_slot=good' 1

echo "== 5. trial but bootcnt missing: rolls back, u-boot creates no file =="
mkdisk "$T/d.img" "$G" "$A" - "$T/u.trial"
runboot "$T/d.img" > "$T/o5"
check "bump failed path"     "$T/o5" 'AB bump failed' 1
checkfirst "first attempt is good" "$T/o5" good
check "no bootcnt created"   "$T/o5" 'FINAL_BOOTCNT=absent' 1

echo "== 6. no uEnv.txt at all: stock sdboot boots the committed slot =="
mkdisk "$T/d.img" "$G" "$A" 0 -
runboot "$T/d.img" > "$T/o6"
check "stock path taken"     "$T/o6" 'STOCKBOOT' 1
check "no AB logic"          "$T/o6" 'AB_PRE' 0

echo "== 7. torn uEnv.txt (truncated): sdboot is last, so stock sdboot survives =="
head -c 300 "$T/u.trial" > "$T/u.torn"
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.torn"
runboot "$T/d.img" > "$T/o7"
check "stock path taken"     "$T/o7" 'STOCKBOOT' 1
check "trial slot not booted" "$T/o7" 'AB_BOOTM trial' 0
check "counter untouched"    "$T/o7" 'FINAL_BOOTCNT=0' 1

echo "== 8. no NUL terminator, RAM poisoned: import overscans =="
"$HERE/mkuenv.sh" trial "$T/u.nonul" host 0:1 'echo AB_PRE' 'echo AB_BOOTM ${ab_slot}'
python3 -c "import sys;d=open(sys.argv[1],'rb').read().rstrip(b'\x00');open(sys.argv[1],'wb').write(d)" "$T/u.nonul"
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.nonul"
runboot "$T/d.img" "mw.b $ADDR 0x41 0x8000" > "$T/o8"
check "overscan observed"    "$T/o8" 'exceeds 1048576|input data size = 1048578' 1
check "still reached a slot" "$T/o8" 'AB_BOOTM (good|trial)|STOCKBOOT|AUTOBOOT' 1

echo "== 8b. WITH NUL terminator, same poison: import stops at end of file =="
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.trial"
runboot "$T/d.img" "mw.b $ADDR 0x41 0x8000" > "$T/o8b"
check "no overscan"          "$T/o8b" 'exceeds 1048576|input data size = 1048578' 0
checkfirst "first attempt is trial" "$T/o8b" trial

echo "== 9. binary garbage uEnv.txt: stock sdboot boots the committed slot =="
head -c 4096 /dev/urandom > "$T/u.junk"
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.junk"
runboot "$T/d.img" > "$T/o9"
check "reached a good kernel" "$T/o9" 'STOCKBOOT|AUTOBOOT|AB_BOOTM good' 1
check "trial slot not booted" "$T/o9" 'AB_BOOTM trial' 0

echo "== 10. first-boot adoption: committed, no alt slot, no bootcnt =="
mkdisk "$T/d.img" "$G" - - "$T/u.committed"
runboot "$T/d.img" > "$T/o10"
checkfirst "first attempt is good" "$T/o10" good
check "slot=good in bootargs" "$T/o10" 'nanokvm_slot=good' 1
check "no fallback needed"   "$T/o10" 'STOCKBOOT|AUTOBOOT|AUTOFAILED' 0

echo "== 11. trial with a garbage counter (0xFFFFFFFF): rolls back =="
mkdisk "$T/d.img" "$G" "$A" 4294967295 "$T/u.trial"
runboot "$T/d.img" > "$T/o11"
check "rollback announced"   "$T/o11" 'AB rollback' 1
check "trial slot not booted" "$T/o11" 'AB_BOOTM trial' 0

echo "== 12. committed slot missing: unrecoverable, and it says so =="
mkdisk "$T/d.img" - "$A" 0 "$T/u.committed"
runboot "$T/d.img" > "$T/o12"
check "stock auto also fails" "$T/o12" 'AUTOFAILED' 1

echo "== 13. sdboot defined but a helper missing: falls back to sdbootauto =="
grep -av '^ab_good_boot=' "$T/u.committed" > "$T/u.partial.tmp"
python3 -c "import sys;open(sys.argv[1],'ab').write(b'\x00')" "$T/u.partial.tmp"
mv "$T/u.partial.tmp" "$T/u.partial"
mkdisk "$T/d.img" "$G" "$A" 0 "$T/u.partial"
runboot "$T/d.img" > "$T/o13"
check "sdbootauto rescued it" "$T/o13" 'AUTOBOOT' 1

echo
echo "pass=$pass fail=$fail"
[ "$fail" = 0 ]
