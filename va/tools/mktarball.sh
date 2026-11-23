#!/bin/sh
# mktarball.sh - the mTrio / mAnalog source tarball for a local build of the
# APKBUILD (packaging/APKBUILD downloads the repository's GitHub archive;
# this is for builds of an unpublished state, e.g. in the builder VM).
#
#   tools/mktarball.sh [OUTDIR]            HEAD (committed files only)
#   tools/mktarball.sh --worktree [OUTDIR] the working tree as it is
#
# Writes OUTDIR/mdt-va-lv2-VERSION.tar.gz (top directory mdt-va-lv2-VERSION/
# with va/ and common/, which it uses; the recipe builds in
# mdt-va-lv2-VERSION/va) and prints its sha512sum line.
# VERSION: the CMake project version.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
worktree=0
if [ "${1:-}" = "--worktree" ]; then
	worktree=1
	shift
fi
outdir=${1:-.}
mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)

version=$(sed -n 's/^project (mdt-va VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")
[ -n "$version" ] || { echo "mktarball: no version in CMakeLists.txt" >&2; exit 1; }
name=mdt-va-lv2-$version
tarball=$outdir/$name.tar.gz

if [ $worktree = 1 ]; then
	# the tracked and untracked (not ignored) files of the working tree
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	mkdir "$tmp/$name"
	(cd "$here/.." && git ls-files -co --exclude-standard va common | tar -cf - -T -) | tar -xf - -C "$tmp/$name"
	tar -C "$tmp" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -czf "$tarball" "$name"
else
	top=$(git -C "$here" rev-parse --show-toplevel)
	git -C "$top" archive --format=tar.gz --prefix="$name/" HEAD va common > "$tarball"
	echo "commit $(git -C "$top" rev-parse --short HEAD)" >&2
fi
echo "$tarball" >&2
(cd "$outdir" && sha512sum "$name.tar.gz")
