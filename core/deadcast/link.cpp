// The link's core (link_core.h), the same over every transport: the leader's commands (hello, pings,
// the test, IP up and down), the follower's answers, IP over the link (dclink0) and the status the menu
// shows.
#include "link_core.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace deadcast::link
{
namespace core
{

std::atomic<bool> stopping { false };
std::string ownSerial;

namespace
{

constexpr u32 Magic = 0x4b4e4c44;		// "DLNK"
enum Op : u32 { Hello = 1, Ping, Sink, Source, Echo, Result, IpUp, IpDown, VmuSwap };
constexpr u32 Block = 65536;			// the largest packet any pipe takes, and a test pattern's block
constexpr u32 EchoSize = 64;
constexpr u32 VmuMax = 128 << 10;		// a VMU's flash
constexpr u32 FirstHash = 2166136261u;

// both ways: a command, then its reply (none for Echo)
struct Message
{
	u32 magic = Magic;
	u32 op = 0;
	u32 a = 0, b = 0;		// Sink, Source: bytes, 1 = hashed; Echo: count, size; Result: the text's size,
							// the input delay; VmuSwap (and its reply): the VMU's size (it follows)
	u64 bytes = 0, ns = 0;	// a reply's: what it moved, in how long
	u32 hash = 0;
	u32 flags = 0;			// Ping's reply: 1 = the follower wants a test
	char text[64] {};		// Hello (and its reply): the sender's serial number, a line break, its game
};
static_assert(sizeof(Message) == 104, "Message is on the wire");

std::mutex statusMutex;
Status current;
std::mutex logMutex;
// never destroyed: a joinable std::thread at exit would abort
std::thread *worker;
Transport *transport;
Pipe *led;							// the pipe this Brick leads over, while it does
std::atomic<bool> wantTest { false };	// the leader runs it; the follower asks the leader at its next ping
int pingsMissed;					// in a row, by the leader
std::mutex gameMutex;
std::string ownGame, ownVmuFile;	// setGame()'s
std::vector<u8> peerVmuData;		// the other Brick's player's VMU, as swapped

// block k of a test stream: xorshift seeded by k, so either side makes it and checks it
void pattern(u32 *words, u32 k)
{
	u32 x = k * 2654435761u + 1;
	for (u32 i = 0; i < Block / 4; i++)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		words[i] = x;
	}
}

u32 hashWords(u32 hash, const void *data, size_t size)
{
	const u32 *words = (const u32 *)data;
	for (size_t i = 0; i < size / 4; i++)
		hash = (hash ^ words[i]) * 16777619u;
	return hash;
}

// ---- IP over the link: a TUN interface on each Brick (dclink0: LeaderIp on the leader, FollowerIp on
// the follower). Its packets share the pipe with the link's commands (USB: the Brick's UDC has four bulk
// endpoints, and adbd's function takes two): an IP frame is IpMagic, the packet's length and the
// packet, and a byte more should that end on a whole 512-byte USB packet (then nothing would end the
// transfer, and the next would run into it); a command is a Message. The test streams raw data, so IP
// is up only after it (IpUp) and down for another one (IpDown).

constexpr const char *TunName = "dclink0";
constexpr const char *LeaderIp = "10.73.0.1";
constexpr const char *FollowerIp = "10.73.0.2";
constexpr int TunMtu = 1500;
constexpr u32 FrameMax = 2048;
constexpr char IpMagic[4] { 'D', 'L', 'I', 'P' };

// an IP frame to the other Brick: its size, or < 0
using SendFrame = std::function<int(const u8 *, u32)>;

struct Tunnel
{
	int fd = -1;
	std::atomic<bool> running { false };
	std::thread *out = nullptr;		// dclink0 -> the other Brick
	std::thread *in = nullptr;		// on the leader: the one reader of the pipe (IP frames, replies)
} tunnel;

bool setAddress(int sock, unsigned long request, const char *ip)
{
	ifreq ifr {};
	std::strncpy(ifr.ifr_name, TunName, IFNAMSIZ - 1);
	sockaddr_in *address = (sockaddr_in *)&ifr.ifr_addr;
	address->sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address->sin_addr);
	return ioctl(sock, request, &ifr) == 0;
}

// dclink0, up, point to point between the two addresses
int tunOpen(const char *self, const char *peer)
{
	const int fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
	{
		log("ip: no /dev/net/tun: %s", std::strerror(errno));
		return -1;
	}
	ifreq ifr {};
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	std::strncpy(ifr.ifr_name, TunName, IFNAMSIZ - 1);
	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	bool up = ioctl(fd, TUNSETIFF, &ifr) == 0 && sock >= 0 && setAddress(sock, SIOCSIFADDR, self)
			&& setAddress(sock, SIOCSIFDSTADDR, peer) && setAddress(sock, SIOCSIFNETMASK, "255.255.255.255");
	if (up)
	{
		ifr.ifr_mtu = TunMtu;
		up = ioctl(sock, SIOCSIFMTU, &ifr) == 0 && ioctl(sock, SIOCGIFFLAGS, &ifr) == 0;
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		up = up && ioctl(sock, SIOCSIFFLAGS, &ifr) == 0;
	}
	const int error = errno;
	if (sock >= 0)
		close(sock);
	if (!up)
	{
		log("ip: cannot set %s up: %s", TunName, std::strerror(error));
		close(fd);
		return -1;
	}
	return fd;
}

// the IP packet in a packet from the other Brick, to dclink0; false if the packet is not an IP frame
bool tunnelTake(const u8 *data, int n)
{
	if (n < 8 || std::memcmp(data, IpMagic, 4) != 0)
		return false;
	u32 length;
	std::memcpy(&length, data + 4, 4);
	if (tunnel.fd >= 0 && length + 8 <= (u32)n && write(tunnel.fd, data + 8, length) < 0)
		log("ip: a packet not taken: %s", std::strerror(errno));
	return true;
}

void tunnelStop()
{
	if (tunnel.fd < 0)
		return;
	tunnel.running = false;
	for (std::thread **t : { &tunnel.out, &tunnel.in })
		if (*t != nullptr)
		{
			(*t)->join();
			delete *t;
			*t = nullptr;
		}
	close(tunnel.fd);
	tunnel.fd = -1;
	log("ip: %s down", TunName);
	update([](Status& s) {
		s.address.clear();
		s.peerAddress.clear();
	});
}

// dclink0 up, its packets out through send; reader (the leader's): what takes the pipe's packets meanwhile
void tunnelStart(bool leader, SendFrame send, std::function<void()> reader = nullptr)
{
	if (tunnel.fd >= 0)
		return;
	const char *self = leader ? LeaderIp : FollowerIp;
	const char *peer = leader ? FollowerIp : LeaderIp;
	tunnel.fd = tunOpen(self, peer);
	if (tunnel.fd < 0)
		return;
	tunnel.running = true;
	tunnel.out = new std::thread([send] {
		std::vector<u8> frame(FrameMax);
		std::memcpy(frame.data(), IpMagic, 4);
		while (tunnel.running)
		{
			pollfd p { tunnel.fd, POLLIN, 0 };
			if (poll(&p, 1, 100) <= 0)
				continue;
			const ssize_t n = read(tunnel.fd, frame.data() + 8, TunMtu);
			if (n <= 0)
				continue;
			const u32 length = (u32)n;
			std::memcpy(frame.data() + 4, &length, 4);
			u32 size = length + 8;
			if (size % 512 == 0)
				frame[size++] = 0;
			// lost when the link is (its pings notice)
			send(frame.data(), size);
		}
	});
	if (reader)
		tunnel.in = new std::thread(reader);
	log("ip: %s up, %s to %s", TunName, self, peer);
	update([self, peer](Status& s) {
		s.address = self;
		s.peerAddress = peer;
	});
}

// ---- the leader's side

// with IP up the tunnel's reader takes everything from the pipe: the replies, for receive()
std::mutex replyMutex;
std::condition_variable replyReady;
std::deque<Message> replies;

bool send(Message& m)
{
	return led->send(&m, sizeof(m), 2000) == (int)sizeof(m);
}

bool receive(Message& m, u32 timeoutMs)
{
	if (tunnel.in != nullptr)
	{
		std::unique_lock<std::mutex> lock(replyMutex);
		if (!replyReady.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return !replies.empty(); }))
			return false;
		m = replies.front();
		replies.pop_front();
		return m.magic == Magic;
	}
	u8 buffer[FrameMax];
	const double until = now() + timeoutMs / 1000.0;
	while (now() < until)
	{
		const int n = led->receive(buffer, sizeof(buffer), timeoutMs);
		if (n < 0)
			return false;
		// (an IP frame still on its way from before IP went down is dropped)
		if (n == (int)sizeof(m))
		{
			std::memcpy(&m, buffer, sizeof(m));
			return m.magic == Magic;
		}
	}
	return false;
}

