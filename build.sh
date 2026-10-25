#!/usr/bin/env bash
#===============================================================================
# build.sh - Cross-distribution build script for Drum Foundry VST
#
# Detects your Linux distribution, installs missing dependencies (skipping
# the sudo prompt entirely if everything is already installed), then builds
# VST3 and Standalone formats.
#
# Usage: ./build.sh [--release|--debug] [--clean] [--no-install]
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
CLEAN_BUILD=false
SKIP_INSTALL_PROMPT=false

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
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --no-install)
            SKIP_INSTALL_PROMPT=true
            shift
            ;;
        -h|--help)
            cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --release     Build in Release mode (default)
  --debug       Build in Debug mode
  --clean       Remove build directory before building
  --no-install  Skip the install.sh prompt after building
  -h, --help    Show this help message

Environment Variables:
  BUILD_TYPE    Override build type (Release/Debug)

Examples:
  $0                     # Release build, then offer to install
  $0 --debug             # Debug build
  $0 --clean --release   # Clean release build
  $0 --no-install        # Build only, no install prompt
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
# Distribution Detection
#-------------------------------------------------------------------------------
detect_distro() {
    if [[ -f /etc/os-release ]]; then
        . /etc/os-release
        DISTRO_ID="${ID:-unknown}"
        DISTRO_VERSION="${VERSION_ID:-unknown}"

        case "${DISTRO_ID}" in
            cachyos|arch|manjaro|endeavouros)
                PKG_MANAGER="pacman"
                ;;
            debian|ubuntu|pop|linuxmint)
                PKG_MANAGER="apt"
                ;;
            fedora|nobara)
                PKG_MANAGER="dnf"
                ;;
            opensuse-leap|opensuse-tumbleweed)
                PKG_MANAGER="zypper"
                ;;
            *)
                PKG_MANAGER="unknown"
                warn "Unrecognized distribution: ${DISTRO_ID}"
                warn "Supported: Arch/CachyOS, Debian/Ubuntu, Fedora, openSUSE"
                ;;
        esac

        info "Detected: ${DISTRO_ID} ${DISTRO_VERSION} (${PKG_MANAGER})"
    else
        error "/etc/os-release not found!"
        error "Cannot determine distribution."
        exit 1
    fi
}

#-------------------------------------------------------------------------------
# Package Check Helpers
#
# Each of these checks whether a package is already installed WITHOUT
# invoking the package manager (and therefore without needing sudo).
# Returns 0 if installed, 1 if not.
#-------------------------------------------------------------------------------
pkg_installed() {
    case "${PKG_MANAGER}" in
        pacman)
            pacman -Qi "$1" &>/dev/null
            ;;
        apt)
            dpkg -s "$1" &>/dev/null
            ;;
        dnf|zypper)
            rpm -q "$1" &>/dev/null
            ;;
        *)
            return 1
            ;;
    esac
}

# Collects names of missing packages from the arguments into MISSING_PKGS
MISSING_PKGS=()
collect_missing() {
    MISSING_PKGS=()
    local pkg
    for pkg in "$@"; do
        if ! pkg_installed "$pkg"; then
            MISSING_PKGS+=("$pkg")
        fi
    done
}

