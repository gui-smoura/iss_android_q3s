#!/usr/bin/env bash
# Build the actual inFAMOUS Bionic game loader and Android direct-present JNI.
set -Eeuo pipefail
trap 'echo "ISS Bionic native runtime failed at $LINENO: $BASH_COMMAND" >&2' ERR
cd -- "$(dirname "$0")/../../.."
ndk=${ISS_NDK:-${ANDROID_NDK_HOME:?Set ISS_NDK}}
api=${ISS_BIONIC_API:-30}
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
prefix=$PWD/.local-deps/android/bionic-prefix
out=$PWD/out/bionic
cc="$llvm/bin/aarch64-linux-android${api}-clang"
ar="$llvm/bin/llvm-ar"
# Compile the complete game-specific HLE/launcher sources before the expensive GPU link.
# --compile-only catches later-stage Android C build errors in the current CI run.
if [[ ${1:-} != --compile-only ]]; then
  test -s "$out/lib/libisscpu.so"
  test -s "$out/lib/libtlggpu.so"
fi
test -s "$prefix/lib/libavcodec.a"
test -s "$prefix/lib/libSDL3.a"
test -s "$prefix/lib/libfreetype.a"
test -s "$prefix/lib/libucontext.a"
test -s "$prefix/lib/libopenxr_loader.so"
test -s "$prefix/include/openxr/openxr.h"
mkdir -p "$out/bin" "$out/atrac9" "$out/runtime-objects" "$out/lib"
for source in port/third_party/LibAtrac9/C/src/*.c; do
  "$cc" -std=c99 -O2 -w -fPIC -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
"$ar" rcs "$out/libatrac9.a" "$out"/atrac9/*.o
includes=(-Iport -Iport/src -Iport/gpu -I"$prefix/include" -I"$prefix/include/freetype2")
objects=()
compile_failed=0
build_commit=$(git rev-parse --verify HEAD)
for source in port/src/probe.c port/src/runtime*.c; do
  object="$out/runtime-objects/$(basename "${source%.c}").o"
  # Continue on errors and identify *every* incompatible HLE source in one pass.
  if ! "$cc" -std=gnu11 -O2 -g -Wall -Wextra -pthread -fPIE \
      "-DTLG_BUILD_COMMIT=\"$build_commit\"" "${includes[@]}" -c "$source" -o "$object"; then
    printf 'ISS Bionic C compilation failed: %s\n' "$source" >&2
    compile_failed=1
  fi
  objects+=("$object")
done
if ! "$cc" -std=gnu11 -O2 -Wall -Wextra -fPIC -Itools/android/present -c \
    tools/android/bionic/present/isspresent.c -o "$out/runtime-objects/isspresent.o"; then
  printf 'ISS Bionic C compilation failed: %s\n' "tools/android/bionic/present/isspresent.c" >&2
  compile_failed=1
fi
if ! "$cc" -std=gnu11 -O2 -Wall -Wextra -fPIC -I"$prefix/include" -c \
    tools/android/bionic/openxr/issopenxr.c -o "$out/runtime-objects/issopenxr.o"; then
  printf 'ISS Bionic C compilation failed: %s\n' "tools/android/bionic/openxr/issopenxr.c" >&2
  compile_failed=1
fi
if [[ $compile_failed != 0 ]]; then
  echo 'Bionic C/HLE preflight failed; all affected source files reported above' >&2
  exit 1
fi
if [[ ${1:-} = --compile-only ]]; then
  echo "ISS Bionic C/HLE/launcher objects compile successfully"; exit 0
fi
# The Vulkan test in the desktop probe calls the system Vulkan loader directly.
# Bionic must instead always use the configured Turnip HAL through the ISS Vulkan driver shim.
cat > "$out/runtime-objects/vulkan_smoke_bionic.c" <<'EOF'
#include <stdio.h>
int vulkan_smoke(void) {
  fputs("Use the ISS Bionic Vulkan HAL smoke path; the desktop Vulkan test is not available.\n", stderr);
  return 1;
}
EOF
"$cc" -std=c11 -O2 -c "$out/runtime-objects/vulkan_smoke_bionic.c" -o "$out/runtime-objects/vulkan_smoke_bionic.o"
"$cc" -pie -rdynamic "${objects[@]}" "$out/runtime-objects/vulkan_smoke_bionic.o" \
  "$out/libatrac9.a" \
  "$prefix/lib/libavformat.a" "$prefix/lib/libavcodec.a" "$prefix/lib/libswscale.a" \
  "$prefix/lib/libswresample.a" "$prefix/lib/libavutil.a" \
  "$prefix/lib/libSDL3.a" "$prefix/lib/libfreetype.a" \
  "$prefix/lib/libucontext.a" \
  -L"$out/lib" -ltlggpu -lisscpu -lc++_shared -lm -lz -ldl -landroid -llog \
  -lOpenSLES -lEGL -lGLESv2 -pthread -Wl,-rpath,'$ORIGIN/../lib' \
  -o "$out/bin/iss-probe"
"$cc" -std=gnu11 -O2 -Wall -Wextra -pie tools/android/build/kgsl_chip_id.c -o "$out/bin/kgsl-chip-id"
"$cc" -std=gnu11 -O2 -Wall -Wextra -fPIC -shared "$out/runtime-objects/isspresent.o" \
  -landroid -lnativewindow -lEGL -lGLESv2 -llog -ldl -pthread \
  -o "$out/lib/libisspresent.so"
cp "$prefix/lib/libopenxr_loader.so" "$out/lib/libopenxr_loader.so"
"$cc" -std=gnu11 -O2 -Wall -Wextra -fPIC -shared "$out/runtime-objects/issopenxr.o" \
  "$out/lib/libopenxr_loader.so" -landroid -lEGL -lGLESv2 -llog -pthread \
  -Wl,-rpath,'$ORIGIN' -o "$out/lib/libissopenxr.so"
for file in "$out/bin/iss-probe" "$out/bin/kgsl-chip-id" "$out/lib/libisspresent.so" "$out/lib/libopenxr_loader.so" "$out/lib/libissopenxr.so"; do
  "$llvm/bin/llvm-readelf" -h "$file" | grep -q 'Machine:.*AArch64'
done
echo "Compiled the inFAMOUS Bionic probe, KGSL helper, direct AHardwareBuffer bridge and OpenXR wrapper."