// the tunnel's reader on the leader: IP frames to dclink0, replies to receive()
void leaderReader()
{
	std::vector<u8> buffer(FrameMax);
	while (tunnel.running)
	{
		const int n = led->receive(buffer.data(), FrameMax, 200);
		if (n <= 0 || tunnelTake(buffer.data(), n) || n != (int)sizeof(Message))
			continue;
		std::lock_guard<std::mutex> lock(replyMutex);
		replies.emplace_back();
		std::memcpy(&replies.back(), buffer.data(), sizeof(Message));
		replyReady.notify_one();
	}
}

bool command(Message& m, Message& reply, u32 timeoutMs)
{
	return send(m) && receive(reply, timeoutMs) && reply.op == m.op;
}

// Hello's text both ways: this Brick's serial number and game
void sayHello(Message& m)
{
	std::lock_guard<std::mutex> lock(gameMutex);
	std::snprintf(m.text, sizeof(m.text), "%s\n%s", ownSerial.c_str(), ownGame.c_str());
}

// and the other Brick's, in Status: whether it plays the same game as this one
void heardHello(Message& m, Status& s)
{
	m.text[sizeof(m.text) - 1] = '\0';
	const std::string text = m.text;
	const size_t newline = text.find('\n');
	s.peer = text.substr(0, newline);
	const std::string game = newline == std::string::npos ? "" : text.substr(newline + 1);
	std::lock_guard<std::mutex> lock(gameMutex);
	s.sameGame = !game.empty() && game == ownGame;
}

