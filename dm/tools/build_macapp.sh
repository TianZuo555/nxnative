#!/bin/bash
# Build a double-clickable macOS .app for one game: the host binary, the rewritten modules, the
# shader libraries and the game files, all inside the bundle.  The result runs without a terminal
# and without setting any environment variable.
#
# Usage: dm/tools/build_macapp.sh [output.app]
#   GAME    game profile (dm/games/<base game title ID>), required
#   W       the game's work folder, required
#   PATCHED rewritten modules       (default $W/patched_<package>)
#   ROMFS   the game files          (default $W/extract/<package>/RomFS)
#   MSL     shader libraries        (default $W/libout)
#   SAVE    save data to seed       (default $W/save)
#   AOC     add-on content          (default none)
#
# The game files are 11 GB for Mario Kart 8 Deluxe.  They are copied into the bundle only when
# DM_MACAPP_LINK=0; by default they are hard-linked (same volume, no extra space, still a real
# directory inside the .app) and fall back to a copy across volumes.
set -u
cd "$(dirname "$0")/.." || exit 1
DM=$PWD                     # this script lives in dm/tools, so ".." is the dm directory
t() { echo "[$(date +%T)] $*"; }; die() { t "FAIL: $*"; exit 1; }
[ -n "${GAME:-}" ] || die "GAME is not set: GAME=<base game title ID> $0"
export GAME; GD=$DM/games/$GAME
gp() { python3 "$DM/games/game.py" "$GAME" "$1" || die "game profile $GD: $1"; }
[ -f "$GD/game.tsv" ] || die "no game profile $GD/game.tsv"
[ -n "${W:-}" ] || die "W is not set: W=<the game's work folder> $0"
PKG=$(gp package); MODS=$(gp modules); MARKER=$(gp romfs_marker)
PATCHED=${PATCHED:-$W/patched_$PKG}
ROMFS=${ROMFS:-$W/extract/$PKG/RomFS}
MSL=${MSL:-$W/libout}
SAVE=${SAVE:-$W/save}
AOC=${AOC:-}
OUT=${1:-$(dirname "$W")/$(gp package).app}
NAME=$(gp name)
V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' "$GD/dmsh_chain.h")

for f in $MODS; do [ -f "$PATCHED/$f.patched.bin" ] && [ -f "$PATCHED/$f.fixups.tsv" ] || die "no rewritten module $PATCHED/$f (run dm/tools/patch_nso.py first)"; done
[ -e "$ROMFS/$MARKER" ] || die "no game files at $ROMFS (missing $MARKER)"
[ -n "$V" ] && ls "$MSL/b-$V" 2>/dev/null | grep -q '_b.metallib$' || die "no shader libraries $MSL/b-$V"

t "app: $NAME"
t "host: make GAME=$GAME"
make -C "$DM/host" GAME=$GAME dmhost || die "host build failed"
BIN=$DM/host/dmhost

rm -rf "$OUT"; mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources/dm" "$OUT/Contents/Resources/msl" || die "mkdir bundle"
A=$OUT/Contents

