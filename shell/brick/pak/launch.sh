#!/bin/sh
# Deadcast for NextUI on the TrimUI Brick (tg5040). NextUI passes the game picked in Roms: a disc
# (.chd, .gdi, .cdi) or an .m3u naming one.
PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"

rm -f "$LOGS_PATH/$PAK_NAME.txt"
exec >>"$LOGS_PATH/$PAK_NAME.txt" 2>&1
echo "$0" "$@"
cd "$PAK_DIR" || exit 1

ROM="$1"
case "$ROM" in
*.m3u | *.M3U)
	# the first disc listed, relative to the playlist's folder unless absolute
	DISC="$(tr -d '\r' <"$ROM" | grep -v '^[[:space:]]*$' | grep -v '^#' | head -n 1)"
	case "$DISC" in
	/*) ;;
	*) DISC="$(dirname "$ROM")/$DISC" ;;
	esac
	;;
*)
	DISC="$ROM"
	;;
esac
if [ ! -f "$DISC" ]; then
	echo "Deadcast: no disc at $DISC"
	exit 1
fi

# Settings, mappings, VMUs and save states are kept in DEADCAST_HOME; the BIOS is read from Bios/DC.
export DEADCAST_HOME="$USERDATA_PATH/DC-Deadcast"
export HOME="$DEADCAST_HOME"
export DEADCAST_BIOS_PATH="$BIOS_PATH/DC"
mkdir -p "$DEADCAST_HOME/mappings"

if [ ! -f "$DEADCAST_BIOS_PATH/dc_boot.bin" ]; then
	echo "Deadcast: missing $DEADCAST_BIOS_PATH/dc_boot.bin"
	exit 1
fi

# The default settings and controls, on the first run. The Brick's controls appear as an Xbox 360
# pad, and Flycast names a mapping after the pad.
if [ ! -f "$DEADCAST_HOME/emu.cfg" ]; then
	cp "$PAK_DIR/config/emu.cfg" "$DEADCAST_HOME/emu.cfg"
fi
MAPPING="SDL_Xbox 360 Controller.cfg"
if [ ! -f "$DEADCAST_HOME/mappings/$MAPPING" ]; then
	cp "$PAK_DIR/config/$MAPPING" "$DEADCAST_HOME/mappings/$MAPPING"
fi
# For the menu: the game's name, and the settings Restore defaults goes back to
export DEADCAST_ROM="$ROM"
export DEADCAST_DEFAULTS="$PAK_DIR/config/emu.cfg"

cleanup() {
	rm -f /tmp/stay_awake
	sync
}
trap cleanup EXIT INT TERM HUP QUIT

# no auto sleep while playing
echo 1 >/tmp/stay_awake
# NextUI starts paks at its performance clock; run at its Auto, as its own emulators do
sh "$SYSTEM_PATH/bin/governor.sh" auto

echo "Deadcast: $DISC"
"$PAK_DIR/deadcast" "$DISC"
echo "Deadcast: deadcast exited with $?"
# NextUI's display mode again, should deadcast have been killed before restoring it
command -v fbset > /dev/null && fbset -g 1024 768 1024 16384 32