// data that does not fit one packet, in packets of the pipe's size (sent after a Message saying how much)
bool sendRaw(Pipe& pipe, const std::vector<u8>& data)
{
	for (size_t sent = 0; sent < data.size(); )
	{
		const u32 size = (u32)std::min<size_t>(std::min(Block, pipe.chunk()), data.size() - sent);
		if (pipe.send(&data[sent], size, 2000) != (int)size)
			return false;
		sent += size;
	}
	return true;
}

bool receiveRaw(Pipe& pipe, std::vector<u8>& data)
{
	const double until = now() + 5;
	for (size_t got = 0; got < data.size(); )
	{
		const int n = pipe.receive(&data[got], (u32)std::min<size_t>(std::min(Block, pipe.chunk()), data.size() - got), 2000);
		if (n < 0 || (n == 0 && now() > until))
			return false;
		got += n;
	}
	return true;
}

// this Brick's player's VMU (none: empty), and the other Brick's as it came
std::vector<u8> ownVmu()
{
	std::string path;
	{
		std::lock_guard<std::mutex> lock(gameMutex);
		path = ownVmuFile;
	}
	std::vector<u8> data(VmuMax);
	FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
	if (f == nullptr || fread(data.data(), 1, data.size(), f) != data.size())
		data.clear();
	if (f != nullptr)
		fclose(f);
	return data;
}

void tookVmu(std::vector<u8>&& data)
{
	std::lock_guard<std::mutex> lock(gameMutex);
	peerVmuData = std::move(data);
}

