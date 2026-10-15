#!/usr/bin/env bash
# Task 513 Stage 1. Builds the platform-neutral core and its execution-free
# probes for wasm32 with Emscripten.
#
# Issue #67: and the engine, on the emulated execution model, where the guest
# runs on the rex86 CPU core and no guest byte runs on the host CPU. `repiu` is
# the loader, run under Node for now:
#
#     node build/web_wasm/repiu.js pumpitea      (from the repository root)
#
# See docs/design/20260828-513-web-wasm-execution.md and
# docs/design/20261015-i067-emscripten-headless.md.
set -euo pipefail

configuration="Release"
targets=()
build_dir=""
rex86_source=""

usage()
{
    cat <<'USAGE'
usage: build_web_wasm.sh [--config Debug|Release|RelWithDebInfo|MinSizeRel]
                         [--build-dir PATH] [--rex86-source PATH]
                         [--target NAME]...
Builds into build/web_wasm unless --build-dir names another directory (under
WSL, one on the Linux filesystem builds far faster than one under /mnt/c).
--rex86-source builds a local rex86 checkout instead of the pinned tag. With no
--target, every default target is built.

Release by default, unlike the Linux script: an Emscripten Debug build of this
core is large and slow to link, and nothing at this stage is being debugged.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            configuration="${2:?--config needs a value}"
            shift 2
            ;;
        --target)
            targets+=("${2:?--target needs a value}")
            shift 2
            ;;
        --build-dir)
            build_dir="${2:?--build-dir needs a value}"
            shift 2
            ;;
        --rex86-source)
            rex86_source="${2:?--rex86-source needs a value}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${build_dir:-$root/build/web_wasm}"
rex86_options=()
if [[ -n "$rex86_source" ]]; then
    rex86_options+=("-DFETCHCONTENT_SOURCE_DIR_REX86=$rex86_source")
fi

# The toolchain is checked and named up front, for the same reason
# build_linux_i386.sh checks for the 32-bit compiler: without it the failure
# arrives as hundreds of header errors with the actual cause buried.
#
# emsdk is not a package. It is a checkout that must also be activated in the
# current shell, and the second half is the one that gets forgotten -- the
# directory exists, `emcmake` does not, and the error says nothing about why.
if ! command -v emcmake > /dev/null 2>&1; then
    emsdk_root="${EMSDK:-$HOME/emsdk}"
    if [[ -f "$emsdk_root/emsdk_env.sh" ]]; then
        cat >&2 <<NEEDS
emsdk is installed at $emsdk_root but not active in this shell:

    source "$emsdk_root/emsdk_env.sh"

NEEDS
    else
        cat >&2 <<'NEEDS'
Emscripten is not available. To install it:

    git clone --depth 1 https://github.com/emscripten-core/emsdk.git ~/emsdk
    cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
    source ~/emsdk/emsdk_env.sh

NEEDS
    fi
    exit 1
fi

# SDL3 is in the link because the public input headers name SDL_Keycode, so the
# core's interface carries it whether or not a window is ever opened. Emscripten
# is one of SDL3's own targets, so this needs no special handling -- but the
# desktop video and audio backends do not apply, and leaving them on makes SDL
# probe for X11 headers that are not part of an Emscripten sysroot.
#
# Task 513 Stage 5 is where a real browser video backend is chosen. Until then
# nothing here opens a window.
sdl_options=(
    -DSDL_X11=OFF
    -DSDL_WAYLAND=OFF
    -DSDL_PULSEAUDIO=OFF
    -DSDL_ALSA=OFF
)

emcmake cmake -S "$root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE="$configuration" \
    "${sdl_options[@]}" \
    "${rex86_options[@]}"

# The job count is named rather than left to a bare `--parallel`, for the reason
# build_linux_i386.sh records: an unlimited `-j` took the WSL VM down three
# times by starting every ready target at once.
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc 2>/dev/null || echo 2)}"
build_arguments=("--build" "$build_dir" "--parallel" "$jobs")
for name in "${targets[@]:-}"; do
    if [[ -n "$name" ]]; then
        build_arguments+=("--target" "$name")
    fi
done
cmake "${build_arguments[@]}"

echo
echo "Output directory: $build_dir"
echo "Run the probe with:  node $build_dir/repiu_core_probe.js"