#-------------------------------------------------------------------------------
# Dependency Installation Functions
#
# Each function first checks which packages are missing; if none are,
# it skips the package manager entirely (no sudo prompt at all).
#-------------------------------------------------------------------------------
install_arch_deps() {
    local pkgs=(base-devel cmake ninja git curl wget)
    collect_missing "${pkgs[@]}"

    if [[ ${#MISSING_PKGS[@]} -eq 0 ]]; then
        success "All Arch dependencies already installed - skipping sudo"
        return 0
    fi

    info "Missing packages: ${MISSING_PKGS[*]}"
    info "Installing dependencies for Arch-based systems..."

    if [[ $EUID -ne 0 ]]; then
        sudo pacman -S --needed --noconfirm "${MISSING_PKGS[@]}"
    else
        pacman -S --needed --noconfirm "${MISSING_PKGS[@]}"
    fi

    success "Arch dependencies installed"
}

install_debian_deps() {
    local pkgs=(build-essential cmake ninja-build git curl wget libcurl4-openssl-dev libglu1-mesa-dev x11proto-core-dev)
    collect_missing "${pkgs[@]}"

    if [[ ${#MISSING_PKGS[@]} -eq 0 ]]; then
        success "All Debian/Ubuntu dependencies already installed - skipping sudo"
        return 0
    fi

    info "Missing packages: ${MISSING_PKGS[*]}"
    info "Installing dependencies for Debian/Ubuntu..."

    sudo apt-get update
    sudo apt-get install -y "${MISSING_PKGS[@]}"

    success "Debian/Ubuntu dependencies installed"
}

install_fedora_deps() {
    local pkgs=(cmake ninja-build git curl wget libcurl-devel mesa-libGLU-devel gcc gcc-c++ make)
    collect_missing "${pkgs[@]}"

    if [[ ${#MISSING_PKGS[@]} -eq 0 ]]; then
        success "All Fedora dependencies already installed - skipping sudo"
        return 0
    fi

    info "Missing packages: ${MISSING_PKGS[*]}"
    info "Installing dependencies for Fedora..."

    sudo dnf groupinstall -y "Development Tools" || true
    sudo dnf install -y "${MISSING_PKGS[@]}"

    success "Fedora dependencies installed"
}

install_opensuse_deps() {
    local pkgs=(cmake ninja git curl wget libcurl4-devel Mesa-libGLU-devel gcc gcc-c++ make)
    collect_missing "${pkgs[@]}"

    if [[ ${#MISSING_PKGS[@]} -eq 0 ]]; then
        success "All openSUSE dependencies already installed - skipping sudo"
        return 0
    fi

    info "Missing packages: ${MISSING_PKGS[*]}"
    info "Installing dependencies for openSUSE..."

    sudo zypper install -y "${MISSING_PKGS[@]}"
    sudo zypper install -y -t pattern devel_basis || true

    success "openSUSE dependencies installed"
}

#-------------------------------------------------------------------------------
# Build Functions
#-------------------------------------------------------------------------------
configure_cmake() {
    info "Configuring CMake build (type=${BUILD_TYPE})..."

    mkdir -p "${BUILD_DIR}"

    cmake \
        -S "${SCRIPT_DIR}" \
        -B "${BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DDRUMFOUNDRY_BUILD_TOOLS=OFF

    success "CMake configuration complete"
}

build_project() {
    info "Building Drum Foundry..."

    cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

    success "Build completed successfully"
}

report_artifacts() {
    info "Plugin build artifacts are in: ${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/"

    echo ""
    echo "Available plugins:"
    ls -la "${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/" 2>/dev/null || \
        warn "No artefacts found - check build output for errors"

    echo ""
    info "Installation locations:"
    echo "  VST3:       ~/.vst3/ or /usr/lib/vst3/"
    echo "  Standalone: Build directory (run directly)"
}

#-------------------------------------------------------------------------------
# Install Prompt
#-------------------------------------------------------------------------------
prompt_install() {
    if [[ "${SKIP_INSTALL_PROMPT}" == true ]]; then
        return 0
    fi

    if [[ ! -f "${SCRIPT_DIR}/install.sh" ]]; then
        return 0
    fi

    echo ""
    read -p "Would you like to run install.sh to install to ~/.vst3 and ~/.local? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        info "Running install.sh..."
        bash "${SCRIPT_DIR}/install.sh"
    else
        info "Skipping installation. Run ./install.sh anytime to install manually."
    fi
}

#-------------------------------------------------------------------------------
# Main Execution
#-------------------------------------------------------------------------------
main() {
    echo "==============================================================================="
    echo "                    Drum Foundry VST - Linux Build Script"
    echo "==============================================================================="
    echo ""

    # Step 1: Detect distribution
    detect_distro

    # Step 2: Clean if requested
    if [[ "${CLEAN_BUILD}" == true ]]; then
        info "Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
        success "Clean complete"
    fi

    # Step 3: Install dependencies (skipped entirely if all present)
    case "${PKG_MANAGER}" in
        pacman)
            install_arch_deps
            ;;
        apt)
            install_debian_deps
            ;;
        dnf)
            install_fedora_deps
            ;;
        zypper)
            install_opensuse_deps
            ;;
        *)
            error "Unsupported package manager: ${PKG_MANAGER}"
            error "Please install these manually:"
            echo "  - CMake 3.22+"
            echo "  - Ninja"
            echo "  - GCC/Clang with C++17 support"
            echo "  - git, curl, wget"
            echo ""
            echo "Then run this script again with: SKIP_DEPS=1 $0"
            exit 1
            ;;
    esac

    # Step 4: Configure CMake
    configure_cmake

    # Step 5: Build
    build_project

    # Step 6: Report
    report_artifacts

    # Step 7: Offer installation
    prompt_install

    echo ""
    echo "==============================================================================="
    success "All done! Plugins ready in: ${BUILD_DIR}/DrumFoundry_artefacts/${BUILD_TYPE}/"
    echo "==============================================================================="
}

# Execute main function
main "$@"