void leaderClose(const std::string& why)
{
	tunnelStop();
	replies.clear();
	if (led != nullptr)
		transport->leadLost();
	led = nullptr;
	log("leader: %s", why.c_str());
	update([&why](Status& s) {
		if (s.role != Role::Host)
			return;
		s.state = State::Waiting;
		s.role = Role::None;
		s.port.clear();
		s.peer.clear();
		s.error = why;
	});
}

// the other Brick found: hello both ways (its serial number for this one's)
bool leaderHello(Pipe& pipe)
{
	led = &pipe;
	pingsMissed = 0;
	Message hello, reply;
	hello.op = Hello;
	sayHello(hello);
	if (!command(hello, reply, 2000))
	{
		led = nullptr;
		log("leader: no hello over %s", pipe.where.c_str());
		transport->leadLost();
		return false;
	}
	update([&reply, &pipe](Status& s) {
		s.state = State::Testing;
		s.role = Role::Host;
		s.port = pipe.where;
		heardHello(reply, s);
		s.result.clear();
		s.error.clear();
	});
	const Status s = status();
	log("leader: linked to %s over %s%s", s.peer.c_str(), pipe.where.c_str(), s.sameGame ? "" : ", another game");
	transport->linked();
	return true;
}

// The two players' VMUs, each Brick's to the other (deadcast/netplay.h): this one's, then the follower's
// back; before IP is up, which would take the pipe
bool swapVmus()
{
	const std::vector<u8> own = ownVmu();
	Message m, reply;
	m.op = VmuSwap;
	m.a = (u32)own.size();
	std::vector<u8> peer;
	if (send(m) && sendRaw(*led, own) && receive(reply, 5000) && reply.op == VmuSwap)
	{
		peer.resize(std::min(reply.a, VmuMax));
		if (receiveRaw(*led, peer))
		{
			log("leader: VMUs swapped, %zu bytes out, %zu in", own.size(), peer.size());
			tookVmu(std::move(peer));
			return true;
		}
	}
	leaderClose("The VMUs did not get across.");
	return false;
}

// IP over the link: after a test (whose raw streams can't share the pipe with IP frames), the
// follower's dclink0 up, then this one's
void ipUp()
{
	Message m, reply;
	m.op = IpUp;
	if (!command(m, reply, 2000))
		return log("leader: no IP from the other Brick");
	tunnelStart(true, [](const u8 *data, u32 size) { return led->send(data, size, 1000); }, leaderReader);
}

// before a test: this Brick's dclink0 down, then the follower's
void ipDown()
{
	if (tunnel.fd < 0)
		return;
	tunnelStop();
	replies.clear();
	Message m, reply;
	m.op = IpDown;
	command(m, reply, 2000);
}

// the pipe's test bytes one way, hashed on both sides: MB/s, or < 0
double stream(bool out, bool& intact)
{
	std::vector<u8> buffer(Block);
	const u32 total = led->testBytes();
	const u32 chunk = std::min(Block, led->chunk());
	Message m;
	m.op = out ? Sink : Source;
	m.a = total;
	m.b = 1;
	if (!send(m))
		return -1;
	u32 hash = FirstHash;
	const double start = now();
	u32 moved = 0;
	for (u32 k = 0; moved < total; k++)
	{
		if (stopping)
			return -1;
		const u32 size = std::min(chunk, total - moved);
		if (out)
			pattern((u32 *)buffer.data(), k);
		const int n = out ? led->send(buffer.data(), size, 2000) : led->receive(buffer.data(), chunk, 2000);
		if (n <= 0)
		{
			log("leader: %s stopped after %u bytes: %s", out ? "sending" : "receiving", moved, std::strerror(errno));
			return -1;
		}
		hash = hashWords(hash, buffer.data(), n);
		moved += n;
	}
	const double seconds = now() - start;
	Message reply;
	if (!receive(reply, 2000))
		return -1;
	intact = reply.hash == hash && reply.bytes == total;
	return total / seconds / 1e6;
}

