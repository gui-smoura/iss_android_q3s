#!/usr/bin/env bash
# Adapted from Bloodborne-Android build_bionic_deps.sh, 2e472653.
# Third-party libraries for the Bionic (Android NDK) build, into .local-deps/android/bionic-prefix.
# Pinned Bionic dependencies; fmt 12.1.0 supports the NDK r29 C++23 toolchain.
# Sources are downloaded to .local-deps/android/bionic-src, builds go to bionic-build.
# Host-side prerequisites: curl, tar, cmake, ninja, make, python3.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
root=$PWD/.local-deps/android
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_PREFIX_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
ndk=${ISS_NDK:-${ANDROID_NDK_HOME:-$root/sdk/ndk/29.0.14206865}}
api=${ISS_BIONIC_API:-30}
prefix=$root/bionic-prefix src=$root/bionic-src bld=$root/bionic-build
[[ -f $ndk/build/cmake/android.toolchain.cmake ]] || { echo "No NDK: $ndk" >&2; exit 1; }
mkdir -p "$prefix" "$src" "$bld"
J=${ISS_JOBS:-2}
run() { "$@"; }

fetch() { # name url archive-extension
    local dir=$src/$1
    [[ -d $dir ]] && return
    rm -rf -- "$dir.tmp"
    curl -fsSL "$2" -o "$src/$1.tar" && mkdir -p "$dir.tmp" && tar --no-same-owner -xf "$src/$1.tar" -C "$dir.tmp" --strip-components=1 \
        && mv "$dir.tmp" "$dir" && rm "$src/$1.tar"
}
cm() { # name source-dir extra cmake args...
    local name=$1 sdir=$2; shift 2
    [[ -f $prefix/.done-$name ]] && return
    cmake -S "$sdir" -B "$bld/$name" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" "$@" >/dev/null
    run cmake --build "$bld/$name" -j "$J" >"$bld/$name.log" 2>&1 || { tail -30 "$bld/$name.log" >&2; exit 1; }
    cmake --install "$bld/$name" >/dev/null
    touch "$prefix/.done-$name"
}

fetch fmt-12.1.0 https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
fetch zycore-1.5.2 https://github.com/zyantific/zycore-c/archive/refs/tags/v1.5.2.tar.gz
fetch zydis-4.1.1 https://github.com/zyantific/zydis/archive/refs/tags/v4.1.1.tar.gz
fetch xxhash-0.8.3 https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
fetch miniz-3.1.2 https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
fetch Vulkan-Headers-1.4.363 https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz
fetch SPIRV-Headers-1.4.309 https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.309.0.tar.gz
fetch ffmpeg-7.1.5 https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz
# Pinned ARM64 userspace contexts (Android libc has no makecontext/swapcontext).
fetch libucontext-49e671dd52ff6791295d8161ad3b6da7dc5f6f9d https://github.com/kaniini/libucontext/archive/49e671dd52ff6791295d8161ad3b6da7dc5f6f9d.tar.gz
fetch SDL3-3.2.16 https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.16.tar.gz
fetch freetype-2.13.3 https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
fetch OpenXR-SDK-1.1.65 https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.65.tar.gz

# Header-only: vulkan-hpp (the NDK ships Vulkan 1.3.275 C headers without it) and SPIR-V.
cm vulkan-headers "$src/Vulkan-Headers-1.4.363"
cm spirv-headers "$src/SPIRV-Headers-1.4.309"
cm fmt-12.1.0 "$src/fmt-12.1.0" -DFMT_TEST=OFF -DFMT_DOC=OFF
cm zycore "$src/zycore-1.5.2" -DBUILD_SHARED_LIBS=OFF
# The release tarball lacks the zycore submodule: build against the same Zycore source tree.
[[ -f $src/zydis-4.1.1/dependencies/zycore/CMakeLists.txt ]] || cp -a "$src/zycore-1.5.2/." "$src/zydis-4.1.1/dependencies/zycore/"
cm zydis "$src/zydis-4.1.1" -DBUILD_SHARED_LIBS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_TOOLS=OFF \
    -DZYDIS_BUILD_DOXYGEN=OFF
cm xxhash "$src/xxhash-0.8.3/cmake_unofficial" -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
cm miniz "$src/miniz-3.1.2" -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_PROJECT=ON
# The ISS loader (not the headless Vulkan renderer) still uses SDL3 audio/pad
# and libSceFont uses FreeType; build ARM64 static libraries, no host linkage.
cm freetype-2.13.3 "$src/freetype-2.13.3" -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HARFBUZZ=ON
cm SDL3-3.2.16 "$src/SDL3-3.2.16" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=ON
cm openxr-loader "$src/OpenXR-SDK-1.1.65" -DBUILD_LOADER=ON -DDYNAMIC_LOADER=ON \
    -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
    -DBUILD_WITH_SYSTEM_JSONCPP=OFF -DINSTALL_TO_ARCHITECTURE_PREFIXES=OFF
mkdir -p "$prefix/lib" "$prefix/include/openxr" "$prefix/share/licenses/openxr"
if [[ ! -s $prefix/lib/libopenxr_loader.so ]]; then
    if [[ -s $prefix/jni/arm64-v8a/libopenxr_loader.so ]]; then
        cp "$prefix/jni/arm64-v8a/libopenxr_loader.so" "$prefix/lib/libopenxr_loader.so"
    elif [[ -s $bld/openxr-loader/src/loader/libopenxr_loader.so ]]; then
        cp "$bld/openxr-loader/src/loader/libopenxr_loader.so" "$prefix/lib/libopenxr_loader.so"
    fi
