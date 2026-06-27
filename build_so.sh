#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  build_so.sh  —  Build library.so (arm64-v8a) only
#  No APK, no Gradle, just ndk-build
# ═══════════════════════════════════════════════════════════════

set -e

# ── WARNA OUTPUT ───────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

echo -e "${BOLD}═══════════════════════════════════════${NC}"
echo -e "${BOLD}   BUILD library.so  (arm64-v8a)       ${NC}"
echo -e "${BOLD}═══════════════════════════════════════${NC}"
echo

# ── LOKASI PROJECT ─────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JNI_DIR="$SCRIPT_DIR/app/src/main/jni"
OUT_DIR="$SCRIPT_DIR/output_so"

if [ ! -f "$JNI_DIR/Android.mk" ]; then
    error "Android.mk tidak ditemukan di: $JNI_DIR"
fi

# ── CARI NDK OTOMATIS ─────────────────────────────────────────
NDK_BUILD=""

find_ndk() {
    local base="$1"
    if [ -d "$base" ]; then
        # Cari ndk-build di dalam version folder
        local found
        found=$(find "$base" -maxdepth 3 -name "ndk-build" -type f 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            echo "$found"
            return 0
        fi
    fi
    return 1
}

info "Mencari NDK..."

# 1. Dari environment variable
if [ -n "$ANDROID_NDK_ROOT" ] && [ -f "$ANDROID_NDK_ROOT/ndk-build" ]; then
    NDK_BUILD="$ANDROID_NDK_ROOT/ndk-build"

elif [ -n "$NDK_ROOT" ] && [ -f "$NDK_ROOT/ndk-build" ]; then
    NDK_BUILD="$NDK_ROOT/ndk-build"

# 2. AndroidIDE paths (Android device)
elif found=$(find_ndk "/data/data/com.itsaky.androidide/files/home/android-sdk/ndk"); then
    NDK_BUILD="$found"
elif found=$(find_ndk "/sdcard/android-sdk/ndk"); then
    NDK_BUILD="$found"
elif found=$(find_ndk "$HOME/android-sdk/ndk"); then
    NDK_BUILD="$found"

# 3. local.properties di project
elif [ -f "$SCRIPT_DIR/local.properties" ]; then
    SDK_DIR=$(grep "^sdk.dir=" "$SCRIPT_DIR/local.properties" | cut -d'=' -f2 | tr -d '[:space:]')
    NDK_VERSION="24.0.8215888"
    CANDIDATE="$SDK_DIR/ndk/$NDK_VERSION/ndk-build"
    if [ -f "$CANDIDATE" ]; then
        NDK_BUILD="$CANDIDATE"
    else
        found=$(find_ndk "$SDK_DIR/ndk")
        [ -n "$found" ] && NDK_BUILD="$found"
    fi

# 4. Path umum Linux/Mac
elif found=$(find_ndk "$HOME/Android/Sdk/ndk"); then
    NDK_BUILD="$found"
elif found=$(find_ndk "/opt/android-sdk/ndk"); then
    NDK_BUILD="$found"
elif found=$(find_ndk "/usr/local/android-sdk/ndk"); then
    NDK_BUILD="$found"

# 5. ndk-build di PATH
elif command -v ndk-build &>/dev/null; then
    NDK_BUILD="$(command -v ndk-build)"
fi

if [ -z "$NDK_BUILD" ]; then
    echo
    error "NDK tidak ditemukan!

Set path NDK secara manual:
  export ANDROID_NDK_ROOT=/path/ke/ndk/24.0.8215888
  ./build_so.sh

Atau edit local.properties:
  sdk.dir=/path/ke/android-sdk
(dan pastikan NDK versi 24.0.8215888 sudah di-install)"
fi

ok "NDK ditemukan: $NDK_BUILD"
NDK_DIR="$(dirname "$NDK_BUILD")"

# ── BERSIHKAN OUTPUT LAMA ─────────────────────────────────────
info "Membersihkan build sebelumnya..."
rm -rf "$JNI_DIR/../obj" "$JNI_DIR/../libs" "$OUT_DIR"
mkdir -p "$OUT_DIR"

# ── HITUNG CORE CPU UNTUK PARALLEL BUILD ──────────────────────
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
info "Parallel jobs: $JOBS"

# ── JALANKAN NDK-BUILD ────────────────────────────────────────
echo
info "Memulai kompilasi..."
echo -e "${YELLOW}──────────────────────────────────────────${NC}"

"$NDK_BUILD" \
    -C "$SCRIPT_DIR/app/src/main" \
    NDK_PROJECT_PATH="$SCRIPT_DIR/app/src/main" \
    NDK_APPLICATION_MK="$JNI_DIR/Application.mk" \
    APP_BUILD_SCRIPT="$JNI_DIR/Android.mk" \
    NDK_LIBS_OUT="$SCRIPT_DIR/app/src/main/libs" \
    NDK_OUT="$SCRIPT_DIR/app/src/main/obj" \
    -j"$JOBS" \
    V=0

BUILD_STATUS=$?
echo -e "${YELLOW}──────────────────────────────────────────${NC}"

if [ $BUILD_STATUS -ne 0 ]; then
    echo
    error "Kompilasi GAGAL! (exit code: $BUILD_STATUS)"
fi

# ── COPY .SO KE OUTPUT ────────────────────────────────────────
SO_SRC="$SCRIPT_DIR/app/src/main/libs/arm64-v8a/library.so"

if [ ! -f "$SO_SRC" ]; then
    # Cari di lokasi lain jika nama berbeda
    SO_SRC=$(find "$SCRIPT_DIR/app/src/main/libs" -name "*.so" 2>/dev/null | head -1)
fi

if [ -z "$SO_SRC" ] || [ ! -f "$SO_SRC" ]; then
    error ".so file tidak ditemukan setelah build!"
fi

SO_NAME="$(basename "$SO_SRC")"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
SO_OUT="$OUT_DIR/${SO_NAME%.so}_${TIMESTAMP}.so"

cp "$SO_SRC" "$SO_OUT"
cp "$SO_SRC" "$OUT_DIR/$SO_NAME"   # juga simpan tanpa timestamp

# ── INFO HASIL ────────────────────────────────────────────────
SO_SIZE=$(du -sh "$SO_OUT" | cut -f1)

echo
echo -e "${GREEN}${BOLD}═══════════════════════════════════════${NC}"
echo -e "${GREEN}${BOLD}   BUILD BERHASIL!                     ${NC}"
echo -e "${GREEN}${BOLD}═══════════════════════════════════════${NC}"
ok "File : $SO_NAME"
ok "Size : $SO_SIZE"
ok "Path : $OUT_DIR/"
echo
info "File output:"
ls -lh "$OUT_DIR/"
echo