// Frames of input delay for a session over this link: a frame's input must reach the other Brick within
// delay - 1 frames (the lockstep waits otherwise). 2 at least, even over the cable's ~0.1 ms each way: a
// frame of slack for each Brick's frame times to vary (with 1, each waited 5 to 10% of the time for the
// other, 2026-10-31); Bluetooth's tens of ms: more.
int inputDelay(double p99RoundTripMs)
{
	constexpr double FrameMs = 1000.0 / 59.94;
	const double oneWay = p99RoundTripMs / 2;
	return std::clamp(1 + (int)std::ceil(std::max(0.0, oneWay - 1.0) / FrameMs), 2, 6);
}

// round trips, then each way's speed; the numbers (and the input delay they call for) to both Bricks
void runTest()
{
	update([](Status& s) {
		s.state = State::Testing;
		s.error.clear();
	});
	log("leader: testing");
	Message m;
	m.op = Echo;
	m.a = led->testEchoes();
	m.b = EchoSize;
	if (!send(m))
		return leaderClose("The other Brick did not answer.");
	std::vector<double> rtt;
	u8 ping[EchoSize], pong[512];
	for (u32 i = 0; i < m.a; i++)
	{
		if (stopping)
			return;
		std::memset(ping, (int)i, sizeof(ping));
		const double start = now();
		if (led->send(ping, EchoSize, 1000) != (int)EchoSize || led->receive(pong, sizeof(pong), 1000) != (int)EchoSize
				|| std::memcmp(ping, pong, EchoSize) != 0)
		{
			log("leader: echo %u failed: %s", i, std::strerror(errno));
			return leaderClose("Lost the other Brick during the test.");
		}
		rtt.push_back((now() - start) * 1000);
	}
	std::sort(rtt.begin(), rtt.end());
	const double p99 = rtt[rtt.size() * 99 / 100];
	const int delay = inputDelay(p99);
	bool outIntact = false, inIntact = false;
	const double outSpeed = stream(true, outIntact);
	const double inSpeed = outSpeed < 0 ? -1 : stream(false, inIntact);
	if (outSpeed < 0 || inSpeed < 0)
		return stopping ? void() : leaderClose("Lost the other Brick during the test.");
	const u32 bytes = led->testBytes();
	char text[512];
	std::snprintf(text, sizeof(text),
			"Round trip: %.2f ms (99%% within %.2f ms)\nPlayer 1 to player 2: %.2f MB/s\nPlayer 2 to player 1: %.2f MB/s\n"
			"%u KB each way: %s\nInput delay in two-player games: %d frame%s",
			rtt[rtt.size() / 2], p99, outSpeed, inSpeed, bytes >> 10, outIntact && inIntact ? "intact" : "CORRUPTED",
			delay, delay == 1 ? "" : "s");
	std::string oneLine = text;
	std::replace(oneLine.begin(), oneLine.end(), '\n', ';');
	log("leader: %s", oneLine.c_str());
	Message result, ack;
	result.op = Result;
	result.a = (u32)std::strlen(text);
	result.b = (u32)delay;
	if (!send(result) || led->send(text, result.a, 2000) != (int)result.a || !receive(ack, 2000))
		log("leader: the follower did not take the result");
	const std::string shown = text;
	update([&shown, delay](Status& s) {
		s.state = State::Tested;
		s.result = shown;
		s.inputDelay = delay;
	});
}

// the leader's part of the link's thread: the other Brick taken on, pinged, tested
void lead()
{
	if (led == nullptr)
	{
		if (Pipe *pipe = transport->lead())
			if (leaderHello(*pipe) && swapVmus())
			{
				runTest();
				if (led != nullptr)
					ipUp();
			}
		return;
	}
	static double nextPing;
	if (now() < nextPing && !wantTest)
		return;
	nextPing = now() + 0.5;
	Message ping, reply;
	ping.op = Ping;
	if (!command(ping, reply, 1000))
	{
		// a radio holds packets back now and then (Bluetooth): lost after about 3 s without an answer, as
		// for the two-player game itself (network/ggpo.cpp)
		if (++pingsMissed >= 3)
			leaderClose("Lost the other Brick.");
		return;
	}
	pingsMissed = 0;
	if (wantTest.exchange(false) || (reply.flags & 1))
	{
		ipDown();
		runTest();
		if (led != nullptr)
			ipUp();
	}
}

