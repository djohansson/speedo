#!/usr/bin/env bash
# Tests model and image loading on a set of assets.
#
# usage: scripts/assettest.sh [--client | --client-only] [--preset <name>] [--work <dir>] <zip or directory>...
#
# Zip archives are extracted to <work>/assets/<archive name> (the work dir defaults to a new temporary dir, and is
# kept). Then build/<preset>/assettest imports every .obj file and image the way the client does and checks the
# results (see src/tools/assettest.cpp). With --client, the client also loads each model (and the first image next to
# it) through the full load + upload + draw path, with a user profile dir under <work>, and fails a model if the
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

dirs=()
for input in "${inputs[@]}"; do
	if [[ -d $input ]]; then
		dirs+=("$(cd "$input" && pwd -P)")
	elif [[ $input == *.zip ]]; then
		name=$(basename "$input" .zip)
		if [[ ! -d $work/assets/$name ]]; then
			echo "extracting $input"
			mkdir -p "$work/assets/$name"
			unzip -qo "$input" -d "$work/assets/$name" || { echo "failed to extract $input" >&2; exit 1; }
		fi
		dirs+=("$work/assets/$name")
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

	while IFS= read -r -d '' model; do
		dir=$(dirname "$model")
		image=$(find "$dir" -maxdepth 1 -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.tga' \) ! -name '._*' | sort | head -1)
		log=$work/logs/client-$(basename "$dir")-$(basename "$model" | tr ' ' '_').log

		# glfw finds no monitors while the display sleeps (on macos), and the client then fails to start: wake it, and
		# keep it awake while the client runs
		command -v caffeinate > /dev/null && caffeinate -u -t 1

		start=$(date +%s)
		env=(SPEEDO_AUTOLOAD_MODEL="$model")
		[[ -n $image ]] && env+=(SPEEDO_AUTOLOAD_IMAGE="$image")
		env "${env[@]}" "$build/client" -u "$work/user" > "$log" 2>&1 &
		pid=$!
		command -v caffeinate > /dev/null && caffeinate -d -i -w $pid &
		while kill -0 $pid 2>/dev/null && (( $(date +%s) - start < timeout )); do
			sleep 1
		done
		if kill -0 $pid 2>/dev/null; then
			kill -9 $pid 2>/dev/null
			echo "timed out after ${timeout}s" >> "$log"
		fi
		wait $pid
		status=$?

		problems=$(grep -E "Failed to load|\(errno: |VUID-|UNASSIGNED-|timed out after" "$log" | sort | uniq -c | head -5)
		if [[ $status -ne 0 || -n $problems ]]; then
			echo "FAIL client $model (exit $status, $(( $(date +%s) - start ))s, log: $log)"
			[[ -n $problems ]] && echo "$problems" | sed 's/^/    /'
			fail=$((fail + 1))
		else
			echo "PASS client $model ($(( $(date +%s) - start ))s)"
			pass=$((pass + 1))
		fi
	done < <(find "${dirs[@]}" -type f -iname '*.obj' ! -name '._*' ! -path '*/__MACOSX/*' -print0 | sort -z)

	echo "client: $pass pass, $fail fail."
	[[ $fail -eq 0 ]] || failed=1
fi

exit $failed