# ---- executable -------------------------------------------------------------------------------
cp "$BIN" "$A/MacOS/dmhost" || die "copy host"
for f in $MODS; do cp "$PATCHED/$f.patched.bin" "$A/MacOS/" || die "copy $f"; done
for ext in fixups gates params cuts obs; do cp "$PATCHED"/*.$ext.tsv "$A/MacOS/" 2>/dev/null; done
cp "$GD/shader_map.tsv" "$GD/pso_manifest.tsv" "$GD/c1_full.tsv" "$A/Resources/dm/" || die "copy shader tables"
mkdir -p "$A/Resources/msl" && cp -cR "$MSL/b-$V" "$A/Resources/msl/" || cp -R "$MSL/b-$V" "$A/Resources/msl/" || die "copy shader libraries"
# Shared-font stand-ins.  macOS already has Arial Unicode where the host looks for it, so these are
# only needed if the host is ever pointed at DM_FONT_DIR; copy them when they exist.
for f in "$DM/ios/Gen/dm/font/standard.ttf" "$DM/ios/Gen/dm/font/nintendo_ext.ttf"; do [ -f "$f" ] && cp "$f" "$A/Resources/dm/"; done

# ---- game files -------------------------------------------------------------------------------
t "game files: $ROMFS"
# cp -Rl hard-links (clone -cRl does not: it copies); falling back to a plain copy only when the
# target is on another volume.
if [ "${DM_MACAPP_LINK:-1}" = "0" ]; then cp -cR "$ROMFS" "$A/Resources/romfs" || cp -R "$ROMFS" "$A/Resources/romfs" || die "copy game files"
else cp -Rl "$ROMFS" "$A/Resources/romfs" 2>/dev/null || cp -cR "$ROMFS" "$A/Resources/romfs" || cp -R "$ROMFS" "$A/Resources/romfs" || die "link game files"; fi
[ -e "$A/Resources/romfs/$MARKER" ] || die "game files incomplete"
if [ -n "$AOC" ] && [ -d "$AOC" ]; then t "add-on content: $AOC"; cp -Rl "$AOC" "$A/Resources/aoc" 2>/dev/null || cp -R "$AOC" "$A/Resources/aoc"; fi

# ---- save data ---------------------------------------------------------------------------------
# Seeded into ~/Library/Application Support/<app>/save on first run, so an unlocked save works out of
# the box; afterwards the user's own copy wins and is never overwritten.
if [ -d "$SAVE" ]; then t "save seed: $SAVE"; cp -Rl "$SAVE" "$A/Resources/save-seed" 2>/dev/null || cp -R "$SAVE" "$A/Resources/save-seed" || die "copy save seed"; fi

# ---- launcher ----------------------------------------------------------------------------------
# A shell wrapper instead of the binary as CFBundleExecutable: it sets the paths the host expects and
# keeps a log where the user can find it.  Everything it sets is a path inside the bundle or the
# user's own Application Support directory.
cat > "$A/MacOS/launch" <<'LAUNCH'
#!/bin/bash
# Set up the host's paths, then run the game.  All state lives in
# ~/Library/Application Support/<bundle name>/, so the app can be moved or replaced freely.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)/Resources"
SUP="$HOME/Library/Application Support/$(basename "$(dirname "$(dirname "$(dirname "$0")")")" .app)"
mkdir -p "$SUP/save" "$SUP/temp"
# First run: take the save data that shipped with the app (an unlocked save, if one was bundled).
if [ -d "$R/save-seed" ] && [ -z "$(ls -A "$SUP/save" 2>/dev/null)" ]; then
    echo "首次运行：写入随包的存档" ; cp -R "$R/save-seed/." "$SUP/save/" 2>/dev/null
fi
# One log per run, the previous one kept: the host writes diagnostics to stderr and nothing else.
[ -f "$SUP/log.txt" ] && mv -f "$SUP/log.txt" "$SUP/log.prev.txt"
export DM_GAME="__GAME__"
export DM_ROMFS="$R/romfs"
export DM_SAVE="$SUP/save"
export DM_TEMP="$SUP/temp"
export DM_MSL_DIR="$R/msl"
export DM_NVN_SHMAP="$R/dm/shader_map.tsv"
export DM_FONT_DIR="$R/dm"
[ -d "$R/aoc" ] && export DM_AOC_DIR="$R/aoc"
# The window and keyboard front end: without this the host runs headless with no way to play.
export DM_WINDOW=1
cd "$SUP" || exit 1
exec "$(dirname "$0")/dmhost" "$(dirname "$0")" __MODULES__ "$@" > "$SUP/log.txt" 2>&1
LAUNCH
sed -i '' "s|__GAME__|$GAME|; s|__MODULES__|$MODS|" "$A/MacOS/launch"
chmod +x "$A/MacOS/launch"

cat > "$A/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$NAME</string>
    <key>CFBundleDisplayName</key><string>$NAME</string>
    <key>CFBundleIdentifier</key><string>org.nxnative.$(echo "$GAME" | tr 'A-Z' 'a-z')</string>
    <key>CFBundleExecutable</key><string>launch</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict>
</plist>
PLIST
[ -f "$W/extract/$PKG/icon.jpg" ] && cp "$W/extract/$PKG/icon.jpg" "$A/Resources/icon.jpg" && sips -s format icns "$A/Resources/icon.jpg" --out "$A/Resources/icon.icns" >/dev/null 2>&1 && echo "    <key>CFBundleIconFile</key><string>icon</string>" >/dev/null

# Unsigned bundles are fine locally; ad-hoc signing keeps Gatekeeper from calling it damaged after a
# move.  Not an error if it fails (no developer identity needed for an ad-hoc signature).
codesign --force --deep --sign - "$OUT" 2>/dev/null && t "ad-hoc signed"

t "done: $OUT ($(du -sh "$OUT" | awk '{print $1}'))"
t "run it from Finder, or: open '$OUT'"
t "log: ~/Library/Application Support/$(basename "$OUT" .app)/log.txt"
