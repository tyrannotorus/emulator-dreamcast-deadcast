#include "netplay.h"
#include "link.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/maple/maple_cfg.h"
#include "log/Log.h"
#include "network/ggpo.h"

#include <vector>

namespace deadcast::netplay
{
namespace
{

// What changes how a game runs: alike on both Bricks for a session (the defaults Deadcast ships), whatever
// each player set (for the rest of the run: the game goes on with them alone after the session)
struct Alike
{
	std::string section, key;
	const char *value;
};
template<typename T>
Alike alike(config::Option<T>& option, const char *value)
{
	return { option.getSection(), option.getName(), value };
}
const std::vector<Alike>& alikes()
{
	static const std::vector<Alike> list {
		alike(config::Region, "1"), alike(config::Language, "1"), alike(config::DSPEnabled, "no"), alike(config::FastGDRomLoad, "no"),
		alike(config::DynarecEnabled, "yes"),
	};
	return list;
}

bool session;
bool played;			// a session began in this run
bool host;				// in it: player 1
std::vector<u8> otherVmu;	// the other player's VMU

}

void begin(bool asHost, const std::string& peer, int inputDelay)
{
	host = asHost;
	otherVmu = link::peerVmu();
	config::setTransient("network", "GGPO", "yes");
	config::setTransient("network", "ActAsServer", host ? "yes" : "no");
	// both sides name the other (GGPO sends to it from the start; the server too)
	config::setTransient("network", "server", peer);
	// the link's test says how much input delay hides its round trip (the cable's: one frame; Bluetooth's: more)
	config::setTransient("network", "GGPODelay", std::to_string(inputDelay));
	// a USB cable or Bluetooth: no router to open a port on
	config::setTransient("network", "EnableUPnP", "no");
	// player 2's controller in port B
	config::setTransient("input", "device2", std::to_string(MDT_SegaController));
	config::MapleMainDevices[1].load();
	for (const Alike& a : alikes())
		config::setTransient(a.section, a.key, a.value);
	ggpo::setAloneSide(0);
	session = played = true;
	// the link carries the session: up until Deadcast exits
	link::setKept(true);
	WARN_LOG(NETWORK, "Deadcast: two-player session as %s, input delay %d", host ? "host" : peer.c_str(), inputDelay);
}

bool inSession()
{
	return session && !ggpo::netplayAlone();
}

void leave()
{
	ggpo::netplayLeave();
}

bool twoPlayerRun()
{
	return played;
}

Vmu vmu(const std::string& port)
{
	if (port == (host ? "A1" : "B1"))
		return Vmu::Own;
	if (port == (host ? "B1" : "A1"))
		return Vmu::Peer;
	return Vmu::Blank;
}

const std::vector<u8>& peerVmu()
{
	return otherVmu;
}

void requestPause()
{
	ggpo::netplayRequestPause();
}

PauseState pauseState()
{
	const ggpo::NetplayPause p = ggpo::netplayPause();
	PauseState state;
	state.pause = (Pause)p.reason;
	state.peerPresent = p.peerPresent;
	return state;
}

void resume()
{
	ggpo::netplayResume();
}

}
