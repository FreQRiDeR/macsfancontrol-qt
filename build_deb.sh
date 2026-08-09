#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="$ROOT_DIR/build/deb"
PKG_NAME="macsfancontrol"
VERSION="1.0"
ARCH="$(dpkg --print-architecture)"
PKG_DIR="$BUILD_ROOT/${PKG_NAME}_${VERSION}_${ARCH}"
DEB_FILE="$ROOT_DIR/build/${PKG_NAME}_${VERSION}_${ARCH}.deb"
INSTALL_PREFIX="$PKG_DIR/usr"
BIN_DIR="$INSTALL_PREFIX/bin"
APP_DIR="$INSTALL_PREFIX/share/applications"
PIXMAP_DIR="$INSTALL_PREFIX/share/pixmaps"
DOC_DIR="$INSTALL_PREFIX/share/doc/$PKG_NAME"
SYSTEMD_DIR="$INSTALL_PREFIX/lib/systemd/system"
POLKIT_DIR="$INSTALL_PREFIX/share/polkit-1/rules.d"

mkdir -p "$BIN_DIR" "$APP_DIR" "$PIXMAP_DIR" "$DOC_DIR" "$SYSTEMD_DIR" "$POLKIT_DIR" "$PKG_DIR/DEBIAN"

cd "$ROOT_DIR"
if [ ! -x "./macsfancontrol" ] || [ "./macsfancontrol" -ot src/main.cpp ]; then
  echo "Building macsfancontrol binary..."
  qmake
  make
fi

cp "./macsfancontrol" "$BIN_DIR/$PKG_NAME"
chmod 755 "$BIN_DIR/$PKG_NAME"

cat > "$BIN_DIR/$PKG_NAME-pkexec" <<'EOF'
#!/bin/sh
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
exec /usr/bin/macsfancontrol --daemon --preset "${MACSFANCONTROL_BOOT_PRESET:-default}"
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

mkdir -p "$ROOT_DIR/build"
dpkg-deb --build "$PKG_DIR" "$DEB_FILE"

echo "Created Debian package: $DEB_FILE"
