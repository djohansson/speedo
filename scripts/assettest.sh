#!/usr/bin/env bash
# Tests model and image loading on a set of assets.
#
# usage: scripts/assettest.sh [--client | --client-only] [--preset <name>] [--work <dir>] <zip or directory>...
#
# The test sets are downloaded by scripts/fetch-test-assets.sh, which prints their paths:
#   scripts/assettest.sh --client $(scripts/fetch-test-assets.sh)
#
# Zip archives are extracted to <work>/assets/<archive name> (the work dir defaults to a new temporary dir, and is
# kept). Then build/<preset>/assettest imports every model (.obj, .gltf, .glb) and image the way the client does and checks the
# results (see src/tools/assettest.cpp). With --client, the client also loads each model (and the first image next to
# it) through the full load + upload + draw path, and a zip archive of several models (a set of variants), or a
# subdirectory of a directory with several (e.g. a gltf sample model's encodings), as one, side by side (files the
# importers don't support are skipped there), with a user profile dir under <work>, and fails a model if the
# client crashes or prints load failures, failed asserts or validation errors (validation needs a debug preset).
# --client-only skips the assettest run (which is slow with a debug preset). The client needs a vulkan driver: set
# VK_DRIVER_FILES etc. as for running it by hand.
#
# Exits with 1 if anything failed. Logs are kept under <work>/logs.

set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
preset=
work=
client=0
check=1
inputs=()

while [[ $# -gt 0 ]]; do
	case $1 in
		--client) client=1 ;;
		--client-only) client=1; check=0 ;;
		--preset) preset=$2; shift ;;
		--work) work=$2; shift ;;
		-h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
		*) inputs+=("$1") ;;
	esac
	shift
done

