// The link between two Bricks for two-player games (Options, Emulator Options, Local
// Multiplayer): the other Brick found, the players' VMUs swapped, the link tested, and IP over the link
// (dclink0) for the session (deadcast/netplay.h), which both Bricks start by themselves once linked when
// they play the same game.
// One core (link.cpp, link_core.h) over each way the Bricks connect:
//   USB (link_usb.cpp): a Brick's bottom USB-C port (usbc0) is OTG, a device by default, a host when
//   switched; its top port (usbc1) is a host only. A cable from one Brick's top port to the other's bottom
//   port links them as they are; with a cable from bottom to bottom, one of them hosts over its bottom
//   port: which one, they settle between them (each switches its port between host and device at random
//   until they link).
//   Bluetooth (link_bluetooth.cpp): each Brick looks for the other nearby; one connects (L2CAP).
// Everything runs on threads of its own; what it finds is logged to $DEADCAST_HOME/link.log as well.
#pragma once
#include "types.h"

#include <string>
#include <vector>

namespace deadcast::link
{

enum class Via { Usb, Bluetooth };
enum class State { Off, Waiting, Testing, Tested, Failed };
// Host: this Brick leads the link (the USB host; over Bluetooth, the Brick that connected) and is player 1
enum class Role { None, Host, Device };

struct Status
{
	u32 version = 0;			// changes with everything below
	Via via = Via::Usb;
	State state = State::Off;
	Role role = Role::None;
	bool topPortInUse = false;	// USB: something on the top port: this Brick is the host there
	std::string activity;		// what the link does while it looks (Bluetooth starting...)
	std::string port;			// what the link is over: "the top port", "the bottom port", "Bluetooth"
	std::string peer;			// the other Brick's serial number
	bool sameGame = false;		// it plays this Brick's game (setGame)
	std::string result;			// the last test's, in lines
	std::string address;		// IP over the link (dclink0), once up: this Brick's
	std::string peerAddress;	// and the other Brick's
	int inputDelay = 1;			// frames of input delay for a session over this link (the test's)
	std::string error;
};

// Looks for the other Brick. stop() waits for everything to be as it was (USB: ADB alone on the bottom
// port, a device again; Bluetooth: off again if it was).
void start(Via via);
void stop();
// Kept: the link stays up when the menu that started it closes (a netplay session uses it).
void setKept(bool kept);
bool kept();
// This Brick's game, to tell the other Brick (status().sameGame), and its player's VMU (its file), which the
// two Bricks swap once linked: each plays with both players' VMUs, as a Dreamcast would (deadcast/netplay.h)
void setGame(const std::string& game, const std::string& vmuFile);
// The other Brick's player's VMU, as it came (empty: it has none)
std::vector<u8> peerVmu();
// The test again (the device asks the host for it).
void test();
Status status();

}
