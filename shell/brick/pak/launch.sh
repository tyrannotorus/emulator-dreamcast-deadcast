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

# Settings, mappings, VMUs and save states are kept in DEADCAST_HOME. The BIOS is read from the pak's
# own bios/ first, then from the firmware's Bios/DC; a missing one is shown on the screen.
export DEADCAST_HOME="$USERDATA_PATH/DC-DEADCAST"
export HOME="$DEADCAST_HOME"
mkdir -p "$DEADCAST_HOME/mappings" "$PAK_DIR/bios"
export DEADCAST_BIOS_PATH="$PAK_DIR/bios:$BIOS_PATH/DC"

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
# and the USB ports as they were, should it have been killed with its USB link up (core/deadcast/link.cpp):
# ADB alone on the bottom port, and the port a device again if deadcast made it a host
G=/sys/kernel/config/usb_gadget/g1
if [ -e "$G/configs/c.1/ffs.deadcast" ]; then
	echo >"$G/UDC"
	rm -f "$G/configs/c.1/ffs.deadcast"
	umount /dev/usb-ffs/deadcast
	rmdir /dev/usb-ffs/deadcast "$G/functions/ffs.deadcast"
	sleep 0.3
	[ -n "$(cat "$G/UDC")" ] || ls /sys/class/udc >"$G/UDC"
fi
if [ -f /tmp/deadcast-link-host ]; then
	# reading this file is what switches the port (Allwinner's driver)
	cat /sys/devices/platform/soc/usbc0/usb_device >/dev/null
	rm -f /tmp/deadcast-link-host
fi
# and the top port's 5 V, off while a Brick there ran on its own battery
if [ -f /tmp/deadcast-link-top-off ]; then
	D=/sys/kernel/debug/sunxi_pinctrl
	echo r_pio >$D/dev_name
	echo PL8 >$D/sunxi_pin
	echo 'PL8 1' >$D/data
	rm -f /tmp/deadcast-link-top-off
fi
