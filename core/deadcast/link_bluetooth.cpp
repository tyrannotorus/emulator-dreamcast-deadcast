// The link over Bluetooth (link_core.h). Bluetooth on as NextUI turns it on (its bt_init.sh; off again
// afterwards if it was off), each Brick visible and, now and then for a few seconds at random, looking
// for the others nearby (an inquiry). Of two Bricks that found each other, the one with the lower address
// connects (an L2CAP packet channel on Psm) and leads; the other takes the connection and follows. No
// pairing: the channel asks for none. Kernel sockets and ioctls only (the headers are the toolchain's):
// nothing more for Deadcast to load.
#include "link_core.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/l2cap.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <vector>

extern char **environ;

namespace deadcast::link::core
{
namespace
{

// NextUI's Bluetooth: its script (rfkill.elf, which it calls, is in NextUI's own bin)
constexpr const char *BtInit = "/etc/bluetooth/bt_init.sh";
constexpr const char *BtBin = "/mnt/SDCARD/.system/tg5040/bin";
constexpr const char *Adapter = "/sys/class/bluetooth/hci0";
constexpr int StartSeconds = 25;
// the link's channel: a dynamic PSM (odd, the upper byte's lowest bit clear)
constexpr u16 Psm = 0x10dd;
constexpr u16 Mtu = 4096;
constexpr int ConnectMs = 8000;

std::string text(const bdaddr_t& a)
{
	char s[18];
	std::snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", a.b[5], a.b[4], a.b[3], a.b[2], a.b[1], a.b[0]);
	return s;
}

u64 number(const bdaddr_t& a)
{
	u64 n = 0;
	for (int i = 5; i >= 0; i--)
		n = n << 8 | a.b[i];
	return n;
}

// one L2CAP packet channel, both ways
struct ChannelPipe : Pipe
{
	int fd = -1;

	int send(const void *data, u32 size, u32 timeoutMs) override
	{
		pollfd p { fd, POLLOUT, 0 };
		if (::poll(&p, 1, (int)timeoutMs) <= 0)
			return -1;
		return (int)::send(fd, data, size, MSG_NOSIGNAL);
	}
	int receive(void *data, u32 size, u32 timeoutMs) override
	{
		pollfd p { fd, POLLIN, 0 };
		const int ready = ::poll(&p, 1, (int)timeoutMs);
		if (ready == 0)
			return 0;
		if (ready < 0)
			return errno == EINTR ? 0 : -1;
		const ssize_t n = recv(fd, data, size, MSG_DONTWAIT);
		// 0: the other side closed it
		if (n == 0)
			return -1;
		if (n < 0)
			return errno == EAGAIN || errno == EINTR ? 0 : -1;
		return (int)n;
	}
	u32 chunk() const override { return Mtu; }
	u32 testEchoes() const override { return 100; }
	u32 testBytes() const override { return 256 << 10; }
	void close()
	{
		if (fd >= 0)
		{
			shutdown(fd, SHUT_RDWR);
			::close(fd);
		}
		fd = -1;
	}
};

// an L2CAP packet socket: no pairing asked for, packets up to Mtu
int channelSocket()
{
	const int s = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
	if (s < 0)
		return -1;
	bt_security security {};
	security.level = BT_SECURITY_LOW;
	setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &security, sizeof(security));
	l2cap_options options {};
	socklen_t size = sizeof(options);
	if (getsockopt(s, SOL_L2CAP, L2CAP_OPTIONS, &options, &size) == 0)
	{
		options.imtu = options.omtu = Mtu;
		setsockopt(s, SOL_L2CAP, L2CAP_OPTIONS, &options, sizeof(options));
	}
	return s;
}

struct Bluetooth : Transport
{
	pid_t script = -1;				// bt_init.sh start, while it runs
	bool startedIt = false;			// Bluetooth was off: off again at the end
	double startedAt = 0;
	bool ready = false;
	int hci = -1;					// the adapter's control socket
	int listener = -1;
	bdaddr_t own {};
	std::mt19937 rng;
	double nextLook = 0;
	std::map<u64, double> triedAt;	// a connection to it failed then
	Activity lastActivity = Activity::None;
	ChannelPipe leadPipe;
	ChannelPipe followPipe;
	std::thread *follower = nullptr;
	std::atomic<bool> followRunning { false };
	std::atomic<bool> followDone { false };

	void activity(Activity what)
	{
		if (what == lastActivity)
			return;
		lastActivity = what;
		update([what](Status& s) { s.activity = what; });
	}

	void fail(const std::string& why)
	{
		log("bluetooth: %s", why.c_str());
		update([&why](Status& s) {
			s.state = State::Failed;
			s.activity = Activity::None;
			s.error = why;
		});
	}

