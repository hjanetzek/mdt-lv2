#!/bin/sh
# mktarball.sh - the eDrumSynth source tarball for the APKBUILD
# (packaging/APKBUILD), until lv2/ is published and the recipe downloads
# its archive.
#
#   tools/mktarball.sh [OUTDIR]            HEAD (committed files only)
#   tools/mktarball.sh --worktree [OUTDIR] the working tree as it is
#
# Writes OUTDIR/edrumsynth-VERSION.tar.gz (top directory edrumsynth-VERSION/,
# the contents of lxrdrums/) and prints its sha512sum line for the recipe.
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

version=$(sed -n 's/^project (edrumsynth VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")
[ -n "$version" ] || { echo "mktarball: no version in CMakeLists.txt" >&2; exit 1; }
name=edrumsynth-$version
tarball=$outdir/$name.tar.gz

if [ $worktree = 1 ]; then
	# the tracked and untracked (not ignored) files of the working tree
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	mkdir "$tmp/$name"
	(cd "$here" && git ls-files -co --exclude-standard . | tar -cf - -T -) | tar -xf - -C "$tmp/$name"
	tar -C "$tmp" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -czf "$tarball" "$name"
else
	top=$(git -C "$here" rev-parse --show-toplevel)
	sub=$(git -C "$here" rev-parse --show-prefix)
	git -C "$top" archive --format=tar.gz --prefix="$name/" "HEAD:${sub%/}" > "$tarball"
	echo "commit $(git -C "$top" rev-parse --short HEAD)" >&2
fi
echo "$tarball" >&2
(cd "$outdir" && sha512sum "$name.tar.gz")
