#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
root=${SYSFS_SOUND_ROOT:-/sys/class/sound}

if [ "$#" -eq 0 ]; then
	echo "usage: $0 uac2.instance [...]" >&2
	exit 2
fi

found=0
for attr in "$root"/card*/function_name; do
	[ -e "$attr" ] || continue
	found=1
	mode=$(stat -c '%a' "$attr") || exit 1
	case "$mode" in
		*[2367])
			echo "$attr is writable" >&2
			exit 1
			;;
	esac
done

if [ "$found" -eq 0 ]; then
	echo "no UAC function identity attributes [SKIP]"
	exit "$ksft_skip"
fi

for expected in "$@"; do
	count=0
	card=
	for attr in "$root"/card*/function_name; do
		[ -e "$attr" ] || continue
		if [ "$(cat "$attr")" = "$expected" ]; then
			count=$((count + 1))
			card=${attr%/function_name}
		fi
	done
	if [ "$count" -ne 1 ]; then
		echo "$expected matched $count cards" >&2
		exit 1
	fi
	echo "$expected ${card##*/} [OK]"
done