void run()
{
	ownSerial = readText("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber");
	transport->begin();
	while (!stopping)
	{
		transport->poll();
		lead();
		sleepMs(50);
	}
	if (led != nullptr)
		leaderClose("stopped");
	transport->end();
	log("stop");
}

}	// namespace

void update(const std::function<void(Status&)>& change)
{
	std::lock_guard<std::mutex> lock(statusMutex);
	change(current);
	current.version++;
}

void log(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	const char *home = std::getenv("DEADCAST_HOME");
	const std::string path = std::string(home != nullptr ? home : "/tmp") + "/link.log";
	char when[32];
	const time_t now = time(nullptr);
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
	std::lock_guard<std::mutex> lock(logMutex);
	if (FILE *f = fopen(path.c_str(), "a"))
	{
		fprintf(f, "%s %s\n", when, line);
		fclose(f);
	}
}

double now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepMs(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string readText(const std::string& path)
{
	char buffer[256];
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return "";
	ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (n < 0)
		n = 0;
	while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == ' '))
		n--;
	return std::string(buffer, n);
}

unsigned readNumber(const std::string& path, int base)
{
	const std::string text = readText(path);
	return text.empty() ? ~0u : (unsigned)std::strtoul(text.c_str(), nullptr, base);
}

bool writeText(const char *path, const char *text)
{
	const int fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;
	const bool done = write(fd, text, std::strlen(text)) == (ssize_t)std::strlen(text);
	close(fd);
	return done;
}

bool leading()
{
	return led != nullptr;
}

void leaderLost(const std::string& why)
{
	leaderClose(why);
}

void followerLost(const std::string& why)
{
	tunnelStop();
	update([&why](Status& s) {
		if (s.role != Role::Device)
			return;
		s.state = State::Waiting;
		s.role = Role::None;
		s.port.clear();
		s.peer.clear();
		s.error = why;
	});
}

