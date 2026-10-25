#!/usr/bin/env bash
#===============================================================================
# install.sh - Install Drum Foundry VST to system locations
#
# Copies built plugins to standard Linux locations:
#   - VST3 to ~/.vst3/
#   - Standalone to ~/.local/bin/ (as DrumFoundry)
#   - .desktop file to ~/.local/share/applications/
#   - Icon to ~/.local/share/icons/hicolor/512x512/apps/
#
# Usage: ./install.sh [--uninstall]
#
# License: AGPL-3.0
#===============================================================================

set -euo pipefail

#-------------------------------------------------------------------------------
# Configuration
#-------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
INSTALL_PREFIX="${INSTALL_PREFIX:-$HOME}"

VST3_DIR="${INSTALL_PREFIX}/.vst3"
BIN_DIR="${INSTALL_PREFIX}/.local/bin"
DESKTOP_DIR="${INSTALL_PREFIX}/.local/share/applications"
ICON_DIR="${INSTALL_PREFIX}/.local/share/icons/hicolor/512x512/apps"
ASSETS_DIR="${SCRIPT_DIR}/assets"

UNINSTALL_MODE=false

#-------------------------------------------------------------------------------
# Color output helpers
#-------------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
success() { echo -e "${GREEN}[✓]${NC} $*"; }
error()   { echo -e "${RED}[✗]${NC} $*" >&2; }

#-------------------------------------------------------------------------------
# Argument Parsing
#-------------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --uninstall)
            UNINSTALL_MODE=true
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --uninstall   Remove installed files (reverse install)
  -h, --help    Show this help message

Environment Variables:
  BUILD_TYPE    Build type to install (Release/Debug, default: Release)
  INSTALL_PREFIX Install prefix (default: \$HOME)

Examples:
  $0                           # Install Release build
  $0 --uninstall               # Remove installed files
  BUILD_TYPE=Debug $0          # Install Debug build
EOF
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            exit 1
            ;;
    esac
done

#-------------------------------------------------------------------------------
# Helper Functions
#-------------------------------------------------------------------------------
check_build_exists() {
    local vst3_path="${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/VST3/Drum Foundry.vst3"
    local standalone_path="${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/Standalone/Drum Foundry"

    if [[ ! -d "$vst3_path" ]]; then
        error "VST3 not found at: $vst3_path"
        error "Run ./build.sh first to build the plugin."
        exit 1
    fi

    if [[ ! -f "$standalone_path" ]]; then
        warn "Standalone not found at: $standalone_path"
        warn "Continuing with VST3 installation only..."
        return 1
    fi

    return 0
}

ensure_directory() {
    local dir="$1"
    if [[ ! -d "$dir" ]]; then
        info "Creating directory: $dir"
        mkdir -p "$dir"
    fi
}

#-------------------------------------------------------------------------------
# Install Functions
#-------------------------------------------------------------------------------
install_vst3() {
    local vst3_source="${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/VST3/Drum Foundry.vst3"
    local vst3_dest="${VST3_DIR}/Drum Foundry.vst3"

    info "Installing VST3 to ${VST3_DIR}/..."
    ensure_directory "$VST3_DIR"

    if [[ -d "$vst3_dest" ]]; then
        info "Removing existing installation..."
        rm -rf "$vst3_dest"
    fi

    cp -r "$vst3_source" "$vst3_dest"
    success "VST3 installed to ${vst3_dest}"
}

install_standalone() {
    local standalone_source="${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/Standalone/Drum Foundry"
    local standalone_dest="${BIN_DIR}/DrumFoundry"

    info "Installing standalone to ${BIN_DIR}/..."
    ensure_directory "$BIN_DIR"

    cp "$standalone_source" "$standalone_dest"
    chmod +x "$standalone_dest"
    success "Standalone installed to ${standalone_dest}"
}

install_desktop_file() {
    local desktop_source="${ASSETS_DIR}/DrumFoundry.desktop"
    local desktop_dest="${DESKTOP_DIR}/drum-foundry.desktop"

    # Check if custom .desktop exists, otherwise create default
    if [[ -f "$desktop_source" ]]; then
        info "Copying .desktop file from assets..."
        ensure_directory "$DESKTOP_DIR"
        cp "$desktop_source" "$desktop_dest"
    else
        info "Creating default .desktop file..."
        ensure_directory "$DESKTOP_DIR"

        cat > "$desktop_dest" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Drum Foundry
Comment=A kick and snare synthesizer plugin
Exec=${BIN_DIR}/DrumFoundry
Icon=drum-foundry
Terminal=false
Categories=Audio;AudioVideo;Generator;Midi;Sequencer;Tuner;
Keywords=synth;drums;kick;snare;music;production;
StartupWMClass=DrumFoundry
EOF
        success "Created .desktop file at ${desktop_dest}"
    fi
}

