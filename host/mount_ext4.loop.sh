#!/bin/sh
#
# What host/mount_ext4.sh should be. Its fuse2fs 1.47.0 writes inode modes that
# contradict the ACLs it writes alongside them under -o fakeroot, so a tree
# merged through it is not the tree that was staged. A loop mount has no such
# translation layer, at the cost of needing a privileged container.

set -eu

[ -n "${2:-}" ] || { echo "usage: $0 image dir" >&2; exit 1; }

PART_OFFSET=$(partx -s "$1" | awk '$1 == 2 { print $2 * 512; found = 1 } END { exit !found }')

mkdir -p "$2"
mount -o "loop,offset=$PART_OFFSET" "$1" "$2"