fi
if [[ ! -s $prefix/include/openxr/openxr.h ]]; then
    cp -a "$src/OpenXR-SDK-1.1.65/include/openxr/." "$prefix/include/openxr/"
fi
if [[ -f $src/OpenXR-SDK-1.1.65/LICENSE ]]; then
    cp "$src/OpenXR-SDK-1.1.65/LICENSE" "$prefix/share/licenses/openxr/LICENSE"
elif [[ -f $src/OpenXR-SDK-1.1.65/LICENSES/Apache-2.0.txt ]]; then
    cp "$src/OpenXR-SDK-1.1.65/LICENSES/Apache-2.0.txt" "$prefix/share/licenses/openxr/LICENSE"
fi
test -s "$prefix/lib/libopenxr_loader.so"
test -s "$prefix/include/openxr/openxr.h"
# Native ARM64 coroutine switching for preserved GR2-style HLE fibers. This
# library uses a private freestanding context struct; no glibc symbols/ABI leak.
if [[ ! -f $prefix/.done-libucontext ]]; then
    tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
    ctx=$src/libucontext-49e671dd52ff6791295d8161ad3b6da7dc5f6f9d
    run make -C "$ctx" ARCH=aarch64 FREESTANDING=yes EXPORT_UNPREFIXED=no \
        CC="$tc/bin/aarch64-linux-android$api-clang" AR="$tc/bin/llvm-ar" \
        CFLAGS="-O2 -fPIC" libucontext.a >"$bld/libucontext.log" 2>&1 \
        || { tail -60 "$bld/libucontext.log" >&2; exit 1; }
    mkdir -p "$prefix/include/libucontext" "$prefix/lib" "$prefix/share/licenses/libucontext"
    cp "$ctx/include/libucontext/libucontext.h" "$prefix/include/libucontext/"
    cp "$ctx/arch/aarch64/include/libucontext/bits.h" "$prefix/include/libucontext/"
    cp "$ctx/libucontext.a" "$prefix/lib/"
    cp "$ctx/LICENSE" "$prefix/share/licenses/libucontext/"
    touch "$prefix/.done-libucontext"
fi
# ARM64 NEON FFT accesses external ff_tx_tab_* tables with a non-PIC ADRP.
# --enable-pic is insufficient for handwritten assembly; rewrite only this
# cross-object access through the GOT, keeping the accelerated NEON FFT.
python3 - "$src/ffmpeg-7.1.5/libavutil/aarch64/tx_float_neon.S" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
source = p.read_text()
old = r"        movrel          \re, X(ff_tx_tab_\len\()_float)"
new = r"""#if CONFIG_PIC && !defined(__APPLE__) && !defined(_WIN32)
        adrp            \re, :got:X(ff_tx_tab_\len\()_float)
        ldr             \re, [\re, :got_lo12:X(ff_tx_tab_\len\()_float)]
#else
        movrel          \re, X(ff_tx_tab_\len\()_float)
#endif"""
if source.count(old) != 1 and new not in source:
    raise SystemExit("FFmpeg 7.1.5 ARM64 source changed; refusing unverified PIC patch")
if new not in source:
    p.write_text(source.replace(old, new, 1))
    print("FFmpeg 7.1.5 Bionic: accelerated NEON FFT now uses GOT for external tables")
PY
# Existing cache prefixes contain FFmpeg built before this repair. Only its
# archives must be rebuilt; other already-validated dependencies stay cached.
if [[ ! -f $prefix/.done-ffmpeg-got-pic-v1 ]]; then
    rm -f "$prefix/.done-ffmpeg"
    rm -rf "$bld/ffmpeg"
fi
# FFmpeg (avplayer/videodec): static, PIC, no programs, only built-in codecs/demuxers.
if [[ ! -f $prefix/.done-ffmpeg ]]; then
    tc=$ndk/toolchains/llvm/prebuilt/linux-x86_64
    mkdir -p "$bld/ffmpeg" && pushd "$bld/ffmpeg" >/dev/null
    "$src/ffmpeg-7.1.5/configure" --prefix="$prefix" --enable-cross-compile --target-os=android --arch=aarch64 \
        --cc="$tc/bin/aarch64-linux-android$api-clang" --cxx="$tc/bin/aarch64-linux-android$api-clang++" \
        --ar="$tc/bin/llvm-ar" --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" --nm="$tc/bin/llvm-nm" \
        --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-avfilter --disable-network --disable-autodetect --disable-mediacodec --disable-jni >"$bld/ffmpeg-configure.log" 2>&1 \
        || { tail -30 "$bld/ffmpeg-configure.log" >&2; exit 1; }
    run make -j"$J" >"$bld/ffmpeg.log" 2>&1 || { tail -30 "$bld/ffmpeg.log" >&2; exit 1; }
    make install >/dev/null
    # configure's Android target links libandroid/libmediandk for MediaCodec/JNI, both disabled here.
    sed -i 's/ -landroid -lmediandk//' "$prefix"/lib/pkgconfig/libavutil.pc
    popd >/dev/null
    touch "$prefix/.done-ffmpeg"
fi
touch "$prefix/.done-ffmpeg-got-pic-v1"
echo "Bionic deps in $prefix"
