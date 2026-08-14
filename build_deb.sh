#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_BUILD_ROOT="$ROOT_DIR/build/deb"
OUTPUT_ROOT="$ROOT_DIR/build"
PKG_NAME="macsfancontrol"
VERSION="1.0"
ARCH="$(dpkg --print-architecture)"

BUILD_ROOT="$DEFAULT_BUILD_ROOT"
if [ -d "$DEFAULT_BUILD_ROOT" ]; then
  if [ ! -w "$DEFAULT_BUILD_ROOT" ]; then
    echo "Warning: $DEFAULT_BUILD_ROOT is not writable; using temporary staging area." >&2
    BUILD_ROOT="$(mktemp -d /tmp/macsfancontrol-build.XXXXXX)"
  fi
else
  BUILD_PARENT="$(dirname "$DEFAULT_BUILD_ROOT")"
  if [ ! -w "$BUILD_PARENT" ]; then
    echo "Warning: $BUILD_PARENT is not writable; using temporary staging area." >&2
    BUILD_ROOT="$(mktemp -d /tmp/macsfancontrol-build.XXXXXX)"
  fi
fi

if [ -d "$OUTPUT_ROOT" ] && [ ! -w "$OUTPUT_ROOT" ]; then
  echo "Warning: $OUTPUT_ROOT is not writable; writing package to $ROOT_DIR instead." >&2
  OUTPUT_ROOT="$ROOT_DIR"
fi

PKG_DIR="$BUILD_ROOT/${PKG_NAME}_${VERSION}_${ARCH}"
DEB_FILE="$OUTPUT_ROOT/${PKG_NAME}_${VERSION}_${ARCH}.deb"
INSTALL_PREFIX="$PKG_DIR/usr"
BIN_DIR="$INSTALL_PREFIX/bin"
APP_DIR="$INSTALL_PREFIX/share/applications"
PIXMAP_DIR="$INSTALL_PREFIX/share/pixmaps"
DOC_DIR="$INSTALL_PREFIX/share/doc/$PKG_NAME"
SYSTEMD_DIR="$INSTALL_PREFIX/lib/systemd/system"
POLKIT_DIR="$INSTALL_PREFIX/share/polkit-1/rules.d"

if [ -d "$PKG_DIR" ]; then
  echo "Cleaning existing staging directory: $PKG_DIR"
  rm -rf "$PKG_DIR"
fi
if [ -f "$DEB_FILE" ]; then
  echo "Removing old package file: $DEB_FILE"
  rm -f "$DEB_FILE"
fi

mkdir -p "$BIN_DIR" "$APP_DIR" "$PIXMAP_DIR" "$DOC_DIR" "$SYSTEMD_DIR" "$POLKIT_DIR" "$PKG_DIR/DEBIAN"

cd "$ROOT_DIR"
if [ -f Makefile ]; then
  echo "Cleaning previous build artifacts..."
  make clean >/dev/null 2>&1 || true
fi
echo "Building macsfancontrol binary..."
qmake
make

cp "./macsfancontrol" "$BIN_DIR/$PKG_NAME"
chmod 755 "$BIN_DIR/$PKG_NAME"

cat > "$BIN_DIR/$PKG_NAME-pkexec" <<'EOF'
#!/bin/sh
# pkexec strips the environment by default. Without DISPLAY, XAUTHORITY and
# DBUS_SESSION_BUS_ADDRESS the elevated Qt GUI cannot connect to the user's
# graphical session, so the app would appear to never launch from the desktop.
exec pkexec env DISPLAY="${DISPLAY:-}" XAUTHORITY="${XAUTHORITY:-}" DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-}" /usr/bin/macsfancontrol "$@"
EOF
chmod 755 "$BIN_DIR/$PKG_NAME-pkexec"

cat > "$PKG_DIR/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: libqt5widgets5, libqt5gui5, libqt5core5a, libgl1, policykit-1
Maintainer: Mac Fan Control <noreply@example.com>
Description: Mac Fan Control for Linux
 A Qt5 GUI application to monitor and control Apple Mac fan speeds on Linux.
EOF

cp "$ROOT_DIR/$PKG_NAME.png" "$PIXMAP_DIR/$PKG_NAME.png"

cat > "$BIN_DIR/$PKG_NAME-boot" <<'EOF'
#!/bin/sh
set -eu
if [ -n "${MACSFANCONTROL_BOOT_PRESET:-}" ]; then
  exec /usr/bin/macsfancontrol --daemon --preset "$MACSFANCONTROL_BOOT_PRESET"
fi
exec /usr/bin/macsfancontrol --daemon
EOF
chmod 755 "$BIN_DIR/$PKG_NAME-boot"

cat > "$APP_DIR/$PKG_NAME.desktop" <<EOF
[Desktop Entry]
Name=Mac Fan Control
Comment=Monitor and control Apple Mac fans on Linux
Exec=/usr/bin/$PKG_NAME-pkexec
Icon=/usr/share/pixmaps/$PKG_NAME.png
Terminal=false
Type=Application
Categories=Utility;System;
EOF

cp "$ROOT_DIR/$PKG_NAME.service" "$SYSTEMD_DIR/$PKG_NAME.service"
cp "$ROOT_DIR/$PKG_NAME.rules" "$POLKIT_DIR/50-$PKG_NAME.rules"

cat > "$PKG_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  # Remove an obsolete boot-preset unit from older releases. Two enabled
  # daemons writing the same sysfs fan files caused the GUI to lose control
  # of the fans, so make sure only the current macsfancontrol.service runs.
  if systemctl list-unit-files 2>/dev/null | grep -q 'macsfancontrol-boot.service'; then
    systemctl stop macsfancontrol-boot.service >/dev/null 2>&1 || true
    systemctl disable macsfancontrol-boot.service >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/macsfancontrol-boot.service
  fi
  systemctl daemon-reload >/dev/null 2>&1 || true
  systemctl enable macsfancontrol.service >/dev/null 2>&1 || true
  systemctl restart macsfancontrol.service >/dev/null 2>&1 || true
fi
EOF
chmod 755 "$PKG_DIR/DEBIAN/postinst"

cat > "$DOC_DIR/copyright" <<EOF
Format: http://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: macsfancontrol
Source: https://github.com/yourname/macsfancontrol-qt

Files: *
Copyright: 2026 Mac Fan Control authors
License: MIT
EOF

chmod 755 "$PKG_DIR/DEBIAN"
chmod 644 "$PKG_DIR/DEBIAN/control"
chmod 644 "$APP_DIR/$PKG_NAME.desktop"
chmod 644 "$PIXMAP_DIR/$PKG_NAME.png"
chmod 644 "$SYSTEMD_DIR/$PKG_NAME.service"
chmod 644 "$POLKIT_DIR/50-$PKG_NAME.rules"

chmod 644 "$DOC_DIR/copyright"

if [ ! -d "$OUTPUT_ROOT" ]; then
  mkdir -p "$OUTPUT_ROOT"
fi

dpkg-deb --build --root-owner-group "$PKG_DIR" "$DEB_FILE"

echo "Created Debian package: $DEB_FILE"
