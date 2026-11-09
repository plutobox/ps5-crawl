#!/usr/bin/env bash
# Package Crawl as a native PS5 title (a folder app with eboot.bin) for
# ShadowMountPlus / ProsperoStore.
#
# Run from crawl-ref/source after building the game for the native target:
#
#   make PS5_NATIVE=y crawl-native.objs
#   bash ps5/package-native.sh [--build]
#
# This follows ps5-opengl's SDL2 folder packager (integration/SDL2/folder.py):
# it copies ps5-native-app-boilerplate into build-ps5/app, adds ps5-opengl's
# native runtime (app heap, linker script), the game, its libraries and data,
# and writes param.json. With --build it then runs the boilerplate's build,
# which links eboot.bin and writes build-ps5/app/dist/$TITLE_ID/.
#
# Inputs (defaults match the PS5_NATIVE settings in the Makefile):
#   PS5_NATIVE_TEMPLATE  ps5-native-app-boilerplate checkout with .deps set up
#   PS5_OPENGL_REPO      ps5-opengl checkout (for native-app/)
#   PS5_OPENGL_PREFIX    installed ps5-opengl SDK (include/, lib/)
#   PS5_SDL2_PREFIX      SDL2 built by ps5-opengl's integration/SDL2/build.py
#   PS5_PACBREW_PREFIX   the boilerplate's PacBrew ports (user/homebrew), from
#                        tools/setup-pacbrew-dependencies.sh --all
#   PS5_PLATFORM_LIB     libps5platform.a from mihawk-99/PS5_PayloadSDK's
#                        platform layer (make -C platform), for the C library
#                        functions a title lacks or is refused
#   TITLE_ID, TITLE_NAME, CONTENT_VERSION
set -euo pipefail

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
template=${PS5_NATIVE_TEMPLATE:-$HOME/ps5-native-app-boilerplate}
opengl_repo=${PS5_OPENGL_REPO:-$HOME/ps5-opengl}
opengl=${PS5_OPENGL_PREFIX:-$HOME/ps5-opengl-sdk/ps5-opengl-sdk-1.0.2/sdk}
sdl2=${PS5_SDL2_PREFIX:-$HOME/ps5-opengl/build/sdl-native/sdk}
pacbrew=${PS5_PACBREW_PREFIX:-$template/.deps/pacbrew/v0.40.2/sysroot/user/homebrew}
platform=${PS5_PLATFORM_LIB:-$HOME/ps5-platform-sdk/platform/build/ps5/libps5platform.a}
title_id=${TITLE_ID:-PPSA92041}
title_name=${TITLE_NAME:-Dungeon Crawl Stone Soup}
content_version=${CONTENT_VERSION:-01.000.000}
out="$source_dir/build-ps5/app"

die() { echo "package-native: $*" >&2; exit 1; }

[[ $title_id =~ ^[A-Z]{4}[0-9]{5}$ ]] || die "bad TITLE_ID: $title_id"
[[ $content_version =~ ^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$ ]] ||
    die "bad CONTENT_VERSION: $content_version"
for path in "$template/tools/build.sh" "$template/.deps/native/ps5-payload-sdk" \
            "$opengl_repo/native-app/runtime_shims.c" \
            "$opengl/lib/libPS5OpenGLCore33.a" "$sdl2/lib/libSDL2.a" "$pacbrew/lib" \
            "$platform" "$source_dir/crawl-native.objs"; do
    [[ -e $path ]] || die "missing $path"
done

compiler=(env "PS5_PAYLOAD_SDK=$template/.deps/native/ps5-payload-sdk"
          sh "$template/tooling/prospero-clang18")

rm -rf "$out"
mkdir -p "$out"

# The boilerplate project, with ps5-opengl's native runtime.
for directory in runtime sce_sys tooling tools .deps/native; do
    mkdir -p "$out/$(dirname "$directory")"
    cp -a "$template/$directory" "$out/$directory"
done
sdk="$out/.deps/native/ps5-payload-sdk"
mkdir -p "$out/src" "$out/vendor" "$out/build/native-imports"
# cts_runtime_shims.c provides the locale functions libc++ needs, which the
# title runtime lacks; native_shims.c adds the two it does not cover. ime.c
# opens the system's on-screen keyboard for Crawl's text prompts.
cp "$opengl_repo/native-app/runtime_shims.c" "$opengl_repo/native-app/app_heap.c" \
   "$opengl_repo/native-app/cts_runtime_shims.c" "$source_dir/ps5/native_shims.c" \
   "$source_dir/ps5/ime.c" "$out/src/"
cp "$out/tooling/native/ps5-pie.ld" "$out/tooling/native/ps5-pie-base.ld"
cp "$opengl_repo/native-app/ps5-pie.ld" "$opengl_repo/native-app/app-symbols.map" \
   "$out/tooling/native/"
cp "$opengl/lib/libSceAgc.so" "$opengl/lib/libSceAgcDriver.so" "$sdk/target/lib/"

