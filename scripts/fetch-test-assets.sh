#!/usr/bin/env bash
# Downloads the asset sets that scripts/assettest.sh tests the model and image loaders on.
#
# usage: scripts/fetch-test-assets.sh [--dir <dir>] [--mcguire] [--gltf]
#
# --mcguire: Morgan McGuire's Computer Graphics Archive (https://casual-effects.com/data), obj models with their
#   materials and textures, as zip archives (about 2.7 GB), into <dir>/mcguire. Each file is checked against the size and
#   sha256 in scripts/test-assets/mcguire.txt: the archive publishes no versions, so a file that changed upstream is
#   reported (and kept as <name>.unverified) rather than used, until the manifest is updated.
# --gltf: the Khronos glTF-Sample-Assets models (https://github.com/KhronosGroup/glTF-Sample-Assets), at the commit
#   pinned below (about 2.3 GB), into <dir>/glTF-Sample-Assets.
# Without either, both are fetched. The dir defaults to $SPEEDO_TEST_ASSETS, or resources/test-assets (which git
# ignores, and the client's file dialogs open in). Files already there (and verified) are kept, so running it again only
# fetches what is missing.
#
# Prints the paths to pass to scripts/assettest.sh, e.g.
#   scripts/assettest.sh --client $(scripts/fetch-test-assets.sh)

set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
dir=${SPEEDO_TEST_ASSETS:-$root/resources/test-assets}
mcguire=0
gltf=0

kMcGuireUrl=https://casual-effects.com/g3d/data10
kGltfRepository=https://github.com/KhronosGroup/glTF-Sample-Assets.git
kGltfCommit=edc7c9e67c639d230715049ee31f9a96a6babbbe

while [[ $# -gt 0 ]]; do
	case $1 in
		--dir) dir=$2; shift ;;
		--mcguire) mcguire=1 ;;
		--gltf) gltf=1 ;;
		-h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
[[ $mcguire -eq 0 && $gltf -eq 0 ]] && mcguire=1 gltf=1

failed=0
paths=()

sha256() { shasum -a 256 "$1" | awk '{print $1}'; }

if [[ $mcguire -eq 1 ]]; then
	out=$dir/mcguire
	mkdir -p "$out"

	# <path in the archive> <local name> <size> <sha256>
	while read -r source name size hash; do
		[[ -z $source || $source == \#* ]] && continue
		file=$out/$name

		if [[ -f $file && $(wc -c < "$file" | tr -d ' ') == "$size" && $(sha256 "$file") == "$hash" ]]; then
			continue
		fi

		echo "fetching $name ($((size / 1048576)) MiB)" >&2
		mkdir -p "$(dirname "$file")"
		if ! curl -fL --retry 3 --progress-bar -o "$file.part" "$kMcGuireUrl/$source"; then
			echo "failed to download $kMcGuireUrl/$source" >&2
			rm -f "$file.part"
			failed=1
			continue
		fi

		actual=$(sha256 "$file.part")
		if [[ $actual != "$hash" ]]; then
			mv "$file.part" "$file.unverified"
			echo "$name changed upstream (sha256 $actual, expected $hash): kept as $file.unverified." >&2
			echo "  check it, then update scripts/test-assets/mcguire.txt" >&2
			failed=1
			continue
		fi
		mv "$file.part" "$file"
	done < "$root/scripts/test-assets/mcguire.txt"

	# Bistro comes in parts that reference each other (Exterior/exterior.mtl names ..\BuildingTextures\...), so each is
	# extracted into a directory of its name, side by side
	bistro=$out/bistro
	if [[ ! -f $bistro/.extracted ]]; then
		if compgen -G "$out/bistro-parts/*.zip" > /dev/null; then
			echo "extracting bistro" >&2
			rm -rf "$bistro"
			mkdir -p "$bistro"
			for part in "$out"/bistro-parts/*.zip; do
				unzip -qo "$part" -d "$bistro/$(basename "$part" .zip)" || { echo "failed to extract $part" >&2; failed=1; }
			done
			[[ $failed -eq 0 ]] && touch "$bistro/.extracted"
		fi
	fi

	paths+=("$out"/*.zip)
	[[ -f $bistro/.extracted ]] && paths+=("$bistro")
fi

if [[ $gltf -eq 1 ]]; then
	out=$dir/glTF-Sample-Assets
	if [[ $(git -C "$out" rev-parse HEAD 2>/dev/null) != "$kGltfCommit" ]]; then
		echo "fetching glTF-Sample-Assets at $kGltfCommit" >&2
		mkdir -p "$out"
		{
			git -C "$out" init -q &&
			{ git -C "$out" remote get-url origin > /dev/null 2>&1 || git -C "$out" remote add origin "$kGltfRepository"; } &&
			git -C "$out" sparse-checkout set Models &&
			git -C "$out" fetch -q --depth 1 --filter=blob:none origin "$kGltfCommit" &&
			git -C "$out" checkout -q --detach FETCH_HEAD
		} || { echo "failed to fetch glTF-Sample-Assets" >&2; failed=1; }
	fi
	[[ -d $out/Models ]] && paths+=("$out/Models")
fi

[[ ${#paths[@]} -gt 0 ]] && printf '%s\n' "${paths[@]}"
exit $failed
