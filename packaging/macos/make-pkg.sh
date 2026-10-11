#!/usr/bin/env bash
# Builds the macOS installer: dist/DrumFoundry-<version>-macOS.pkg.
#
# Usage:
#   packaging/macos/make-pkg.sh             configure + build universal Release, then package
#   packaging/macos/make-pkg.sh --no-build  package an existing build
#
# Environment (all optional):
#   BUILD_DIR                 build folder (default: <repo>/build-release)
#   DEVELOPER_ID_APPLICATION  codesign identity for the bundles, e.g. "Developer ID Application: ..."
#   DEVELOPER_ID_INSTALLER    productbuild identity for the pkg, e.g. "Developer ID Installer: ..."
#   NOTARY_PROFILE            notarytool keychain profile; submits the pkg and staples the ticket
#
# Each signing step is skipped when its variable is unset, which gives an unsigned pkg.
# The signing and notarization paths have not been tested yet (no Developer ID
# certificate on the machine this was written on).
#
# The pkg holds four component packages:
#   VST3       -> /Library/Audio/Plug-Ins/VST3
#   AU         -> /Library/Audio/Plug-Ins/Components
#   App        -> /Applications
#   Licences   -> /Library/Application Support/Drum Foundry/Licenses (always installed)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-release}"
DIST_DIR="$REPO_ROOT/dist"

PRODUCT="Drum Foundry"
ID_BASE="com.milliganaudio.drumfoundry"
MIN_MACOS="11.0"

DO_BUILD=1
for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

# The version comes from the project() line in CMakeLists.txt.
VERSION="$(sed -nE 's/^project\(DrumFoundry VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n 1)"
if [[ -z "$VERSION" ]]; then
    echo "Couldn't read the version from CMakeLists.txt" >&2
    exit 1
fi
OUTPUT_PKG="$DIST_DIR/DrumFoundry-$VERSION-macOS.pkg"

echo "Drum Foundry $VERSION"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [[ "$DO_BUILD" == 1 ]]; then
    echo "==> Configuring $BUILD_DIR"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
        -DDRUMFOUNDRY_BUILD_TOOLS=OFF \
        -DDRUMFOUNDRY_COPY_PLUGINS=OFF
    echo "==> Building"
    cmake --build "$BUILD_DIR" --config Release --target DrumFoundry_All
fi

ARTEFACTS="$BUILD_DIR/DrumFoundry_artefacts/Release"
VST3_BUNDLE="$ARTEFACTS/VST3/$PRODUCT.vst3"
AU_BUNDLE="$ARTEFACTS/AU/$PRODUCT.component"
APP_BUNDLE="$ARTEFACTS/Standalone/$PRODUCT.app"

for bundle in "$VST3_BUNDLE" "$AU_BUNDLE" "$APP_BUNDLE"; do
    if [[ ! -d "$bundle" ]]; then
        echo "Missing $bundle (build first, or set BUILD_DIR)" >&2
        exit 1
    fi
done

# The pkg says it runs on both architectures, so warn when a binary isn't universal.
for bundle in "$VST3_BUNDLE" "$AU_BUNDLE" "$APP_BUNDLE"; do
    binary="$bundle/Contents/MacOS/$PRODUCT"
    archs="$(lipo -archs "$binary" 2>/dev/null || true)"
    if [[ "$archs" != *arm64* || "$archs" != *x86_64* ]]; then
        echo "Warning: $binary is not universal (archs: ${archs:-unknown})" >&2
    fi
done

# ---------------------------------------------------------------------------
# Stage: one root folder per component, holding only that component's files
# ---------------------------------------------------------------------------
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/drumfoundry-pkg.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

STAGE="$WORK_DIR/stage"
COMPONENTS="$WORK_DIR/components"
RESOURCES="$WORK_DIR/resources"
mkdir -p "$STAGE/vst3" "$STAGE/au" "$STAGE/app" "$STAGE/licenses" "$COMPONENTS" "$RESOURCES"

echo "==> Staging bundles"
# ditto keeps symlinks, permissions and extended attributes, which cp can lose.
ditto "$VST3_BUNDLE" "$STAGE/vst3/$PRODUCT.vst3"
ditto "$AU_BUNDLE"   "$STAGE/au/$PRODUCT.component"
ditto "$APP_BUNDLE"  "$STAGE/app/$PRODUCT.app"

