// The link's core, shared by its transports (deadcast/link*.cpp only). A transport finds the other Brick
// (USB: link_usb.cpp; Bluetooth: link_bluetooth.cpp) and hands the core a Pipe to it; everything over
// the pipe is the core's, the same on every transport: hello, the VMUs, pings, the test, IP (dclink0).
// The Brick that found the other leads (the USB host; the Bluetooth Brick that connected): it sends the
// commands (link.cpp: the link's thread), and the other answers them (follow(), on a thread of the
// transport's).
#pragma once
#include "link.h"

#include <atomic>
#include <functional>
#include <string>

namespace deadcast::link::core
{

// one packet at a time each way: a USB transfer, an L2CAP packet
struct Pipe
{
	virtual ~Pipe() = default;
	// one packet out, chunk() bytes at most: its size, or < 0
	virtual int send(const void *data, u32 size, u32 timeoutMs) = 0;
	// one packet in, size bytes at most: its size, 0 for nothing (yet), < 0 when the pipe is gone
	virtual int receive(void *data, u32 size, u32 timeoutMs) = 0;
	virtual u32 chunk() const = 0;
	// the test's round trips and its bytes each way
	virtual u32 testEchoes() const = 0;
	virtual u32 testBytes() const = 0;
	std::string where;		// Status::port
};

struct Transport
{
	virtual ~Transport() = default;
	// on the link's thread: begin(), poll() every 50 ms until stopped, end()
	virtual void begin() = 0;
	virtual void poll() = 0;
	virtual void end() = 0;
	// the other Brick found, for this one to lead: a pipe to it (the core says hello), else nullptr
	virtual Pipe *lead() = 0;
	// the led pipe said hello: the other Brick is there
	virtual void linked() {}
	// the led pipe is done with (no hello, lost, stopped): closed
	virtual void leadLost() = 0;
};

Transport *usbTransport();
Transport *bluetoothTransport();

void log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void update(const std::function<void(Status&)>& change);
double now();
void sleepMs(int ms);
std::string readText(const std::string& path);
unsigned readNumber(const std::string& path, int base);
bool writeText(const char *path, const char *text);

extern std::atomic<bool> stopping;
extern std::string ownSerial;

// The other Brick's commands over pipe, answered on this thread while running (or until the pipe is
// gone); IP down when it returns.
void follow(Pipe& pipe, const std::atomic<bool>& running);
// a follower's leader gone: IP down, the page told
void followerLost(const std::string& why);
// leading over a pipe right now
bool leading();
// the led pipe gone under the transport (USB: its hosting ended): let go of, the page told
void leaderLost(const std::string& why);

}