install_icon() {
    local icon_source="${ASSETS_DIR}/Theres_Tears_Icon_512.png"
    local icon_dest="${ICON_DIR}/drum-foundry.png"

    if [[ -f "$icon_source" ]]; then
        info "Installing icon to ${ICON_DIR}/..."
        ensure_directory "$ICON_DIR"
        cp "$icon_source" "$icon_dest"
        
        # Update icon cache if possible
        if command -v gtk-update-icon-cache &>/dev/null; then
            gtk-update-icon-cache -f "${ICON_DIR%/*}" 2>/dev/null || true
        fi
        
        success "Icon installed to ${icon_dest}"
    else
        warn "Icon not found at ${icon_source}, skipping..."
        warn "Place icon at assets/Theres_Tears_Icon_512.png for icon installation"
    fi
}

#-------------------------------------------------------------------------------
# Uninstall Functions
#-------------------------------------------------------------------------------
uninstall_vst3() {
    local vst3_path="${VST3_DIR}/Drum Foundry.vst3"
    
    if [[ -d "$vst3_path" ]]; then
        info "Removing VST3 installation..."
        rm -rf "$vst3_path"
        success "VST3 uninstalled"
    else
        warn "VST3 not installed at ${vst3_path}"
    fi
}

uninstall_standalone() {
    local standalone_path="${BIN_DIR}/DrumFoundry"
    
    if [[ -f "$standalone_path" ]]; then
        info "Removing standalone installation..."
        rm -f "$standalone_path"
        success "Standalone uninstalled"
    else
        warn "Standalone not installed at ${standalone_path}"
    fi
}

uninstall_desktop() {
    local desktop_path="${DESKTOP_DIR}/drum-foundry.desktop"
    
    if [[ -f "$desktop_path" ]]; then
        info "Removing .desktop file..."
        rm -f "$desktop_path"
        success ".desktop file uninstalled"
    else
        warn ".desktop file not found at ${desktop_path}"
    fi
}

uninstall_icon() {
    local icon_path="${ICON_DIR}/drum-foundry.png"
    
    if [[ -f "$icon_path" ]]; then
        info "Removing icon..."
        rm -f "$icon_path"
        success "Icon uninstalled"
    else
        warn "Icon not found at ${icon_path}"
    fi
}

#-------------------------------------------------------------------------------
# Main Execution
#-------------------------------------------------------------------------------
main() {
    echo "==============================================================================="
    echo "                    Drum Foundry VST - Linux Installer"
    echo "==============================================================================="
    echo ""
    echo "Build type: ${BUILD_TYPE}"
    echo "Install prefix: ${INSTALL_PREFIX}"
    echo ""

    if [[ "${UNINSTALL_MODE}" == true ]]; then
        info "Uninstall mode enabled"
        echo ""
        
        read -p "Are you sure you want to uninstall Drum Foundry? [y/N] " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            info "Uninstall cancelled"
            exit 0
        fi

        # Uninstall all components
        uninstall_vst3
        uninstall_standalone
        uninstall_desktop
        uninstall_icon

        echo ""
        echo "==============================================================================="
        success "Uninstallation complete!"
        echo "==============================================================================="
        exit 0
    fi

    # Check if build exists
    check_build_exists

    echo "This will install:"
    echo "  - VST3 plugin to: ${VST3_DIR}/"
    echo "  - Standalone app to: ${BIN_DIR}/DrumFoundry"
    echo "  - Desktop entry to: ${DESKTOP_DIR}/drum-foundry.desktop"
    echo "  - Icon to: ${ICON_DIR}/drum-foundry.png"
    echo ""

    # Ask for confirmation
    read -p "Continue with installation? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        info "Installation cancelled"
        exit 0
    fi

    # Install VST3
    install_vst3

    # Install standalone (if available)
    if check_build_exists 2>/dev/null; then
        install_standalone
    fi

    # Install desktop file
    install_desktop_file

    # Install icon
    install_icon

    echo ""
    echo "==============================================================================="
    success "Installation complete!"
    echo ""
    echo "To use in your DAW:"
    echo "  1. Rescan plugins (usually in DAW preferences)"
    echo "  2. Look for 'Drum Foundry' in your plugin list"
    echo ""
    echo "To run standalone:"
    echo "  DrumFoundry"
    echo ""
    echo "Note: If your DAW doesn't find the plugin, you may need to:"
    echo "  - Run: ${SCRIPT_DIR}/build.sh --clean && ${SCRIPT_DIR}/build.sh"
    echo "  - Manually rescan in your DAW"
    echo "  - Check DAW plugin paths include ~/.vst3/"
    echo "==============================================================================="
}

# Execute main function
main "$@"