if [[ ${#inputs[@]} -eq 0 ]]; then
	echo "usage: $0 [--client | --client-only] [--preset <name>] [--work <dir>] <zip or directory>..." >&2
	exit 2
fi

if [[ -z $preset ]]; then
	# the first configured release preset, else any
	preset=$(cd "$root/build" 2>/dev/null && ls -d *-release 2>/dev/null | head -1)
	[[ -z $preset ]] && preset=$(cd "$root/build" 2>/dev/null && ls -d */ 2>/dev/null | head -1)
	preset=${preset%/}
fi
build=$root/build/$preset
if [[ ! -x $build/assettest ]]; then
	echo "no assettest in $build: build preset '$preset' first" >&2
	exit 2
fi

# the vcpkg install tree for the preset's triplet (the preset name without its config)
config=${preset##*-}
triplet=${preset%-*}
lib=$root/install/$triplet/$([[ $config == debug ]] && echo debug/)lib
export DYLD_LIBRARY_PATH=$lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}
export LD_LIBRARY_PATH=$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export VK_LAYER_PATH=${VK_LAYER_PATH:-$root/install/$triplet/share/vulkan/explicit_layer.d}

work=${work:-$(mktemp -d "${TMPDIR:-/tmp}/assettest.XXXXXX")}
mkdir -p "$work/assets" "$work/logs"
echo "work dir: $work"

modelPattern=(\( -iname '*.obj' -o -iname '*.gltf' -o -iname '*.glb' \) ! -name '._*' ! -path '*/__MACOSX/*')

dirs=()
setArchives=() # zip archives of several models (sets of variants), which the client loads as one, side by side
setFolders=() # the same for the subdirectories of a directory, e.g. a gltf sample model's encodings
for input in "${inputs[@]}"; do
	if [[ -d $input ]]; then
		dir=$(cd "$input" && pwd -P)
		dirs+=("$dir")
		for sub in "$dir"/*/; do
			[[ -d $sub && $(find "$sub" -type f "${modelPattern[@]}" | wc -l) -gt 1 ]] && setFolders+=("${sub%/}")
		done
	elif [[ $input == *.zip ]]; then
		name=$(basename "$input" .zip)
		if [[ ! -d $work/assets/$name ]]; then
			echo "extracting $input"
			mkdir -p "$work/assets/$name"
			unzip -qo "$input" -d "$work/assets/$name" || { echo "failed to extract $input" >&2; exit 1; }
		fi
		dirs+=("$work/assets/$name")
		[[ $(find "$work/assets/$name" -type f "${modelPattern[@]}" | wc -l) -gt 1 ]] &&
			setArchives+=("$(cd "$(dirname "$input")" && pwd -P)/$(basename "$input")")
	else
		echo "not a zip archive or directory: $input" >&2
		exit 2
	fi
done

failed=0

if [[ $check -eq 1 ]]; then
	"$build/assettest" "${dirs[@]}" | tee "$work/logs/assettest.log"
	[[ ${PIPESTATUS[0]} -eq 0 ]] || failed=1
fi

if [[ $client -eq 1 ]]; then
	if [[ ! -x $build/client ]]; then
		echo "no client in $build" >&2
		exit 2
	fi

	# the client exits this many frames after its loads have finished
	export SPEEDO_AUTOLOAD_EXIT=${SPEEDO_AUTOLOAD_EXIT:-30}
	timeout=${ASSETTEST_CLIENT_TIMEOUT:-600}

	pass=0
	fail=0

	# runs the client on a model (a file, or a zip archive), drawn with image on the default material if given
	runClient()
	{
		local model=$1 image=$2 label=$3
		local log=$work/logs/client-$label.log

		# glfw finds no monitors while the display sleeps (on macos), and the client then fails to start: wake it, and
		# keep it awake while the client runs
		command -v caffeinate > /dev/null && caffeinate -u -t 1

		local start=$(date +%s)
		local env=(SPEEDO_AUTOLOAD_MODEL="$model")
		[[ -n $image ]] && env+=(SPEEDO_AUTOLOAD_IMAGE="$image")
		env "${env[@]}" "$build/client" -u "$work/user" > "$log" 2>&1 &
		local pid=$!
		command -v caffeinate > /dev/null && caffeinate -d -i -w $pid &
		while kill -0 $pid 2>/dev/null && (( $(date +%s) - start < timeout )); do
			sleep 1
		done
		if kill -0 $pid 2>/dev/null; then
			kill -9 $pid 2>/dev/null
			echo "timed out after ${timeout}s" >> "$log"
		fi
		wait $pid
		local status=$?

		local problems=$(grep -E "Failed to load (model|image|archive)|\(errno: |VUID-|UNASSIGNED-|timed out after" "$log" | sort | uniq -c | head -5)
		if [[ $status -ne 0 || -n $problems ]]; then
			echo "FAIL client $model (exit $status, $(( $(date +%s) - start ))s, log: $log)"
			[[ -n $problems ]] && echo "$problems" | sed 's/^/    /'
			fail=$((fail + 1))
		else
			echo "PASS client $model ($(( $(date +%s) - start ))s)"
			pass=$((pass + 1))
		fi
	}

	# an archive of several models is one run, with its first image (its textures can be missing, or its models leave
	# the default material to an image, as sphere.zip's do)
	setDirs=()
	for archive in ${setArchives[@]+"${setArchives[@]}"}; do
		dir=$work/assets/$(basename "$archive" .zip)
		setDirs+=("$dir")
		image=$(find "$dir" -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.tga' \) ! -name '._*' | sort | head -1)
		runClient "$archive" "$image" "$(basename "$archive" .zip)"
	done
	for folder in ${setFolders[@]+"${setFolders[@]}"}; do
		setDirs+=("$folder")
		runClient "$folder" "" "$(basename "$folder" | tr ' ' '_')"
	done

	while IFS= read -r -d '' model; do
		dir=$(dirname "$model")
		skip=0
		for setDir in ${setDirs[@]+"${setDirs[@]}"}; do
			[[ $model == "$setDir"/* ]] && skip=1
		done
		[[ $skip -eq 1 ]] && continue

		# an obj model's textures can be missing, so it is drawn with an image next to it on the default material
		image=
		[[ $model == *.[oO][bB][jJ] ]] &&
			image=$(find "$dir" -maxdepth 1 -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.tga' \) ! -name '._*' | sort | head -1)
		runClient "$model" "$image" "$(basename "$dir")-$(basename "$model" | tr ' ' '_')"
	done < <(find "${dirs[@]}" -type f "${modelPattern[@]}" -print0 | sort -z)

	echo "client: $pass pass, $fail fail."
	[[ $fail -eq 0 ]] || failed=1
fi

exit $failed
