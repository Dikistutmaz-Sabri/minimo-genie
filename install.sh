#!/bin/sh
# minimo-genie: install into Minimo OS's session (or take it out again with --remove).
#
#   ./install.sh          build, then:
#     1. ~/.local/lib/minimo-genie/minimo-genie.so          the plugin (new file)
#     2. ~/.config/hypr/minimo-oturum.lua                   + one line at the end, loading it:
#          pcall(hl.plugin.load, os.getenv("HOME") .. "/.local/lib/minimo-genie/minimo-genie.so") -- minimo-genie
#     3. ~/.config/hypr/minimo/serbest.lua                  minimise (Super+M, the dock, the title bar's
#                                                           amber drop) flows into the dock when the plugin
#                                                           is loaded, instead of shrinking  (minimo/serbest.lua.patch)
#     4. ~/.config/pleamar/shells/minimo-dock/dock.luau     the dock writes where its icons are to
#                                                           $XDG_RUNTIME_DIR/minimo-genie   (minimo/dock.luau.patch)
#   2-4 are links into ~/Proyectos/minimo-ayarlar: the real files there are changed. Before
#   anything is changed they are copied to ./backup/<time>/.
#   ./install.sh --remove   undoes 1-4 (the line out, the patches reversed, the plugin deleted).
# No sudo, no packages, no services restarted: log out and into «Minimo OS» (or see the end).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SO_DIR="$HOME/.local/lib/minimo-genie"
SESSION=$(readlink -f "$HOME/.config/hypr/minimo-oturum.lua")
SERBEST=$(readlink -f "$HOME/.config/hypr/minimo/serbest.lua")
DOCK=$(readlink -f "$HOME/.config/pleamar/shells/minimo-dock/dock.luau")
MARK="-- minimo-genie"
LINE="pcall(hl.plugin.load, os.getenv(\"HOME\") .. \"/.local/lib/minimo-genie/minimo-genie.so\") $MARK"

for f in "$SESSION" "$SERBEST" "$DOCK"; do
	[ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

backup() {
	B="$HERE/backup/$(date +%Y%m%d-%H%M%S)"
	mkdir -p "$B"
	cp -p "$SESSION" "$SERBEST" "$DOCK" "$B/"
	echo "backup: $B"
}

if [ "${1:-}" = "--remove" ]; then
	# check first, change after: nothing is half undone
	if grep -q "minimo-genie" "$SERBEST"; then patch --dry-run -R -s "$SERBEST" "$HERE/minimo/serbest.lua.patch" >/dev/null; fi
	if grep -q "minimo-genie" "$DOCK"; then patch --dry-run -R -s "$DOCK" "$HERE/minimo/dock.luau.patch" >/dev/null; fi
	backup
	sed -i "/$MARK\$/d" "$SESSION"
	if grep -q "minimo-genie" "$SERBEST"; then patch -R -s "$SERBEST" "$HERE/minimo/serbest.lua.patch"; fi
	if grep -q "minimo-genie" "$DOCK"; then patch -R -s "$DOCK" "$HERE/minimo/dock.luau.patch"; fi
	[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] && hyprctl plugin unload "$SO_DIR/minimo-genie.so" >/dev/null 2>&1 || true
	rm -rf "$SO_DIR"
	rm -f "${XDG_RUNTIME_DIR:-/tmp}/minimo-genie"
	echo "removed. In Minimo: Super+Shift+Escape (or log out and in) to finish."
	exit 0
fi

# built against the Hyprland installed now (a plugin for another version is refused by Hyprland)
make -C "$HERE" check minimo-genie.so

if ! grep -q "minimo-genie" "$SERBEST"; then patch --dry-run -s "$SERBEST" "$HERE/minimo/serbest.lua.patch" >/dev/null; fi
if ! grep -q "minimo-genie" "$DOCK"; then patch --dry-run -s "$DOCK" "$HERE/minimo/dock.luau.patch" >/dev/null; fi
backup

mkdir -p "$SO_DIR"
# a new file renamed over the old one: a running Hyprland keeps the old one it has loaded
cp "$HERE/minimo-genie.so" "$SO_DIR/.minimo-genie.so.new"
mv -f "$SO_DIR/.minimo-genie.so.new" "$SO_DIR/minimo-genie.so"

if ! grep -q -e "$MARK\$" "$SESSION"; then
	[ -z "$(tail -c1 "$SESSION")" ] || echo >>"$SESSION"
	echo "$LINE" >>"$SESSION"
fi
if ! grep -q "minimo-genie" "$SERBEST"; then patch -s "$SERBEST" "$HERE/minimo/serbest.lua.patch"; fi
if ! grep -q "minimo-genie" "$DOCK"; then patch -s "$DOCK" "$HERE/minimo/dock.luau.patch"; fi

echo "installed. Log out and into «Minimo OS»; or, inside it now:"
echo "  systemctl --user restart minimo-dock && hyprctl reload"
echo "After every Hyprland update: ./install.sh again (it rebuilds)."