# Licence texts, installed next to the binaries rather than inside them (a file added
# to a bundle would break its signature).
install -m 644 "$REPO_ROOT/LICENSE"                     "$STAGE/licenses/LICENSE.txt"
install -m 644 "$REPO_ROOT/NOTICE"                      "$STAGE/licenses/NOTICE.txt"
install -m 644 "$REPO_ROOT/libs/CloudSeedCore/LICENSE"  "$STAGE/licenses/CloudSeedCore-LICENSE.txt"
install -m 644 "$REPO_ROOT/assets/fonts/Inter-OFL.txt"   "$STAGE/licenses/Inter-OFL.txt"
install -m 644 "$REPO_ROOT/assets/fonts/Archivo-OFL.txt" "$STAGE/licenses/Archivo-OFL.txt"

# A quarantine flag copied into the payload would follow the files onto the user's Mac.
xattr -r -d com.apple.quarantine "$STAGE" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Sign the bundles (optional, untested)
# ---------------------------------------------------------------------------
if [[ -n "${DEVELOPER_ID_APPLICATION:-}" ]]; then
    echo "==> Signing bundles as $DEVELOPER_ID_APPLICATION"
    for bundle in "$STAGE/vst3/$PRODUCT.vst3" "$STAGE/au/$PRODUCT.component"; do
        codesign --force --options runtime --timestamp --sign "$DEVELOPER_ID_APPLICATION" "$bundle"
    done
    # The app has no nested frameworks or helpers today; --deep covers any that appear.
    codesign --force --deep --options runtime --timestamp --sign "$DEVELOPER_ID_APPLICATION" \
        "$STAGE/app/$PRODUCT.app"
    for bundle in "$STAGE/vst3/$PRODUCT.vst3" "$STAGE/au/$PRODUCT.component" "$STAGE/app/$PRODUCT.app"; do
        codesign --verify --strict --verbose=1 "$bundle"
    done
else
    # The build leaves the VST3 with a broken seal (moduleinfo.json is written after
    # signing) and the AU and app with only the linker's signature on the binary. An ad hoc
    # signature over each whole bundle makes them verify; it carries no identity.
    echo "==> DEVELOPER_ID_APPLICATION not set: ad hoc signing the bundles"
    for bundle in "$STAGE/vst3/$PRODUCT.vst3" "$STAGE/au/$PRODUCT.component" "$STAGE/app/$PRODUCT.app"; do
        codesign --force --sign - "$bundle"
        codesign --verify --strict "$bundle"
    done
fi

# ---------------------------------------------------------------------------
# Component packages
# ---------------------------------------------------------------------------
# make_component <stage-root> <identifier> <install-location> <output.pkg>
make_component() {
    local root="$1" identifier="$2" location="$3" output="$4"
    local plist="$WORK_DIR/$(basename "$output" .pkg).plist"

    # Turn off bundle relocation. Otherwise Installer looks for a bundle with the same
    # identifier anywhere on the disk (a build folder, say) and "upgrades" that copy in
    # place instead of installing to the install location.
    pkgbuild --analyze --root "$root" "$plist" >/dev/null
    local i=0
    while plutil -extract "$i" xml1 -o /dev/null "$plist" 2>/dev/null; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
        i=$((i + 1))
    done
    if [[ "$i" -eq 0 ]]; then
        echo "pkgbuild found no bundle in $root" >&2
        exit 1
    fi

    pkgbuild --root "$root" \
        --component-plist "$plist" \
        --identifier "$identifier" \
        --version "$VERSION" \
        --install-location "$location" \
        --min-os-version "$MIN_MACOS" \
        "$output"
}

echo "==> Building component packages"
make_component "$STAGE/vst3" "$ID_BASE.vst3" "/Library/Audio/Plug-Ins/VST3"      "$COMPONENTS/DrumFoundry-VST3.pkg"
make_component "$STAGE/au"   "$ID_BASE.au"   "/Library/Audio/Plug-Ins/Components" "$COMPONENTS/DrumFoundry-AU.pkg"
make_component "$STAGE/app"  "$ID_BASE.app"  "/Applications"                      "$COMPONENTS/DrumFoundry-App.pkg"