replace_once() {
    local file=$1 old=$2 new=$3
    python3 - "$file" "$old" "$new" <<'EOF'
import sys
path, old, new = sys.argv[1:]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"package-native: template changed, cannot patch {path}: {old}")
open(path, "w").write(text.replace(old, new))
EOF
}
# As in ps5-opengl's folder.py: a bounded heap, prebuilt dependencies, and
# the app heap's allocator wrappers.
replace_once "$out/tooling/native/sce_module_writer.cpp" \
    'write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());' \
    'write_u64(result.data, result.heap_size, 0x10000000ULL);'
replace_once "$out/tools/build.sh" \
    'bash "$root/tools/setup-native-dependencies.sh" >/dev/null' \
    'test -x "$root/.deps/native/ps5-payload-sdk/bin/prospero-lld"'
# The C library functions a title lacks (no system module it loads exports
# them), is refused (the sandbox fails them, or a raw system call raises
# SIGSYS) or gets wrong, bound to the platform layer's ps5_ versions. A title
# must not define libc's own names, so they are bound at link time.
# Not sysconf: ps5_sysconf calls sysconf, which the binding would turn into
# a call to itself.
platform_functions=(access closedir fchown fork getcwd isatty localeconv
                    localtime_r mkstemp opendir readdir readlink regcomp
                    regerror regexec regfree strcasestr)
defsyms=()
for function in "${platform_functions[@]}"; do
    defsyms+=("--defsym=$function=ps5_$function")
done
# lstat fails for a title too, and the platform layer has none.
defsyms+=("--defsym=lstat=crawl_ps5_lstat")
replace_once "$out/tools/build.sh" \
    '--eh-frame-hdr \' \
    "--eh-frame-hdr --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size --wrap=fopen ${defsyms[*]} \\"

# The link: every game object, then the libraries in one group.
compiler_rt="$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
# PacBrew's PS5 builds: SDL2_image (with the WebP it was built with),
# freetype (with bzip2), libpng, zlib, Lua and SQLite. SDL2 is ps5-opengl's.
libraries=("$pacbrew/lib/libSDL2_image.a" "$pacbrew/lib/libwebpdemux.a"
           "$pacbrew/lib/libwebp.a" "$pacbrew/lib/libsharpyuv.a"
           "$pacbrew/lib/libfreetype.a" "$pacbrew/lib/libbz2.a"
           "$pacbrew/lib/libpng16.a" "$pacbrew/lib/libz.a"
           "$pacbrew/lib/liblua.a" "$pacbrew/lib/libsqlite3.a" "$platform"
           "$sdl2/lib/libSDL2.a" "$opengl/lib/libPS5OpenGLCore33.a"
           "$sdk/target/lib/libunwind.a" "$sdk/target/lib/libc++abi.a"
           "$sdk/target/lib/libc++.a" "$compiler_rt")
for path in "${libraries[@]}"; do
    [[ -f $path ]] || die "missing link input: $path"
done
{
    printf 'SEARCH_DIR("%s")\nSEARCH_DIR("%s")\n' "$sdk/target/lib" "$opengl/lib"
    printf 'INPUT(\n'
    while IFS= read -r object; do
        [[ -f $object ]] || die "missing object: $object"
        printf '  "%s"\n' "$object"
    done < "$source_dir/crawl-native.objs"
    printf ')\nGROUP(\n'
    printf '  "%s"\n' "${libraries[@]}"
    printf ')\n'
} > "$out/vendor/libcrawl.a"

# Game data, read-only in the title at /app0/assets.
make -C "$source_dir" --no-print-directory PS5_NATIVE=y install-data \
    DESTDIR="$out/build/data-stage" >/dev/null
cp -a "$out/build/data-stage/app0/assets" "$out/assets"

# Title metadata and art.
rm -rf "$out/sce_sys"
mkdir -p "$out/sce_sys"
cp "$source_dir/dat/tiles/stone_soup_icon-512x512.png" "$out/sce_sys/icon0.png"
python3 - "$opengl_repo/native-app/param.json" "$out/sce_sys/param.json" \
    "$title_id" "$title_name" "$content_version" <<'EOF'
import json, sys
template, output, title_id, name, version = sys.argv[1:]
param = json.load(open(template))
param["titleId"] = title_id
param["conceptId"] = title_id[4:]
param["contentId"] = f"IV9999-{title_id}_00-CRAWLSTONESOUP00"
param["contentVersion"] = version
param["localizedParameters"]["en-US"]["titleName"] = name
json.dump(param, open(output, "w"), indent=2)
open(output, "a").write("\n")
EOF

echo "Prepared $out ($title_id)"
if [[ ${1:-} == --build ]]; then
    (cd "$out" &&
     env APP_STATIC_ARCHIVES=vendor/libcrawl.a APP_DEFINITIONS= APP_INCLUDE_PATHS= \
        PACBREW_PACKAGES= PACBREW_INCLUDE_PATHS= PACBREW_STATIC_ARCHIVES= \
        APP_RUNTIME_MODULES= PS5_PAYLOAD_SDK="$sdk" \
        bash "$out/tools/build.sh" Folder)
    echo "Title folder: $out/dist/$title_id"
fi
