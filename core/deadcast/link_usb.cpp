// The link over USB (link_core.h): the other Brick's function found on a host port and led through
// usbfs, or this Brick's function (FunctionFS, next to adbd's in g1) followed from the bottom port; the
// bottom port's role settled between the two Bricks when they are cabled bottom to bottom.
#include "link_core.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <random>
#include <thread>

namespace deadcast::link::core
{
namespace
{

// adbd's gadget (the firmware's /etc/init.d/adbd sets it up) and the function added to it
constexpr const char *Gadget = "/sys/kernel/config/usb_gadget/g1";
constexpr const char *GadgetUdc = "/sys/kernel/config/usb_gadget/g1/UDC";
constexpr const char *Function = "/sys/kernel/config/usb_gadget/g1/functions/ffs.deadcast";
constexpr const char *Linked = "/sys/kernel/config/usb_gadget/g1/configs/c.1/ffs.deadcast";
constexpr const char *Mount = "/dev/usb-ffs/deadcast";
// The bottom port's role: usb_host, usb_device or null (nothing seen on it). Allwinner's driver SWITCHES
// it when usb_host, usb_device or usb_null is read (by cat too); otg_role only tells it. HostMark:
// Deadcast made it a host, for launch.sh to undo should Deadcast be killed.
constexpr const char *OtgRole = "/sys/devices/platform/soc/usbc0/otg_role";
constexpr const char *OtgHost = "/sys/devices/platform/soc/usbc0/usb_host";
constexpr const char *OtgDevice = "/sys/devices/platform/soc/usbc0/usb_device";
constexpr const char *OtgNull = "/sys/devices/platform/soc/usbc0/usb_null";
// The bottom port's 5 V out as a host: 0 off (TrimUI's usb_dock.sh: a host whose power comes from the
// other side). Written, never read (like the role files, its read may act). Off whenever this Brick hosts
// over its bottom port: the other Brick (its port pinned a device) links without it (seen 2026-11-01:
// found in 4 s, kept with the 5 V on and off again), so no Brick ever powers the other over that cable,
// and two hosting at once while they settle which hosts put no two 5 V supplies on one wire.
constexpr const char *Axp5v = "/sys/devices/platform/soc/usbc0/axp_5v";
constexpr const char *HostMark = "/tmp/deadcast-link-host";
// power coming into the bottom port that the PMIC accepts: a charger's, a computer's, a Brick's top
// port's (not a Brick's bottom port's: it reads 0 then, even with 5 V on the pins)
constexpr const char *PowerIn = "/sys/class/power_supply/axp2202-usb/online";
// The top port's 5 V: a switch on pin PL8 (usb1-vbus, which its host controllers turn on), off while a
// Brick linked there runs on its own battery; through the pin controller's debug files, as user space
// has no other way to it. TopOffMark: it is off, for the guard process and launch.sh to undo.
constexpr const char *PinDebug = "/sys/kernel/debug/sunxi_pinctrl";
constexpr const char *TopOffMark = "/tmp/deadcast-link-top-off";

// the function's interface, for the host to find
constexpr unsigned Class = 0xff, SubClass = 0xdc, Protocol = 1;
constexpr char InterfaceName[] = "Deadcast link";

struct __attribute__((packed)) Interface
{
	usb_interface_descriptor intf;
	usb_endpoint_descriptor_no_audio in, out;
};
struct __attribute__((packed)) Descriptors
{
	usb_functionfs_descs_head_v2 header;
	__le32 fsCount, hsCount;
	Interface fs, hs;
};
struct __attribute__((packed)) Strings
{
	usb_functionfs_strings_head header;
	__le16 language;
	char name[sizeof(InterfaceName)];
};

// never destroyed: a joinable std::thread at exit would abort
std::thread *server;
std::atomic<bool> serving { false };
std::string udc;

struct Device
{
	int ep0 = -1, in = -1, out = -1;
	bool enabled = false;		// a host has the function
} device;

struct Host
{
	int fd = -1;
	int intf = 0;
	unsigned in = 0, out = 0;
	std::string node, port;
} host;

int bulk(unsigned endpoint, void *data, u32 size, u32 timeoutMs);

// leading the other Brick's function: usbfs bulk transfers
struct HostPipe : Pipe
{
	int send(const void *data, u32 size, u32 timeoutMs) override
	{
		return bulk(host.out, (void *)data, size, timeoutMs);
	}
	int receive(void *data, u32 size, u32 timeoutMs) override
	{
		const int n = bulk(host.in, data, size, timeoutMs);
		return n >= 0 ? n : errno == ETIMEDOUT ? 0 : -1;
	}
	u32 chunk() const override { return 65536; }
	u32 testEchoes() const override { return 1000; }
	u32 testBytes() const override { return 16 << 20; }
} hostPipe;

// following on this Brick's function: its endpoint files (blocking once a host enables them; until then
// they fail at once, and the follower tries again after 20 ms). It ends with deviceDown().
struct DevicePipe : Pipe
{
	int send(const void *data, u32 size, u32) override
	{
		return (int)write(device.in, data, size);
	}
	int receive(void *data, u32 size, u32) override
	{
		const ssize_t n = read(device.out, data, size);
		if (n >= 0)
			return (int)n;
		sleepMs(20);
		return 0;
	}
	u32 chunk() const override { return 65536; }
	u32 testEchoes() const override { return 1000; }
	u32 testBytes() const override { return 16 << 20; }
} devicePipe;

// the PMIC's 5 V out of the bottom port (off while Deadcast hosts there: Axp5v)
std::string bottomPower()
{
	if (DIR *dir = opendir("/sys/class/regulator"))
	{
		std::string state = "unknown";
		while (dirent *e = readdir(dir))
		{
			const std::string r = std::string("/sys/class/regulator/") + e->d_name;
			if (readText(r + "/name") == "axp2202-drivevbus")
				state = readText(r + "/state");
		}
		closedir(dir);
		return state;
	}
	return "unknown";
}

// the top port's 5 V switched on or off; false if the pin cannot be set
bool topPower(bool on)
{
	const std::string dir = PinDebug;
	if (!writeText((dir + "/dev_name").c_str(), "r_pio") || !writeText((dir + "/sunxi_pin").c_str(), "PL8")
			|| !writeText((dir + "/data").c_str(), on ? "PL8 1" : "PL8 0"))
	{
		log("top port: its 5 V not switched %s: %s", on ? "on" : "off", std::strerror(errno));
		return false;
	}
	if (on)
		unlink(TopOffMark);
	else if (FILE *f = fopen(TopOffMark, "w"))
		fclose(f);
	log("top port: its 5 V %s", on ? "on" : "off");
	return true;
}

// a USB device on the top port (its controllers at 0x5200000): this Brick is the host there
bool topPortInUse()
{
	bool used = false;
	if (DIR *dir = opendir("/sys/bus/usb/devices"))
	{
		while (const dirent *e = readdir(dir))
		{
			// devices only: not interfaces (1-1:1.0) nor root hubs (usb1)
			if (e->d_name[0] == '.' || std::strchr(e->d_name, ':') != nullptr || std::strncmp(e->d_name, "usb", 3) == 0)
				continue;
			char real[PATH_MAX];
			const std::string path = std::string("/sys/bus/usb/devices/") + e->d_name;
			if (realpath(path.c_str(), real) != nullptr && std::strstr(real, "5200000.") != nullptr)
				used = true;
		}
		closedir(dir);
	}
	return used;
}

// the bottom port as the drivers see it: its role, power coming in, and the PMIC's 5 V going out
std::string bottomState()
{
	return readText(OtgRole) + ", power in " + (readText(PowerIn) == "1" ? "yes" : "no")
			+ ", 5 V out " + bottomPower();
}

// ---- the device's side: the function in g1, a thread following the host's commands

bool bound()
{
	return !readText(GadgetUdc).empty();
}

// g1 on the UDC again. adbd races for it (it writes UDC itself once its function is ready again).
void rebind()
{
	for (int i = 0; i < 25 && !bound(); i++)
	{
		writeText(GadgetUdc, udc.c_str());
		sleepMs(200);
	}
	if (!bound())
		log("g1 is still not on %s", udc.c_str());
}

void deviceDown(bool quiet = false)
{
	serving = false;
	// unbound, a transfer under way ends at once
	writeText(GadgetUdc, "\n");
	unlink(Linked);
	// the follower's thread ends (IP down with it)
	if (server != nullptr)
	{
		server->join();
		delete server;
		server = nullptr;
	}
	// ep0 closed unbinds g1 too, hence the rebind after
	for (int *fd : { &device.in, &device.out, &device.ep0 })
		if (*fd >= 0)
		{
			close(*fd);
			*fd = -1;
		}
	umount2(Mount, MNT_DETACH);
	rmdir(Mount);
	rmdir(Function);
	device.enabled = false;
	sleepMs(300);
	rebind();
	if (!quiet)
		log("device: the function is down");
}

// interface number, its protocol and string, two bulk endpoints (first in, first + 1 out)
Interface interface(u16 packetSize, u8 number, u8 protocol, u8 name, u8 first)
{
	Interface i {};
	i.intf = { USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, number, 0, 2, Class, SubClass, protocol, name };
	i.in = { USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, (u8)(first | USB_DIR_IN), USB_ENDPOINT_XFER_BULK, packetSize, 0 };
	i.out = { USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, (u8)((first + 1) | USB_DIR_OUT), USB_ENDPOINT_XFER_BULK, packetSize, 0 };
	return i;
}

bool deviceUp(bool quiet = false)
{
	if (udc.empty() || access(Gadget, F_OK) != 0)
	{
		log("device: no gadget or UDC");
		return false;
	}
	if (mkdir(Function, 0755) != 0 && errno != EEXIST)
	{
		log("device: cannot make %s: %s", Function, std::strerror(errno));
		return false;
	}
	mkdir("/dev/usb-ffs", 0755);
	mkdir(Mount, 0755);
	if (mount("deadcast", Mount, "functionfs", 0, nullptr) != 0 && errno != EBUSY)
	{
		log("device: cannot mount functionfs: %s", std::strerror(errno));
		rmdir(Function);
		return false;
	}
	Descriptors descriptors {};
	descriptors.header.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
	descriptors.header.length = sizeof(descriptors);
	descriptors.header.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;
	descriptors.fsCount = descriptors.hsCount = 3;
	descriptors.fs = interface(64, 0, Protocol, 1, 1);
	descriptors.hs = interface(512, 0, Protocol, 1, 1);
	Strings strings {};
	strings.header.magic = FUNCTIONFS_STRINGS_MAGIC;
	strings.header.length = sizeof(strings);
	strings.header.str_count = 1;
	strings.header.lang_count = 1;
	strings.language = 0x0409;
	std::memcpy(strings.name, InterfaceName, sizeof(InterfaceName));
	device.ep0 = open((std::string(Mount) + "/ep0").c_str(), O_RDWR);
	if (device.ep0 < 0 || write(device.ep0, &descriptors, sizeof(descriptors)) != sizeof(descriptors)
			|| write(device.ep0, &strings, sizeof(strings)) != sizeof(strings))
	{
		log("device: no descriptors: %s", std::strerror(errno));
		deviceDown();
		return false;
	}
	device.in = open((std::string(Mount) + "/ep1").c_str(), O_RDWR | O_NONBLOCK);
	device.out = open((std::string(Mount) + "/ep2").c_str(), O_RDWR | O_NONBLOCK);
	if (device.in < 0 || device.out < 0)
	{
		log("device: no endpoints: %s", std::strerror(errno));
		deviceDown();
		return false;
	}
	// g1 again with the function in it; adbd's comes back by itself
	writeText(GadgetUdc, "\n");
	if (symlink(Function, Linked) != 0 && errno != EEXIST)
	{
		log("device: cannot add the function to g1: %s", std::strerror(errno));
		deviceDown();
		return false;
	}
	sleepMs(300);
	rebind();
	// g1 not bound with the function in it: no USB on this port at all, ADB gone with it. The
	// function out again then, and g1 as it was (2026-10-31: a function g1 could not take kept a
	// Brick off USB until it was rebooted).
	if (!bound())
	{
		log("device: g1 cannot take the function: taking it out again");
		deviceDown();
		return false;
	}
	fcntl(device.ep0, F_SETFL, O_NONBLOCK);
	serving = true;
	server = new std::thread([] { follow(devicePipe, serving); });
	if (!quiet)
		log("device: the function is up on %s", udc.c_str());
	return true;
}

// ep0's events: a host taking the function, or leaving it
void deviceEvents()
{
	usb_functionfs_event events[4];
	ssize_t n;
	while ((n = read(device.ep0, events, sizeof(events))) > 0)
		for (int i = 0; i < n / (ssize_t)sizeof(events[0]); i++)
		{
			const u8 type = events[i].type;
			if (type == FUNCTIONFS_ENABLE && !device.enabled)
			{
				device.enabled = true;
				log("device: a host enabled the function");
			}
			else if ((type == FUNCTIONFS_DISABLE || type == FUNCTIONFS_UNBIND || type == FUNCTIONFS_SUSPEND) && device.enabled)
			{
				device.enabled = false;
				log("device: the host is gone");
				followerLost("Lost the other Brick.");
			}
		}
}

// ---- the host's side: the other Brick's function on one of this Brick's host ports

int bulk(unsigned endpoint, void *data, u32 size, u32 timeoutMs)
{
	usbdevfs_bulktransfer transfer {};
	transfer.ep = endpoint;
	transfer.len = size;
	transfer.timeout = timeoutMs;
	transfer.data = data;
	return ioctl(host.fd, USBDEVFS_BULK, &transfer);
}

// an interface's bulk endpoints
void endpoints(const std::string& intf, unsigned& in, unsigned& out)
{
	in = out = 0;
	if (DIR *eps = opendir(intf.c_str()))
	{
		while (const dirent *ep = readdir(eps))
		{
			if (std::strncmp(ep->d_name, "ep_", 3) != 0)
				continue;
			const std::string epdir = intf + "/" + ep->d_name;
			if (readNumber(epdir + "/bmAttributes", 16) != USB_ENDPOINT_XFER_BULK)
				continue;
			const unsigned address = readNumber(epdir + "/bEndpointAddress", 16);
			(address & USB_DIR_IN ? in : out) = address;
		}
		closedir(eps);
	}
}

bool findPeer()
{
	DIR *dir = opendir("/sys/bus/usb/devices");
	if (dir == nullptr)
		return false;
	bool found = false;
	while (!found)
	{
		const dirent *e = readdir(dir);
		if (e == nullptr)
			break;
		const std::string name = e->d_name;
		const size_t colon = name.find(':');
		if (colon == std::string::npos)
			continue;
		const std::string intf = "/sys/bus/usb/devices/" + name;
		if (readNumber(intf + "/bInterfaceClass", 16) != Class || readNumber(intf + "/bInterfaceSubClass", 16) != SubClass
				|| readNumber(intf + "/bInterfaceProtocol", 16) != Protocol)
			continue;
		const std::string dev = "/sys/bus/usb/devices/" + name.substr(0, colon);
		char node[64];
		std::snprintf(node, sizeof(node), "/dev/bus/usb/%03u/%03u", readNumber(dev + "/busnum", 10), readNumber(dev + "/devnum", 10));
		host.node = node;
		host.intf = (int)readNumber(intf + "/bInterfaceNumber", 16);
		// the top port's controllers are at 0x5200000 (ehci1, ohci1), the bottom one's at 0x5101000
		char real[PATH_MAX];
		host.port = realpath(dev.c_str(), real) != nullptr && std::strstr(real, "5200000.") != nullptr ? "top" : "bottom";
		endpoints(intf, host.in, host.out);
		found = host.in != 0 && host.out != 0;
	}
	closedir(dir);
	return found;
}

bool hostOpen()
{
	host.fd = open(host.node.c_str(), O_RDWR);
	if (host.fd < 0 || ioctl(host.fd, USBDEVFS_CLAIMINTERFACE, &host.intf) != 0)
	{
		log("host: cannot open %s: %s", host.node.c_str(), std::strerror(errno));
		if (host.fd >= 0)
			close(host.fd);
		host.fd = -1;
		return false;
	}
	hostPipe.where = "the " + host.port + " port";
	return true;
}

// ---- the bottom port's role

// "usb_host", "usb_device" or "null"; reading the driver's file for it is what switches it
void setRole(const std::string& want)
{
	if (readText(OtgRole) != want)
	{
		if (want == "usb_host")
			if (FILE *f = fopen(HostMark, "w"))
				fclose(f);
		const std::string said = readText(want == "usb_host" ? OtgHost : want == "usb_device" ? OtgDevice : OtgNull);
		// the driver turns the 5 V on as it makes the port a host (written 0 before, it still does: seen
		// 2026-11-01): off again at once, on a few milliseconds
		if (want == "usb_host")
		{
			writeText(Axp5v, "0");
			log("bottom port: a host, its 5 V out switched off: %s", bottomPower().c_str());
		}
		for (int i = 0; i < 20 && readText(OtgRole) != want; i++)
			sleepMs(100);
		log("bottom port: %s (%s), its 5 V out %s", readText(OtgRole).c_str(), said.c_str(), bottomPower().c_str());
	}
	if (want != "usb_host")
		unlink(HostMark);
	if (want == "usb_device")
	{
		// ADB on it again
		sleepMs(300);
		rebind();
	}
}

// A process of its own that outlives Deadcast, killed or crashed with the link up: then it takes the
// function out of g1 (its ep0 closed, g1 cannot bind, and ADB is gone with it), makes the bottom port
// a device again if Deadcast made it a host, and switches the top port's 5 V on again if Deadcast
// switched it off. launch.sh does the same, but only for a game it launched.
pid_t guard = -1;

constexpr const char *GuardScript =
	"while kill -0 \"$1\" 2>/dev/null; do sleep 1; done\n"
	"G=/sys/kernel/config/usb_gadget/g1\n"
	"if [ -e $G/configs/c.1/ffs.deadcast ]; then\n"
	"	echo > $G/UDC\n"
	"	rm -f $G/configs/c.1/ffs.deadcast\n"
	"	umount /dev/usb-ffs/deadcast\n"
	"	rmdir /dev/usb-ffs/deadcast $G/functions/ffs.deadcast\n"
	"	sleep 0.3\n"
	"	[ -n \"$(cat $G/UDC)\" ] || ls /sys/class/udc > $G/UDC\n"
	"fi\n"
	"if [ -f /tmp/deadcast-link-host ]; then\n"
	"	cat /sys/devices/platform/soc/usbc0/usb_device > /dev/null\n"
	"	rm -f /tmp/deadcast-link-host\n"
	"fi\n"
	"if [ -f /tmp/deadcast-link-top-off ]; then\n"
	"	D=/sys/kernel/debug/sunxi_pinctrl\n"
	"	echo r_pio > $D/dev_name; echo PL8 > $D/sunxi_pin; echo 'PL8 1' > $D/data\n"
	"	rm -f /tmp/deadcast-link-top-off\n"
	"fi\n";

void startGuard()
{
	const std::string self = std::to_string(getpid());
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	// its own session: no signal meant for Deadcast's reaches it
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
	const char *argv[] { "sh", "-c", GuardScript, "deadcast-link-guard", self.c_str(), nullptr };
	if (posix_spawn(&guard, "/bin/sh", nullptr, &attr, (char *const *)argv, environ) != 0)
	{
		guard = -1;
		log("no guard process: %s", std::strerror(errno));
	}
	posix_spawnattr_destroy(&attr);
}

void stopGuard()
{
	if (guard <= 0)
		return;
	kill(guard, SIGKILL);
	waitpid(guard, nullptr, 0);
	guard = -1;
}

struct Usb : Transport
{
	std::string startRole;
	bool hosting = false;
	bool wantHosting = false;		// the bottom port a host: for now (switching roles), or linked as one
	bool top = false;
	std::mt19937 rng;
	std::uniform_real_distribution<double> spell { 2.0, 4.0 };
	double switchAt = 0;
	bool deviceIsUp = false;
	double deviceUpAt = 0;
	int rebinds = 0;				// g1 bound again for a host that did not take the function
	double nextTry = 0, nextLook = 0, nextArm = 0;
	bool arming = false;			// setting the role again (logged once a wait)
	std::string bottom;
	// Each Brick on its own battery: the top port's 5 V off while linked to a Brick there, that Brick's
	// bottom port pinned a device (the driver's role for a powered port goes with the power)
	bool pinned = false;
	bool topOff = false;
	double topOffAt = 0;
	bool keepTopOn = false;			// a link lost at once without the power: on from then
	double holdUntil = 0;			// no switching the bottom port's role until then

