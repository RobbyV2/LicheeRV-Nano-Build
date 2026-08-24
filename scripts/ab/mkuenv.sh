#!/bin/sh
# Render uEnv.txt for the A/B boot policy.
# usage: mkuenv.sh <committed|trial> <out> [iface] [dev] [pre] [bootm]
# The output must end with "\n\0": u-boot "env import <addr> -" scans for a
# newline followed by NUL and otherwise parses RAM past the end of the file.
set -eu
state=${1:?state}
out=${2:?out}
iface=${3:-mmc}
dev=${4:-0:1}
pre=${5:-}
bootm=${6:-bootm}
[ -n "$pre" ] || pre='mmc dev ${sddev}'

case $state in
committed|trial) ;;
*) echo "bad state: $state" >&2; exit 1 ;;
esac

src=$(dirname "$0")/uEnv.txt.in
sed -e "s|@IF@|$iface|" -e "s|@DEV@|$dev|" -e "s|@PRE@|$pre|" \
    -e "s|@STATE@|$state|" -e "s|@BOOTM@|$bootm|" "$src" > "$out.tmp"
printf '\000' >> "$out.tmp"
mv "$out.tmp" "$out"
