#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  push_github.sh  —  Push project LYN8BP ke GitHub
#  Otomatis bersihkan file besar (NDK, .so) sebelum push
#  Token dibaca dari env var GITHUB_TOKEN atau argumen
# ═══════════════════════════════════════════════════════════════

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD}   PUSH LYN8BP → GITHUB                    ${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo

GITHUB_USER="Kztutorial99"
GITHUB_REPO="LYN8BP"
BRANCH="main"

# ── Ambil token ───────────────────────────────────────────────
TOKEN="${GITHUB_TOKEN:-$1}"
if [ -z "$TOKEN" ]; then
    err "Token tidak ada! Jalankan:\n  export GITHUB_TOKEN=ghp_xxxxx && ./push_github.sh"
fi

REMOTE_URL="https://${GITHUB_USER}:${TOKEN}@github.com/${GITHUB_USER}/${GITHUB_REPO}.git"

# ── Git config ────────────────────────────────────────────────
git config user.email "lyn-build@bot.local" 2>/dev/null || true
git config user.name  "LYN Build Bot"       2>/dev/null || true

# ── Setup remote ──────────────────────────────────────────────
info "Setup remote github..."
if git remote get-url github &>/dev/null 2>&1; then
    git remote set-url github "$REMOTE_URL"
else
    git remote add github "$REMOTE_URL"
fi
ok "Remote github siap"

# ── Bersihkan file besar dari git tracking (jika ada) ─────────
LARGE_DIRS=("android-sdk" "ndk" "output_so")
CLEANED=0

for dir in "${LARGE_DIRS[@]}"; do
    if git ls-files --error-unmatch "$dir" &>/dev/null 2>&1; then
        warn "Menghapus '$dir' dari git tracking (file tetap ada di disk)..."
        git rm -r --cached "$dir" --quiet
        CLEANED=1
    fi
done

# Bersihkan .so files yang mungkin ter-track
SO_FILES=$(git ls-files "*.so" 2>/dev/null | head -5)
if [ -n "$SO_FILES" ]; then
    warn "Menghapus .so files dari tracking..."
    git ls-files "*.so" | xargs git rm --cached --quiet 2>/dev/null || true
    CLEANED=1
fi

# ── Tambah .gitignore + commit perubahan ─────────────────────
info "Stage semua perubahan..."
git add -A

# Cek apakah ada yang perlu di-commit
if ! git diff --cached --quiet; then
    MSG="Cleaned: hapus NDK/build dari tracking, tambah .gitignore"
    if [ $CLEANED -eq 1 ]; then
        MSG="Remove large files (NDK/SDK) from git tracking"
    fi

    # Cek apakah ada uncommitted source changes
    CHANGED=$(git diff --cached --name-only | grep -v "^android-sdk" | grep -v "^output_so" | wc -l)
    if [ "$CHANGED" -gt 0 ]; then
        MSG="Update source + remove large files from tracking"
    fi

    git commit -m "$MSG"
    ok "Commit: $MSG"
else
    info "Tidak ada perubahan baru, langsung push."
fi

# ── Push ke GitHub ────────────────────────────────────────────
echo
info "Pushing ke github.com/${GITHUB_USER}/${GITHUB_REPO} (${BRANCH})..."
echo

git push github HEAD:"$BRANCH" --force-with-lease 2>&1 || {
    warn "force-with-lease gagal, coba force push..."
    git push github HEAD:"$BRANCH" --force
}

echo
echo -e "${GREEN}${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${GREEN}${BOLD}   PUSH BERHASIL!                           ${NC}"
echo -e "${GREEN}${BOLD}═══════════════════════════════════════════${NC}"
ok "Repo  : github.com/${GITHUB_USER}/${GITHUB_REPO}"
ok "Branch: ${BRANCH}"
echo
echo "  Lihat repo kamu di:"
echo "  https://github.com/${GITHUB_USER}/${GITHUB_REPO}"
echo

# ── Bersihkan token dari remote URL setelah push ─────────────
git remote set-url github "https://github.com/${GITHUB_USER}/${GITHUB_REPO}.git"
ok "Token dibersihkan dari remote URL"