	void begin() override
	{
		startGuard();
		devicePipe.where = "the bottom port";
		if (DIR *dir = opendir("/sys/class/udc"))
		{
			while (const dirent *e = readdir(dir))
				if (e->d_name[0] != '.')
					udc = e->d_name;
			closedir(dir);
		}
		startRole = readText(OtgRole);
		log("start: Brick %s, USB, bottom port %s", ownSerial.c_str(), startRole.c_str());
		// the bottom port a host still, from a Deadcast killed while it was
		if (access(HostMark, F_OK) == 0)
			setRole("usb_device");
		// With nothing on it, or another Brick's bottom port (the driver does not take that for a host), the
		// port has no role and no device behind it: a device then, for the other Brick to find. It stays one
		// after the link (end()).
		if (readText(OtgRole) == "null")
			setRole("usb_device");
		// Power coming in (a host: another Brick's top port, a computer; or a charger): the driver made
		// the port a device itself, and ends that when the power goes. Pinned a device (its file read) for
		// a link to outlast the power, which the other Brick switches off once linked (linked()). It
		// stays pinned after.
		else if (readText(PowerIn) == "1" && readText(OtgRole) == "usb_device")
		{
			readText(OtgDevice);
			pinned = true;
			log("bottom port: pinned a device, %s", bottomState().c_str());
			sleepMs(300);
			rebind();
		}
		hosting = wantHosting = false;
		keepTopOn = false;
		top = topPortInUse();
		// the two Bricks' spells apart: seeded with this one's serial as well as the time
		rng.seed((unsigned)std::hash<std::string>()(ownSerial) ^ (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
		switchAt = now() + spell(rng) / 2;
		deviceIsUp = deviceUp();
		deviceUpAt = now();
		rebinds = 0;
		nextTry = nextLook = 0;
		nextArm = now() + 4;
		arming = false;
		bottom.clear();
	}

	void poll() override
	{
		// what each side saw of the other, in the log: the bottom port's state whenever it changes
		if (now() >= nextLook)
		{
			nextLook = now() + 1;
			const std::string state = bottomState();
			if (state != bottom)
				log("bottom port now: %s", state.c_str());
			bottom = state;
			top = topPortInUse();
			update([this](Status& s) { s.topPortInUse = top; });
		}
		// Bottom to bottom, which of the two hosts: each Brick switches its bottom port between host and
		// device, for 2 to 4 s at random each time, until the two are opposite long enough to link; then
		// both stay as they are (as a dual-role USB-C port would). Only with nothing on the top port (a
		// Brick there: this one hosts on it already) and no power coming into the bottom port (a computer,
		// a charger, the other Brick's top port: this one is a device). Never hosting over a powered
		// bottom port: two 5 V supplies on one wire (a charger plugged into a hosting port shows as power
		// once the PMIC takes it, and the loop ends hosting within 50 ms).
		const bool powered = readText(PowerIn) == "1";
		const bool linked = host.fd >= 0 || device.enabled;
		// pinned: the bottom port on another Brick's top port, whose 5 V is off while linked and comes
		// back as the link ends; never a host meanwhile (two 5 V supplies on one wire)
		if (pinned && (powered || linked))
			holdUntil = now() + 10;
		const bool switching = !linked && !top && !powered && now() >= holdUntil;
		if (!switching)
		{
			// hosting kept only for a link over the bottom port itself
			if (wantHosting && (powered || host.fd < 0 || host.port != "bottom"))
				wantHosting = false;
			switchAt = now() + spell(rng);
		}
		else if (now() >= switchAt)
		{
			wantHosting = !wantHosting;
			switchAt = now() + spell(rng);
		}
		if (wantHosting != hosting)
		{
			hosting = wantHosting;
			if (hosting)
			{
				if (deviceIsUp)
					deviceDown();
				deviceIsUp = false;
				setRole("usb_host");
			}
			else
			{
				if (host.fd >= 0 && host.port == "bottom")
					leaderLost("Stopped hosting.");
				setRole("usb_device");
				deviceIsUp = deviceUp();
				deviceUpAt = now();
			}
			nextArm = now() + (hosting ? 8 : 1.5);
		}
		if (deviceIsUp)
			deviceEvents();
		// Power coming in: a host is there (a computer, the other Brick's top port), and a host takes every
		// function of g1 within a second. Not this one within 3 s: g1 was bound without it (seen 2026-10-31,
		// the other Brick saw ADB's interface alone; adbd binds g1 itself too), so bound again, 3 times at most.
		if (deviceIsUp && device.enabled)
			rebinds = 0;
		else if (deviceIsUp && !hosting && powered && rebinds < 3 && now() - deviceUpAt > 3)
		{
			rebinds++;
			log("device: the host did not take the function: g1 bound again (%d)", rebinds);
			writeText(GadgetUdc, "\n");
			sleepMs(300);
			rebind();
			deviceUpAt = now();
		}
		// The cable can go in at any time, and nothing tells a Brick that another Brick's bottom port
		// came (its PMIC reports no power from one). Seen 2026-10-31: roles set before the cable went in
		// never linked; set again with the cable in and the other Brick hosting, they did at once. So
		// until linked, each side sets its role again now and then; reading the driver's file redoes
		// the role even when it is the role already. Never on a powered port (a computer, a charger,
		// a Brick's top port): the driver sees those itself, and doing it again drops their link.
		if (now() >= nextArm)
		{
			if (!hosting && deviceIsUp && !device.enabled && switching)
			{
				nextArm = now() + 1.5;
				if (!arming)
					log("device: setting the role again every 1.5 s until a host comes");
				arming = true;
				deviceDown(true);
				readText(OtgDevice);
				deviceIsUp = deviceUp(true);
				deviceUpAt = now();
			}
			else if (hosting && host.fd < 0)
			{
				nextArm = now() + 8;
				if (!arming)
					log("host: setting the role again every 8 s until a device comes");
				arming = true;
				readText(OtgHost);
			}
			else
			{
				nextArm = now() + 4;
				arming = false;
			}
		}
	}

	Pipe *lead() override
	{
		if (host.fd >= 0 || now() < nextTry || !findPeer())
			return nullptr;
		if (!hostOpen())
		{
			nextTry = now() + 2;
			return nullptr;
		}
		return &hostPipe;
	}

	// linked to a Brick on the top port: its 5 V off, that Brick on its own battery (its bottom port
	// pinned: begin())
	void linked() override
	{
		if (host.port == "top" && !keepTopOn && topPower(false))
		{
			topOff = true;
			topOffAt = now();
		}
	}

	void leadLost() override
	{
		if (host.fd >= 0)
		{
			close(host.fd);
			host.fd = -1;
		}
		if (topOff)
		{
			topPower(true);
			topOff = false;
			// gone with the power at once (a Deadcast on it that does not pin its port): kept on
			if (now() - topOffAt < 5 && !stopping)
			{
				keepTopOn = true;
				log("top port: the link went with its 5 V: kept on from now");
			}
		}
	}

	void end() override
	{
		if (deviceIsUp)
			deviceDown();
		deviceIsUp = false;
		if (hosting)
			setRole("usb_device");
		hosting = false;
		// The port stays a device, never null again: choosing any role stops the driver's own watch of
		// the port, so given back as null it no longer finds a computer plugged in later, and ADB and the
		// PC are gone until a reboot (seen 2026-10-31 and 2026-11-01). A device, a computer finds it.
		pinned = false;
		holdUntil = 0;
		// the ports are as they were: nothing left for it to do
		stopGuard();
	}
};

}	// namespace

Transport *usbTransport()
{
	static Usb usb;
	return &usb;
}

}
