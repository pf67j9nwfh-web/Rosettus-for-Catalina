#!/bin/bash
# tools/mkpkg.sh [VERSION] - build "Rosettus for Catalina VERSION.pkg" in dist/.
#
# The package contains only this project's files (no Apple software), like the repository:
#   /Applications/Rosettus Launcher.app      the GUI, for every user
#   /Library/Rosettus/Setup/                 install.sh, the patches, manifest and runtime files
# Its postinstall script finishes the install for every user (install.sh --system) when it finds
# both exports (the Snow Leopard one and the Mojave one): next to the package, or in the logged-in
# user's Downloads or Desktop. Otherwise Rosettus Launcher asks for them the first time it is opened.
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
# Refuse to package a bin/ file whose sources changed after it was built (see tools/binstamp.sh).
"$R/tools/binstamp.sh" --check || [ -n "$ROSETTUS_ALLOW_STALE" ] || {
  echo "mkpkg: rebuild the files above and run tools/binstamp.sh --update NAME (or set ROSETTUS_ALLOW_STALE=1)"; exit 1; }
V="${1:-1.0}"
V="${V#v}"
W="$(mktemp -d /tmp/rmpkg.XXXXXX)"
trap 'rm -rf "$W"' EXIT
S="$W/root/Library/Rosettus/Setup"
mkdir -p "$S" "$W/root/Applications" "$W/scripts" "$W/res" "$R/dist"
for d in import bin scripts bridges agents tools; do cp -R "$R/$d" "$S/"; done
cp "$R/install.sh" "$R/uninstall.sh" "$R/LICENSE" "$R/RELEASE_NOTES.md" "$S/"
# The Snow Leopard side (export_from_sl.sh, Install Rosetta.app, the Java export for Tiger) stays in
# the Setup kit, and a copy the user can find goes to /Applications/Rosettus as the "Snow Leopard
# Export Kit": the folder to copy to the Snow Leopard Mac, with its own README.
K="$W/root/Applications/Rosettus/Snow Leopard Export Kit"
mkdir -p "$K"
cp -R "$R/import/sl/." "$K/"
rm -f "$K/dump_text_ppc.c"
mkdir -p "$K/tiger"; cp -R "$R/import/tiger/." "$K/tiger/"
cp "$R/import/manifest.tsv.gz" "$K/manifest.tsv.gz"
cp "$R/import/sl/README.txt" "$K/README.txt" 2>/dev/null || true
# Without a Snow Leopard Mac: the same export made on this Mac from a Snow Leopard install DVD and
# Apple's 10.6.8 updates (import/dvd), as a folder with a double-clickable .command
D="$W/root/Applications/Rosettus/Snow Leopard Install DVD Import"
mkdir -p "$D"
cp -R "$R/import/dvd/." "$D/"
cp "$R/import/sl/export_from_sl.py" "$R/import/manifest.tsv.gz" "$D/"
# Mojave's 32-bit Intel libraries (import/mojave): the folder to copy to a Mojave Mac
M="$W/root/Applications/Rosettus/Mojave Export Kit"
mkdir -p "$M"
cp -R "$R/import/mojave/." "$M/"
rm -f "$M/commdump.c"
chmod 755 "$M"/*.command "$M"/*.py "$M/commdump"
# PowerPC Java from Apple's installers (import/java), with a double-clickable .command
J="$W/root/Applications/Rosettus/PowerPC Java Import"
mkdir -p "$J"
cp -R "$R/import/java/." "$J/"
chmod 755 "$J"/*.sh "$J"/*.command
find "$S" -name .DS_Store -delete
cp -R "$R/bin/Rosettus Launcher.app" "$W/root/Applications/"
/usr/libexec/PlistBuddy -c "Delete :RosettusRoot" "$W/root/Applications/Rosettus Launcher.app/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :RosettusRoot string /Library/Rosettus" "$W/root/Applications/Rosettus Launcher.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $V" "$W/root/Applications/Rosettus Launcher.app/Contents/Info.plist"   # the release shows in Get Info / About
codesign -f -s - "$W/root/Applications/Rosettus Launcher.app" 2>/dev/null || true
# Rosettus Preference Pane Viewer: in /Applications as well (install.sh puts it there; the package
# runs install.sh with --no-apps, so it has to be in the payload)
if [ -d "$R/bin/Rosettus Preference Pane Viewer.app" ]; then
  cp -R "$R/bin/Rosettus Preference Pane Viewer.app" "$W/root/Applications/"
  /usr/libexec/PlistBuddy -c "Delete :RosettusRoot" "$W/root/Applications/Rosettus Preference Pane Viewer.app/Contents/Info.plist" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :RosettusRoot string /Library/Rosettus" "$W/root/Applications/Rosettus Preference Pane Viewer.app/Contents/Info.plist"
  codesign -f -s - "$W/root/Applications/Rosettus Preference Pane Viewer.app" 2>/dev/null || true
fi

cat > "$W/scripts/postinstall" <<'EOF'
#!/bin/bash
# Rosettus package: finish the install for every user account.
exec >> /Library/Logs/RosettusInstall.log 2>&1
echo "=== $(date) postinstall"
S=/Library/Rosettus/Setup
chown -R root:wheel "$S"; chmod -R go-w "$S"
u=$(stat -f %Su /dev/console); h=""
[ -n "$u" ] && [ "$u" != root ] && h=$(dscl . -read "/Users/$u" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
EXPORT=""; MJ=""
for d in "$(dirname "$PACKAGE_PATH")" ${h:+"$h/Downloads" "$h/Desktop"}; do
  [ -z "$EXPORT" ] && [ -f "$d/Rosettus-SL-export.tar.gz" ] && EXPORT="$d/Rosettus-SL-export.tar.gz"
  [ -z "$MJ" ] && [ -f "$d/Rosettus-Mojave-export.tar.gz" ] && MJ="$d/Rosettus-Mojave-export.tar.gz"
done
if [ -x /Library/Rosettus/translate_mojave ] && [ -f /Library/Rosettus/i386/var/db/dyld/dyld_shared_cache_i386 ]; then
  echo "updating the existing install"
  "$S/install.sh" --update --system --no-apps; st=$?
elif [ -n "$EXPORT" ] && [ -n "$MJ" ]; then
  echo "installing from $EXPORT and $MJ"
  "$S/install.sh" "$EXPORT" "$MJ" --system --no-apps; st=$?
else
  echo "the Snow Leopard and Mojave exports were not both found: Rosettus Launcher will ask for them"
  mkdir -p /Applications/Rosettus; chown root:admin /Library/Rosettus /Applications/Rosettus; chmod 775 /Library/Rosettus /Applications/Rosettus
  st=0
fi
chown -R root:admin "/Applications/Rosettus Launcher.app"; chmod -R go-w "/Applications/Rosettus Launcher.app"
PV="/Applications/Rosettus Preference Pane Viewer.app"
if [ -d "$PV" ]; then
  # this Mac's System Preferences icon for the viewer and the pane host (Apple artwork is not shipped)
  # (Catalina keeps System Preferences in /System/Applications)
  icon="/System/Applications/System Preferences.app/Contents/Resources/PrefApp.icns"
  [ -f "$icon" ] || icon="/Applications/System Preferences.app/Contents/Resources/PrefApp.icns"
  if [ -f "$icon" ]; then
    for app in "$PV" "/Library/Rosettus/prefpanes/Rosettus Preference Pane Host.app"; do
      [ -d "$app" ] && mkdir -p "$app/Contents/Resources" && cp "$icon" "$app/Contents/Resources/AppIcon.icns"
    done
    codesign -f -s - "$PV" 2>/dev/null
  fi
  chown -R root:admin "$PV"; chmod -R go-w "$PV"
fi
echo "postinstall exit $st"
exit $st
EOF
chmod 755 "$W/scripts/postinstall"

# Bundles are not relocatable: Rosettus Launcher.app is in the payload twice (in /Applications and in
# Setup/bin, with the same bundle identifier). With relocation (pkgbuild's default) an upgrade over an
# installed copy sent both to /Applications and failed: "Rosettus Launcher.app couldn't be moved to
# Applications because an item with the same name already exists".
pkgbuild --analyze --root "$W/root" "$W/components.plist" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i:RootRelativeBundlePath" "$W/components.plist" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$W/components.plist" >/dev/null 2>&1 || true
  /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$W/components.plist"
  i=$((i + 1))
done
pkgbuild --root "$W/root" --scripts "$W/scripts" --component-plist "$W/components.plist" --identifier io.github.rosettus.catalina --version "$V" \
         --install-location / --ownership recommended "$W/Rosettus.pkg" >/dev/null

cat > "$W/res/welcome.html" <<'EOF'
<html><body style="font-family:-apple-system,Helvetica;font-size:13px">
<h2>Rosettus for Catalina</h2>
<p>Runs PowerPC Mac applications on macOS 10.15 Catalina with the Rosetta translator from Mac OS X 10.6 Snow Leopard.</p>
<p>This package installs <b>Rosettus Launcher</b> in the Applications folder and the Rosettus runtime in
<code>/Library/Rosettus</code>, for every user account on this Mac.</p>
<p><b>No Apple software is included.</b> Rosettus for Catalina needs two sets of files from your own Macs:</p>
<ul>
<li><b>Rosettus-SL-export.tar.gz</b> from a Snow Leopard Mac (the Rosetta translator and the PowerPC system):
run <code>export_from_sl.sh</code> from the <i>Snow Leopard Export Kit</i> there.</li>
<li><b>Rosettus-Mojave-export.tar.gz</b> from a Mac running macOS 10.14 Mojave (the 32-bit Intel libraries
Catalina no longer has): double-click <code>export_from_mojave.command</code> in the <i>Mojave Export Kit</i> there.
No Mojave Mac? <code>from_installer.sh</code> in the same kit makes it on any Mac, this one included, from
Apple's <i>Install macOS Mojave</i> app.</li>
</ul>
<p>Put both next to this package or into your Downloads folder before installing. If they are not found,
Rosettus Launcher asks for them the first time you open it. The kits are installed in
<b>Applications &rsaquo; Rosettus</b>.</p>
<p>No Snow Leopard Mac? <b>Applications &rsaquo; Rosettus &rsaquo; Snow Leopard Install DVD Import</b>
makes the Snow Leopard export from a Mac OS X 10.6 install DVD (or its disc image) and Apple's free 10.6.8 updates.</p>
</body></html>
EOF
cat > "$W/res/conclusion.html" <<'EOF'
<html><body style="font-family:-apple-system,Helvetica;font-size:13px">
<h2>Installed</h2>
<p>Open <b>Rosettus Launcher</b> from the Applications folder, click <b>Search This Mac</b>, then <b>Patch All…</b>.
Patched apps open with a normal double-click for every user. Administrators can patch apps; everyone can run them.</p>
<p>Install log: <code>/Library/Logs/RosettusInstall.log</code>. To remove everything:
<code>sudo /Library/Rosettus/Setup/uninstall.sh --system</code></p>
</body></html>
EOF
cat > "$W/dist.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Rosettus for Catalina $V</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="never" require-scripts="true" rootVolumeOnly="true" hostArchitectures="x86_64"/>
    <volume-check><allowed-os-versions><os-version min="10.15" before="10.16"/></allowed-os-versions></volume-check>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline><line choice="default"><line choice="io.github.rosettus.catalina"/></line></choices-outline>
    <choice id="default"/>
    <choice id="io.github.rosettus.catalina" visible="false"><pkg-ref id="io.github.rosettus.catalina"/></choice>
    <pkg-ref id="io.github.rosettus.catalina" version="$V" onConclusion="none">Rosettus.pkg</pkg-ref>
</installer-gui-script>
EOF
OUT="$R/dist/Rosettus for Catalina $V.pkg"
productbuild --distribution "$W/dist.xml" --resources "$W/res" --package-path "$W" "$OUT" >/dev/null
echo "built $OUT ($(du -h "$OUT" | cut -f1))"
