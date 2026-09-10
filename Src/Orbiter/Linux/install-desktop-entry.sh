#!/bin/bash
# Install the desktop entry and icon, so Wayland compositors can show Orbiter's
# icon.
#
# ===========================================================================
# WHY THIS IS NEEDED AT ALL
# ===========================================================================
#
# Windows takes the window icon from the window class:
#     wndClass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MAIN_ICON));
# X11 takes it from _NET_WM_ICON, a property on the window carrying the pixels,
# which UIHost sets with glfwSetWindowIcon.
#
# WAYLAND HAS NEITHER. There is no protocol for a client to hand a compositor
# an icon; asking GLFW to do it returns
#     GLFW error 65548: Wayland: The platform does not support setting the
#                       window icon
# The Wayland mechanism is indirect: the client declares an xdg-shell app_id
# (UIHost sets "openorbiter"), and the compositor looks for an installed
# .desktop file of that name and takes the Icon= from it.
#
# So on Wayland the icon is a FILE ON DISK, not something the program can set.
# This installs it for the current user only -- nothing is written outside
# $HOME, and no privileges are needed.
#
# The alternative, if you would rather install nothing, is to run under X11:
#     ORBITER_GLFW_PLATFORM=x11 ./Orbiter
# where the icon comes straight from the executable's own resource.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

APPDIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONDIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"

mkdir -p "$APPDIR" "$ICONDIR"

# The icon is the 256x256 PNG entry extracted from Src/Orbiter/Orbiter.ico --
# the same image the executable carries.
install -m 0644 "$HERE/openorbiter.png" "$ICONDIR/openorbiter.png"

# Exec= must be an absolute path or something on PATH. The .desktop in the
# source tree says just "Orbiter"; point it at this build so launching from a
# menu works too.
BUILD_ORBITER=$(cd "$HERE/../../.." && pwd)/build-dbg/Orbiter
if [ -x "$BUILD_ORBITER" ]; then
	sed "s|^Exec=.*|Exec=$BUILD_ORBITER|" "$HERE/openorbiter.desktop" \
		> "$APPDIR/openorbiter.desktop"
else
	install -m 0644 "$HERE/openorbiter.desktop" "$APPDIR/openorbiter.desktop"
fi
chmod 0644 "$APPDIR/openorbiter.desktop"

# Refresh the caches, where the tools exist. Neither is fatal.
command -v update-desktop-database >/dev/null 2>&1 \
	&& update-desktop-database "$APPDIR" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null 2>&1 \
	&& gtk-update-icon-cache -f -t "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" 2>/dev/null || true

echo "installed:"
echo "  $APPDIR/openorbiter.desktop"
echo "  $ICONDIR/openorbiter.png"
echo
echo "The app_id UIHost sets is \"openorbiter\", which must match the .desktop"
echo "basename and StartupWMClass. Restart Orbiter to see the icon."