# The licence folder holds no bundles, so there's nothing to analyze.
pkgbuild --root "$STAGE/licenses" \
    --identifier "$ID_BASE.licenses" \
    --version "$VERSION" \
    --install-location "/Library/Application Support/$PRODUCT/Licenses" \
    --min-os-version "$MIN_MACOS" \
    "$COMPONENTS/DrumFoundry-Licenses.pkg"

# ---------------------------------------------------------------------------
# Installer pages and distribution file
# ---------------------------------------------------------------------------
sed "s/@VERSION@/$VERSION/g" "$SCRIPT_DIR/welcome.html" > "$RESOURCES/welcome.html"

# Licence panel: NOTICE (copyright and additional terms) followed by the full AGPLv3.
{
    cat "$REPO_ROOT/NOTICE"
    printf '\n\n'
    printf '%s\n' "------------------------------------------------------------------------------"
    printf '\n'
    cat "$REPO_ROOT/LICENSE"
} > "$RESOURCES/License.txt"

DISTRIBUTION="$WORK_DIR/distribution.xml"
cat > "$DISTRIBUTION" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>$PRODUCT</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <license file="License.txt" mime-type="text/plain"/>

    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="$MIN_MACOS"/>
        </allowed-os-versions>
    </volume-check>

    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
        <line choice="licenses"/>
    </choices-outline>

    <choice id="vst3" title="VST3 plug-in" selected="true"
        description="Drum Foundry.vst3, installed to /Library/Audio/Plug-Ins/VST3. For VST3 hosts such as Cubase, Bitwig, Reaper and Studio One.">
        <pkg-ref id="$ID_BASE.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit plug-in" selected="true"
        description="Drum Foundry.component, installed to /Library/Audio/Plug-Ins/Components. For Audio Unit hosts such as Logic Pro, GarageBand and Ableton Live.">
        <pkg-ref id="$ID_BASE.au"/>
    </choice>
    <choice id="app" title="Standalone app" selected="true"
        description="Drum Foundry.app, installed to /Applications. Runs on its own, without a plug-in host.">
        <pkg-ref id="$ID_BASE.app"/>
    </choice>
    <!-- Always installed, not shown under Customize. -->
    <choice id="licenses" title="Licences" visible="false" enabled="false" selected="true">
        <pkg-ref id="$ID_BASE.licenses"/>
    </choice>

    <pkg-ref id="$ID_BASE.vst3" version="$VERSION" onConclusion="none">DrumFoundry-VST3.pkg</pkg-ref>
    <pkg-ref id="$ID_BASE.au" version="$VERSION" onConclusion="none">DrumFoundry-AU.pkg</pkg-ref>
    <pkg-ref id="$ID_BASE.app" version="$VERSION" onConclusion="none">DrumFoundry-App.pkg</pkg-ref>
    <pkg-ref id="$ID_BASE.licenses" version="$VERSION" onConclusion="none">DrumFoundry-Licenses.pkg</pkg-ref>
</installer-gui-script>
EOF

# ---------------------------------------------------------------------------
# Product archive
# ---------------------------------------------------------------------------
echo "==> Building $OUTPUT_PKG"
mkdir -p "$DIST_DIR"
rm -f "$OUTPUT_PKG"

SIGN_ARGS=()
if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    echo "==> Signing the pkg as $DEVELOPER_ID_INSTALLER"
    SIGN_ARGS=(--sign "$DEVELOPER_ID_INSTALLER" --timestamp)
else
    echo "==> DEVELOPER_ID_INSTALLER not set: the pkg is unsigned"
fi

# "${SIGN_ARGS[@]+...}" keeps bash 3.2 (the macOS default) happy with an empty array under set -u.
productbuild --distribution "$DISTRIBUTION" \
    --resources "$RESOURCES" \
    --package-path "$COMPONENTS" \
    --version "$VERSION" \
    ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} \
    "$OUTPUT_PKG"

# ---------------------------------------------------------------------------
# Notarize (optional, untested)
# ---------------------------------------------------------------------------
if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    echo "==> Notarizing with keychain profile $NOTARY_PROFILE"
    xcrun notarytool submit "$OUTPUT_PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUTPUT_PKG"
fi

echo "==> Done"
echo "$OUTPUT_PKG"
