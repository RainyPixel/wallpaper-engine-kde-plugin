#!/bin/bash
# Wallpaper Engine KDE Plugin - Uninstaller

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[ OK ]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERR ]${NC} $*" >&2; }
die()   { error "$*"; exit 1; }

PLUGIN_ID="com.github.catsout.wallpaperEngineKde"
QML_SYSTEM_DIR="/usr/lib/qt6/qml/com/github/catsout/wallpaperEngineKde"
PLASMA_USER_DIR="${HOME}/.local/share/plasma/wallpapers/${PLUGIN_ID}"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Wallpaper Engine KDE Plugin – Uninstaller"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Parse flags
SKIP_RESTART=0
YES=0
for arg in "$@"; do
    case "$arg" in
        --skip-restart) SKIP_RESTART=1 ;;
        --yes|-y)       YES=1 ;;
        --help|-h)
            echo "Usage: $0 [--yes] [--skip-restart]"
            echo ""
            echo "  --yes / -y      Skip confirmation prompt"
            echo "  --skip-restart  Skip restarting plasmashell after uninstall"
            exit 0
            ;;
    esac
done

# ── confirm ───────────────────────────────────────────────────────────────────
if [[ "${YES}" == "0" ]]; then
    echo "This will remove:"
    echo "  [system]  ${QML_SYSTEM_DIR}"
    echo "  [user]    ${PLASMA_USER_DIR}"
    echo ""
    read -rp "Proceed? [y/N] " ans
    [[ "${ans,,}" == "y" ]] || { info "Aborted."; exit 0; }
    echo ""
fi

# ── remove system QML plugin (requires sudo) ──────────────────────────────────
remove_system() {
    if [[ -d "${QML_SYSTEM_DIR}" ]]; then
        info "Removing system QML plugin: ${QML_SYSTEM_DIR}"
        sudo rm -rf "${QML_SYSTEM_DIR}" \
            && ok "Removed ${QML_SYSTEM_DIR}" \
            || { error "Failed to remove ${QML_SYSTEM_DIR}"; return 1; }
    else
        warn "System QML plugin not found (already uninstalled?)"
    fi
}

# ── remove plasma user package ────────────────────────────────────────────────
remove_user_pkg() {
    if [[ -d "${PLASMA_USER_DIR}" ]]; then
        info "Removing user plasma package: ${PLASMA_USER_DIR}"
        if command -v kpackagetool6 &>/dev/null; then
            kpackagetool6 -t Plasma/Wallpaper -r "${PLUGIN_ID}" 2>/dev/null \
                && ok "Removed via kpackagetool6" \
                || { warn "kpackagetool6 removal failed, falling back to rm"; rm -rf "${PLASMA_USER_DIR}"; ok "Removed ${PLASMA_USER_DIR}"; }
        else
            rm -rf "${PLASMA_USER_DIR}"
            ok "Removed ${PLASMA_USER_DIR}"
        fi
    else
        warn "User plasma package not found (already uninstalled?)"
    fi
}

# ── restart plasmashell ───────────────────────────────────────────────────────
restart_plasma() {
    info "Restarting plasmashell…"
    if systemctl --user is-active plasma-plasmashell.service &>/dev/null; then
        systemctl --user restart plasma-plasmashell.service \
            && ok "plasmashell restarted." \
            || warn "Could not restart plasmashell. Please log out and back in."
    else
        warn "plasma-plasmashell service not active. Please restart your KDE session."
    fi
}

# ── main ──────────────────────────────────────────────────────────────────────
remove_system
remove_user_pkg

[[ "${SKIP_RESTART}" == "0" ]] && restart_plasma

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ok "Uninstall complete."
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