	// NextUI's script, start or stop; stop waited for
	pid_t runScript(const char *what, bool wait)
	{
		std::vector<std::string> env;
		for (char **e = environ; *e != nullptr; e++)
			if (std::strncmp(*e, "PATH=", 5) != 0)
				env.push_back(*e);
		const char *path = std::getenv("PATH");
		env.push_back(std::string("PATH=") + (path != nullptr ? path : "/usr/bin:/bin:/usr/sbin:/sbin") + ":" + BtBin);
		std::vector<char *> envp;
		for (std::string& e : env)
			envp.push_back(&e[0]);
		envp.push_back(nullptr);
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
		const char *argv[] { "sh", BtInit, what, nullptr };
		pid_t pid = -1;
		if (posix_spawn(&pid, "/bin/sh", &actions, nullptr, (char *const *)argv, envp.data()) != 0)
			pid = -1;
		posix_spawn_file_actions_destroy(&actions);
		if (pid > 0 && wait)
		{
			waitpid(pid, nullptr, 0);
			pid = -1;
		}
		return pid;
	}

	void begin() override
	{
		ready = false;
		startedIt = false;
		script = -1;
		hci = listener = -1;
		triedAt.clear();
		lastActivity = Activity::None;
		followDone = false;
		rng.seed((unsigned)std::hash<std::string>()(ownSerial) ^ (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
		log("start: Brick %s, Bluetooth", ownSerial.c_str());
		if (access(Adapter, F_OK) != 0)
		{
			if (access(BtInit, X_OK) != 0)
				return fail("This Brick has no Bluetooth.");
			activity(Activity::TurningOn);
			script = runScript("start", false);
			startedIt = true;
		}
		startedAt = now();
	}

	// the adapter up: its address, visible, and the channel listened for
	bool setUp()
	{
		hci = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
		if (hci < 0)
		{
			fail("Bluetooth did not start. Please try again.");
			return false;
		}
		hci_dev_info info {};
		info.dev_id = 0;
		if (ioctl(hci, HCIGETDEVINFO, &info) != 0)
		{
			fail("Bluetooth did not start. Please try again.");
			return false;
		}
		if (!(info.flags & (1u << HCI_UP)))
		{
			ioctl(hci, HCIDEVUP, 0);
			ioctl(hci, HCIGETDEVINFO, &info);
		}
		own = info.bdaddr;
		visible();
		listener = channelSocket();
		sockaddr_l2 self {};
		self.l2_family = AF_BLUETOOTH;
		// (any adapter: l2_bdaddr all zero)
		self.l2_psm = htobs(Psm);
		if (listener < 0 || bind(listener, (sockaddr *)&self, sizeof(self)) != 0 || listen(listener, 1) != 0)
		{
			log("bluetooth: cannot listen on PSM 0x%x: %s", Psm, std::strerror(errno));
			fail("Bluetooth did not start. Please try again.");
			return false;
		}
		fcntl(listener, F_SETFL, O_NONBLOCK);
		log("bluetooth: up as %s, listening on PSM 0x%x", text(own).c_str(), Psm);
		nextLook = now() + std::uniform_real_distribution<double>(0.5, 3.0)(rng);
		return true;
	}

	// visible to the other Brick's inquiry, and connectable (again before every look: bluetoothd, which
	// NextUI's script starts, ends being visible after a while)
	void visible()
	{
		hci_dev_req scan { 0, SCAN_PAGE | SCAN_INQUIRY };
		if (ioctl(hci, HCISETSCAN, &scan) != 0)
			log("bluetooth: cannot make this Brick visible: %s", std::strerror(errno));
	}

	// the other Brick connected: it leads, this one follows on a thread of its own
	void takeConnection()
	{
		sockaddr_l2 from {};
		socklen_t size = sizeof(from);
		const int fd = accept4(listener, (sockaddr *)&from, &size, SOCK_CLOEXEC);
		if (fd < 0)
			return;
		if (follower != nullptr || leading())
		{
			// linked already: one other Brick at a time
			::close(fd);
			return;
		}
		log("bluetooth: %s connected", text(from.l2_bdaddr).c_str());
		followPipe.fd = fd;
		followPipe.where = "Bluetooth";
		followRunning = true;
		followDone = false;
		follower = new std::thread([this] {
			follow(followPipe, followRunning);
			followDone = true;
		});
	}

	void stopFollowing(const std::string& why)
	{
		if (follower == nullptr)
			return;
		followRunning = false;
		followPipe.close();
		follower->join();
		delete follower;
		follower = nullptr;
		followerLost(why);
		log("bluetooth: %s", why.c_str());
	}

	// Bricks nearby (an inquiry: a few seconds, this Brick not visible meanwhile): their addresses
	std::vector<bdaddr_t> inquire()
	{
		struct
		{
			hci_inquiry_req request;
			inquiry_info found[16];
		} buffer {};
		buffer.request.dev_id = 0;
		buffer.request.flags = IREQ_CACHE_FLUSH;
		// the general inquiry access code
		buffer.request.lap[0] = 0x33;
		buffer.request.lap[1] = 0x8b;
		buffer.request.lap[2] = 0x9e;
		// in 1.28 s units
		buffer.request.length = (u8)std::uniform_int_distribution<int>(2, 4)(rng);
		buffer.request.num_rsp = 16;
		std::vector<bdaddr_t> found;
		if (ioctl(hci, HCIINQUIRY, &buffer) != 0)
		{
			log("bluetooth: inquiry: %s", std::strerror(errno));
			return found;
		}
		for (int i = 0; i < buffer.request.num_rsp && i < 16; i++)
			found.push_back(buffer.found[i].bdaddr);
		return found;
	}

	// a channel to it: false if it is no Brick on the link's page (or out of reach)
	bool connectTo(const bdaddr_t& to)
	{
		const int fd = channelSocket();
		if (fd < 0)
			return false;
		sockaddr_l2 peer {};
		peer.l2_family = AF_BLUETOOTH;
		peer.l2_psm = htobs(Psm);
		bacpy(&peer.l2_bdaddr, &to);
		fcntl(fd, F_SETFL, O_NONBLOCK);
		int error = 0;
		if (connect(fd, (sockaddr *)&peer, sizeof(peer)) != 0)
		{
			error = errno;
			if (error == EINPROGRESS)
			{
				pollfd p { fd, POLLOUT, 0 };
				socklen_t size = sizeof(error);
				if (::poll(&p, 1, ConnectMs) <= 0)
					error = ETIMEDOUT;
				else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
					error = errno;
			}
		}
		if (error != 0)
		{
			log("bluetooth: no link with %s: %s", text(to).c_str(), std::strerror(error));
			::close(fd);
			return false;
		}
		fcntl(fd, F_SETFL, 0);
		leadPipe.fd = fd;
		leadPipe.where = "Bluetooth";
		log("bluetooth: connected to %s", text(to).c_str());
		return true;
	}

	void poll() override
	{
		if (!ready)
		{
			if (script > 0 && waitpid(script, nullptr, WNOHANG) == script)
				script = -1;
			if (access(Adapter, F_OK) == 0 && script <= 0)
			{
				ready = setUp();
				if (!ready)
					startedAt = -1;
			}
			else if (startedAt > 0 && now() - startedAt > StartSeconds)
			{
				startedAt = -1;
				fail("Bluetooth did not start. Please try again.");
			}
			return;
		}
		takeConnection();
		// the leader gone (it left, or out of reach): this Brick looks again
		if (followDone)
		{
			followDone = false;
			stopFollowing("Lost the other Brick.");
		}
		if (follower != nullptr || leading())
		{
			activity(Activity::None);
			return;
		}
		activity(Activity::Looking);
		if (now() < nextLook)
			return;
		// Looking, a few seconds; then visible again for a few (the other Brick looks too). Of two Bricks
		// that see each other, the lower address connects: they never both do. Seen one with a lower
		// address (the other Brick, maybe): visible longer, for it to find this one.
		visible();
		bool waitFor = false;
		for (const bdaddr_t& found : inquire())
		{
			const u64 them = number(found);
			if (them < number(own))
				waitFor = true;
			if (them == number(own) || them < number(own) || follower != nullptr)
				continue;
			const auto tried = triedAt.find(them);
			if (tried != triedAt.end() && now() - tried->second < 10)
				continue;
			log("bluetooth: found %s, connecting", text(found).c_str());
			activity(Activity::Connecting);
			if (connectTo(found))
				break;
			triedAt[them] = now();
			takeConnection();
		}
		nextLook = now() + std::uniform_real_distribution<double>(2.0, 5.0)(rng) + (waitFor ? 6.0 : 0.0);
	}

	Pipe *lead() override
	{
		if (leadPipe.fd < 0)
			return nullptr;
		return &leadPipe;
	}

	void leadLost() override
	{
		leadPipe.close();
	}

	void end() override
	{
		stopFollowing("stopped");
		leadPipe.close();
		if (listener >= 0)
			::close(listener);
		listener = -1;
		if (hci >= 0)
			::close(hci);
		hci = -1;
		if (script > 0)
			waitpid(script, nullptr, 0);
		script = -1;
		// as it was: off again if this turned it on
		if (startedIt)
		{
			log("bluetooth: turning it off again");
			runScript("stop", true);
		}
		update([](Status& s) { s.activity = Activity::None; });
	}
};

}	// namespace

Transport *bluetoothTransport()
{
	static Bluetooth bluetooth;
	return &bluetooth;
}

}
