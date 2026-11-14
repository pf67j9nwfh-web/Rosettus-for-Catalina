#!/bin/bash
# from_installer.sh - make the Mojave export from Apple's "Install macOS Mojave" app, without a Mojave Mac.
#
# usage: ./from_installer.sh INSTALLER [OUTPUT.tar.gz]
#   INSTALLER   "Install macOS Mojave.app", a .zip of it, or its SharedSupport/InstallESD.dmg
#   OUTPUT      default: ~/Desktop/Rosettus-Mojave-export.tar.gz
#
# Runs on any Mac with macOS 10.13 or later (Catalina included); nothing is installed and no
# administrator password is needed. It mounts the installer's disk image read-only, takes the
# system package's payload (Core.pkg, about 4.5 GB) into a work folder, reads only the files Rosettus
# needs out of it (payload_extract.py: about 1 GB), and copies the 32-bit Intel libraries from them
# with export_from_mojave.py --no-cache: an installer has no 32-bit shared cache (macOS builds it
# while installing), so the libraries are copied as files and the cache is built from them with the
# installer's own update_dyld_shared_cache, as a Mojave installation would. The work folder (about
# 6 GB at most, 11 GB when a .zip is given) is removed afterwards.
# Apple software from your own copy of the installer: for use on your own Macs only.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
IN="$1"; OUT="${2:-$HOME/Desktop/Rosettus-Mojave-export.tar.gz}"
[ -n "$IN" ] && [ -e "$IN" ] || { sed -n '2,16p' "$0"; exit 1; }
say() { printf '==> %s\n' "$*"; }
W="$(mktemp -d "${TMPDIR:-/tmp}/rosettus-mojave-installer.XXXXXX")"
MNT="$W/esd"
cleanup() { hdiutil detach -quiet "$MNT" 2>/dev/null || true; chmod -R u+rwX "$W" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
case "$IN" in
  *.zip)
    say "Unzipping $(basename "$IN") (a few minutes)..."
    ditto -x -k "$IN" "$W/zip"
    IN="$(find "$W/zip" -maxdepth 2 -name '*.app' -type d | head -1)"
    [ -n "$IN" ] || { echo "no .app in the zip"; exit 1; };;
esac
if [ -d "$IN" ]; then ESD="$IN/Contents/SharedSupport/InstallESD.dmg"; else ESD="$IN"; fi
[ -f "$ESD" ] || { echo "$ESD not found: is this the full \"Install macOS Mojave\" app (about 6 GB)?"; exit 1; }
say "Mounting $(basename "$ESD")..."
mkdir -p "$MNT"
hdiutil attach -quiet -nobrowse -readonly -noverify -mountpoint "$MNT" "$ESD"
CORE="$MNT/Packages/Core.pkg"
[ -f "$CORE" ] || { echo "no Packages/Core.pkg in the installer image"; exit 1; }
# (pkgutil --expand-full would unpack all 10 GB of it, and stops with "Permission denied" without
# administrator rights: some of its folders are read-only)
say "Taking the system package's payload (a few minutes)..."
mkdir -p "$W/core"
(cd "$W/core" && xar -xf "$CORE" Payload)
hdiutil detach -quiet "$MNT" || true
rm -rf "$W/zip"
[ -f "$W/core/Payload" ] || { echo "Core.pkg has no Payload"; exit 1; }
say "Reading the 32-bit libraries out of it (a few minutes)..."
ROOT="$W/root"
printf '%s\n' System/Library/CoreServices/SystemVersion.plist usr/bin/update_dyld_shared_cache > "$W/extra.txt"
/usr/bin/python "$HERE/payload_extract.py" "$W/core/Payload" "$ROOT" \
    "$HERE/mojave_i386_files.txt" "$HERE/mojave_i386_cache_files.txt" "$W/extra.txt"
rm -f "$W/core/Payload"
[ -f "$ROOT/System/Library/CoreServices/SystemVersion.plist" ] || { echo "the system package has no SystemVersion.plist"; exit 1; }
xattr -d com.apple.quarantine "$ROOT/usr/bin/update_dyld_shared_cache" 2>/dev/null || true
say "Copying the 32-bit Intel libraries..."
/usr/bin/python "$HERE/export_from_mojave.py" --root "$ROOT" --no-cache "$OUT"