void follow(Pipe& pipe, const std::atomic<bool>& running)
{
	std::vector<u8> buffer(Block);
	std::vector<u8> frame(FrameMax);
	const u32 chunk = std::min(Block, pipe.chunk());
	while (running)
	{
		// an IP frame, or a command; nothing (yet): again, gone: done
		const int n = pipe.receive(frame.data(), FrameMax, 200);
		if (n < 0)
			break;
		if (n > 0 && tunnelTake(frame.data(), n))
			continue;
		Message m;
		if (n == sizeof(m))
			std::memcpy(&m, frame.data(), sizeof(m));
		if (n != sizeof(m) || m.magic != Magic)
		{
			if (n > 0)
				log("follower: %d stray bytes", n);
			continue;
		}
		Message reply;
		reply.op = m.op;
		bool replied = false;
		const double start = now();
		switch (m.op)
		{
		case Hello:
		{
			sayHello(reply);
			update([&m, &pipe](Status& s) {
				s.state = State::Testing;
				s.role = Role::Device;
				s.port = pipe.where;
				heardHello(m, s);
				s.result.clear();
				s.error.clear();
			});
			const Status s = status();
			log("follower: linked to %s over %s%s", s.peer.c_str(), pipe.where.c_str(), s.sameGame ? "" : ", another game");
			// a leader linking afresh brings IP up again after its test
			tunnelStop();
			break;
		}
		case VmuSwap:
		{
			std::vector<u8> peer(std::min(m.a, VmuMax));
			if (!receiveRaw(pipe, peer))
			{
				log("follower: the other Brick's VMU did not arrive");
				break;
			}
			tookVmu(std::move(peer));
			const std::vector<u8> own = ownVmu();
			reply.a = (u32)own.size();
			replied = true;
			if (pipe.send(&reply, sizeof(reply), 2000) != sizeof(reply) || !sendRaw(pipe, own))
				log("follower: this Brick's VMU did not go: %s", std::strerror(errno));
			else
				log("follower: VMUs swapped, %u bytes in, %zu out", m.a, own.size());
			break;
		}
		case IpUp:
			tunnelStart(false, [&pipe](const u8 *data, u32 size) { return pipe.send(data, size, 1000); });
			break;
		case IpDown:
			tunnelStop();
			break;
		case Ping:
			reply.flags = wantTest.exchange(false) ? 1 : 0;
			break;
		case Sink:
		case Source:
			update([](Status& s) { s.state = State::Testing; });
			reply.hash = FirstHash;
			for (u32 k = 0; reply.bytes < m.a && running; k++)
			{
				const u32 size = std::min(chunk, (u32)(m.a - reply.bytes));
				if (m.op == Source && (m.b & 1))
					pattern((u32 *)buffer.data(), k);
				const int r = m.op == Sink ? pipe.receive(buffer.data(), chunk, 2000) : pipe.send(buffer.data(), size, 2000);
				if (r <= 0)
				{
					log("follower: %s stopped after %llu bytes: %s", m.op == Sink ? "receiving" : "sending",
							(unsigned long long)reply.bytes, std::strerror(errno));
					break;
				}
				if (m.b & 1)
					reply.hash = hashWords(reply.hash, buffer.data(), r);
				reply.bytes += r;
			}
			break;
		case Echo:
			for (u32 i = 0; i < m.a && running; i++)
			{
				const int r = pipe.receive(buffer.data(), std::min(m.b, chunk), 2000);
				if (r <= 0 || pipe.send(buffer.data(), r, 1000) != r)
				{
					log("follower: echo %u failed: %s", i, std::strerror(errno));
					break;
				}
				reply.bytes += r;
			}
			break;
		case Result:
		{
			std::string text(std::min(m.a, 4096u), '\0');
			const int r = pipe.receive(&text[0], text.size(), 2000);
			if (r > 0)
			{
				text.resize(r);
				log("follower: the leader's result: %s", text.c_str());
				const int delay = std::clamp((int)m.b, 1, 6);
				update([&text, delay](Status& s) {
					s.state = State::Tested;
					s.result = text;
					s.inputDelay = delay;
				});
			}
			break;
		}
		}
		reply.ns = (u64)((now() - start) * 1e9);
		if (m.op != Echo && !replied && pipe.send(&reply, sizeof(reply), 2000) != sizeof(reply))
			log("follower: no reply to command %u: %s", m.op, std::strerror(errno));
	}
	tunnelStop();
}

}	// namespace core

using namespace core;

void start(Via via)
{
	if (worker != nullptr)
		return;
	stopping = false;
	wantTest = false;
	transport = via == Via::Bluetooth ? bluetoothTransport() : usbTransport();
	update([via](Status& s) {
		const u32 version = s.version;
		s = Status();
		s.version = version;
		s.via = via;
		s.state = State::Waiting;
	});
	worker = new std::thread(run);
}

void stop()
{
	if (worker == nullptr)
		return;
	stopping = true;
	worker->join();
	delete worker;
	worker = nullptr;
	update([](Status& s) {
		const u32 version = s.version;
		s = Status();
		s.version = version;
	});
}

std::atomic<bool> keptUp { false };

void setKept(bool kept)
{
	keptUp = kept;
	// kept until Deadcast exits: then stopped (USB as it was, Bluetooth off again if it was off)
	static bool atExit;
	if (kept && !atExit)
	{
		atExit = true;
		std::atexit(stop);
	}
}

bool kept()
{
	return keptUp;
}

void setGame(const std::string& game, const std::string& vmuFile)
{
	std::lock_guard<std::mutex> lock(gameMutex);
	ownGame = game;
	ownVmuFile = vmuFile;
}

std::vector<u8> peerVmu()
{
	std::lock_guard<std::mutex> lock(gameMutex);
	return peerVmuData;
}

void test()
{
	wantTest = true;
}

Status status()
{
	std::lock_guard<std::mutex> lock(statusMutex);
	return current;
}

}
