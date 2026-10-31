/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "ggpo.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/mouse.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "net_platform.h"
#include <algorithm>

namespace ggpo
{

bool inRollback;

// Deadcast: after a two-player session, the player's pad on the side it played (1: player 2's port)
static int aloneSide;

void setAloneSide(int side)
{
	aloneSide = side;
}

static void getLocalInput(MapleInputState inputState[4])
{
	if (!config::ThreadedRendering)
		os_UpdateInputState();
	std::lock_guard<std::mutex> lock(relPosMutex);
	for (int player = 0; player < 4; player++)
	{
		MapleInputState& state = inputState[player];
		state.kcode = kcode[player];
		state.halfAxes[PJTI_L] = lt[player];
		state.halfAxes[PJTI_R] = rt[player];
		state.halfAxes[PJTI_L2] = lt2[player];
		state.halfAxes[PJTI_R2] = rt2[player];
		state.fullAxes[PJAI_X1] = joyx[player];
		state.fullAxes[PJAI_Y1] = joyy[player];
		state.fullAxes[PJAI_X2] = joyrx[player];
		state.fullAxes[PJAI_Y2] = joyry[player];
		state.fullAxes[PJAI_X3] = joy3x[player];
		state.fullAxes[PJAI_Y3] = joy3y[player];
		state.mouseButtons = mo_buttons[player];
		state.absPos.x = mo_x_abs[player];
		state.absPos.y = mo_y_abs[player];
		state.keyboard.shift = kb_shift[player];
		memcpy(state.keyboard.key, kb_key[player], sizeof(kb_key[player]));
		int relX = std::round(mo_x_delta[player]);
		int relY = std::round(mo_y_delta[player]);
		int wheel = std::round(mo_wheel_delta[player]);
		state.relPos.x += relX;
		state.relPos.y += relY;
		state.relPos.wheel += wheel;
		mo_x_delta[player] -= relX;
		mo_y_delta[player] -= relY;
		mo_wheel_delta[player] -= wheel;
	}
	if (aloneSide == 1)
		std::swap(inputState[0], inputState[1]);
}

}

#ifdef USE_GGPO
#include "ggponet.h"
#include "emulator.h"
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "hw/mem/mem_watch.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/spg.h"
#include <map>
#include <string.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <numeric>
#include "imgui.h"
#include "miniupnp.h"
#include "hw/naomi/naomi_cart.h"

//#define SYNC_TEST 1

// (Deadcast: for its desync check too)
#include <xxhash.h>
#include <poll.h>

namespace ggpo
{
using namespace std::chrono;

constexpr int MAX_PLAYERS = 2;
constexpr int SERVER_PORT = 19713;

constexpr u32 BTN_TRIGGER_LEFT	= DC_BTN_BITMAPPED_LAST << 1;
constexpr u32 BTN_TRIGGER_RIGHT	= DC_BTN_BITMAPPED_LAST << 2;

#pragma pack(push, 1)
struct VerificationData
{
	const int protocol = 2;
	u8 gameMD5[16] { };
	u8 stateMD5[16] { };
} ;
#pragma pack(pop)

static GGPOSession *ggpoSession;
static int localPlayerNum;
static GGPOPlayerHandle localPlayer;
static GGPOPlayerHandle remotePlayer;
static bool synchronized;
static std::recursive_mutex ggpoMutex;
static std::array<int, 5> msPerFrame;
static int msPerFrameIndex;
static time_point<steady_clock> lastFrameTime;
static int msPerFrameAvg;
static bool _endOfFrame;
static MiniUPnP miniupnp;
static int analogAxes;
static bool absPointerPos;
static bool keyboardGame;
static bool mouseGame;
static int inputSize;
static void (*chatCallback)(int playerNum, const std::string& msg);

struct MemPages
{
	void load()
	{
		memwatch::ramWatcher.getPages(ram);
		memwatch::vramWatcher.getPages(vram);
		memwatch::aramWatcher.getPages(aram);
		memwatch::elanWatcher.getPages(elanram);
	}
	memwatch::PageMap ram;
	memwatch::PageMap vram;
	memwatch::PageMap aram;
	memwatch::PageMap elanram;
};
static std::unordered_map<int, MemPages> deltaStates;
static int lastSavedFrame = -1;

// Deadcast: both sides hash their state every CheckEvery frames and swap the hashes (GameEvent::Checksum),
// each sent CheckSettle frames later, past any rollback that could redo its frame. A frame whose hashes
// differ is a desync: the two emulators no longer run alike.
constexpr int CheckEvery = 300;
constexpr int CheckSettle = 60;
static std::mutex checkMutex;
static std::map<int, u64> ownHashes, peerHashes;
static int checkToSend = -1;
static u32 checksInStep, desyncs;

// each frame both Bricks have a hash for, compared once; this Brick's hashes kept until sent
static void compareHashes()
{
	for (auto peer = peerHashes.begin(); peer != peerHashes.end(); )
	{
		auto own = ownHashes.find(peer->first);
		if (own == ownHashes.end())
		{
			// gone by here (older than any kept): never to be compared
			if (!ownHashes.empty() && peer->first < ownHashes.begin()->first)
				peer = peerHashes.erase(peer);
			else
				++peer;
			continue;
		}
		if (own->second == peer->second)
		{
			checksInStep++;
			WARN_LOG(NETWORK, "Deadcast: frame %d in step with the other Brick (%u checks, %u desyncs)", peer->first, checksInStep, desyncs);
		}
		else
		{
			if (desyncs++ == 0)
				os_notify("The two Bricks are out of step", 5000);
			WARN_LOG(NETWORK, "Deadcast: DESYNC at frame %d: %016llx here, %016llx there (%u checks in step)", peer->first,
					(unsigned long long)own->second, (unsigned long long)peer->second, checksInStep);
		}
		peer = peerHashes.erase(peer);
	}
	// this Brick's own, a few checks back at most
	while (ownHashes.size() > 4)
		ownHashes.erase(ownHashes.begin());
}

static int timesyncOccurred;

#pragma pack(push, 1)
struct Inputs
{
	u32 kcode:20;
	u32 mouseButtons:4;
	u32 kbModifiers:8;

	union {
		struct {
			u8 x;
			u8 y;
		} analog;
		struct {
			s16 x;
			s16 y;
		} absPos;
		struct {
			s16 x;
			s16 y;
			s16 wheel;
		} relPos;
		u8 keys[6];
	} u;
};
static_assert(sizeof(Inputs) == 10, "wrong Inputs size");
static_assert(BTN_TRIGGER_RIGHT < (1 << 20));

struct GameEvent
{
	enum : char {
		Chat,
		VF4Card,
		Checksum	// Deadcast: the desync check's
	} type;
	union {
		struct {
			u8 playerNum;
			char message[512 - sizeof(playerNum) - sizeof(type)];
		} chat;
		struct {
			u8 playerNum;
			u8 data[128];
		} card;
		struct {
			s32 frame;
			u64 hash;
		} checksum;
	} u;

	constexpr static int chatMessageLen(int len) { return len - sizeof(u.chat.playerNum) - sizeof(type); }
};
#pragma pack(pop)

/*
 * begin_game callback - This callback has been deprecated.  You must
 * implement it, but should ignore the 'game' parameter.
 */
static bool begin_game(const char *)
{
	DEBUG_LOG(NETWORK, "Game begin");
	rend_allow_rollback();
	return true;
}

/*
 * on_event - Notification that something has happened.  See the GGPOEventCode
 * structure for more information.
 */
static bool on_event(GGPOEvent *info)
{
	using namespace i18n;
	switch (info->code) {
	case GGPO_EVENTCODE_CONNECTED_TO_PEER:
		INFO_LOG(NETWORK, "Connected to peer %d", info->u.connected.player);
		os_notify(T("Connected to peer"), 2000);
		break;
	case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
		INFO_LOG(NETWORK, "Synchronizing with peer %d", info->u.synchronizing.player);
		os_notify(T("Synchronizing with peer"), 2000);
		break;
	case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
		INFO_LOG(NETWORK, "Synchronized with peer %d", info->u.synchronized.player);
		os_notify(T("Synchronized with peer"), 2000);
		break;
	case GGPO_EVENTCODE_RUNNING:
		INFO_LOG(NETWORK, "Running");
		os_notify(T("Running"), 2000);
		synchronized = true;
		break;
	case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
		INFO_LOG(NETWORK, "Disconnected from peer %d", info->u.disconnected.player);
		throw FlycastException(Ts("Disconnected from peer"));
		break;
	case GGPO_EVENTCODE_TIMESYNC:
		INFO_LOG(NETWORK, "Timesync: %d frames ahead", info->u.timesync.frames_ahead);
		timesyncOccurred += 5;
		std::this_thread::sleep_for(std::chrono::milliseconds(1000 / (msPerFrameAvg >= 25 ? 30 : 60)));
		break;
	case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
		INFO_LOG(NETWORK, "Connection interrupted with player %d", info->u.connection_interrupted.player);
		os_notify(T("Connection interrupted"), 2000);
		break;
	case GGPO_EVENTCODE_CONNECTION_RESUMED:
		INFO_LOG(NETWORK, "Connection resumed with player %d", info->u.connection_resumed.player);
		os_notify(T("Connection resumed"), 2000);
		break;
	}
	return true;
}

/*
 * advance_frame - Called during a rollback.  You should advance your game
 * state by exactly one frame.  Before each frame, call ggpo_synchronize_input
 * to retrieve the inputs you should use for that frame.  After each frame,
 * you should call ggpo_advance_frame to notify GGPO.net that you're
 * finished.
 *
 * The flags parameter is reserved.  It can safely be ignored at this time.
 */
static bool advance_frame(int)
{
	INFO_LOG(NETWORK, "advance_frame");
	settings.aica.muteAudio = true;
	rend_enable_renderer(false);
	inRollback = true;

	emu.run();
	ggpo_advance_frame(ggpoSession);

	settings.aica.muteAudio = false;
	rend_enable_renderer(true);
	inRollback = false;
	_endOfFrame = false;

	return true;
}

/*
 * load_game_state - GGPO.net will call this function at the beginning
 * of a rollback.  The buffer and len parameters contain a previously
 * saved state returned from the save_game_state function.  The client
 * should make the current game state match the state contained in the
 * buffer.
 */
static bool load_game_state(unsigned char *buffer, int len)
{
	INFO_LOG(NETWORK, "load_game_state");

	rend_start_rollback();
	// FIXME dynarecs
	Deserializer deser(buffer, len, true);
	int frame;
	deser >> frame;
	memwatch::unprotect();
	for (int f = lastSavedFrame - 1; f >= frame; f--)
	{
		const MemPages& pages = deltaStates[f];
		for (const auto& pair : pages.ram)
			memcpy(memwatch::ramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.vram)
			memcpy(memwatch::vramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.aram)
			memcpy(memwatch::aramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.elanram)
			memcpy(memwatch::elanWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		DEBUG_LOG(NETWORK, "Restored frame %d pages: %d ram, %d vram, %d eram, %d aica ram", f, (u32)pages.ram.size(),
					(u32)pages.vram.size(), (u32)pages.elanram.size(), (u32)pages.aram.size());
	}
	dc_deserialize(deser);
	if (deser.size() != (u32)len)
	{
		ERROR_LOG(NETWORK, "load_game_state len %d used %d", len, (int)deser.size());
		die("fatal");
	}
	rend_allow_rollback();	// ggpo might load another state right after this one
	memwatch::reset();
	memwatch::protect();
	return true;
}

/*
 * save_game_state - The client should allocate a buffer, copy the
 * entire contents of the current game state into it, and copy the
 * length into the *len parameter.  Optionally, the client can compute
 * a checksum of the data and store it in the *checksum argument.
 */
static bool save_game_state(unsigned char **buffer, int *len, int *checksum, int frame)
{
	verify(!emu.getSh4Executor()->IsCpuRunning());
	lastSavedFrame = frame;
	// TODO this is way too much memory
	size_t allocSize = settings.platform.isNaomi() ? 20_MB : 10_MB;
	*buffer = (unsigned char *)malloc(allocSize);
	if (*buffer == nullptr)
	{
		WARN_LOG(NETWORK, "Memory alloc failed");
		*len = 0;
		return false;
	}
	Serializer ser(*buffer, allocSize, true);
	try {
		ser << frame;
		dc_serialize(ser);
		*len = ser.size();
	} catch (const Serializer::Exception& e) {
		WARN_LOG(NETWORK, "Save state failed: %s", e.what());
		*len = 0;
		return false;
	}
	// Deadcast: the desync check (redone after a rollback: the last hash of a frame is the one sent)
	if (frame % CheckEvery == 0)
	{
		// main RAM: the game's state (the rollback state also holds things that differ between machines)
		const u64 hash = XXH3_64bits(&mem_b[0], settings.platform.ram_size);
		std::lock_guard<std::mutex> lock(checkMutex);
		ownHashes[frame] = hash;
	}
	if (frame >= CheckSettle && (frame - CheckSettle) % CheckEvery == 0)
		checkToSend = frame - CheckSettle;
#ifdef SYNC_TEST
	*checksum = XXH3_64bits(*buffer, usedSize);
#endif
	memwatch::protect();
	if (frame > 0)
	{
#ifdef SYNC_TEST
		if (deltaStates.count(frame - 1) != 0)
		{
			MemPages memPages;
			memPages.load();
			const MemPages& savedPages = deltaStates[frame - 1];
			//verify(memPages.ram.size() == savedPages.ram.size());
			if (memPages.ram.size() != savedPages.ram.size())
			{
				ERROR_LOG(NETWORK, "old ram size %d new %d", (u32)savedPages.ram.size(), (u32)memPages.ram.size());
				if (memPages.ram.size() > savedPages.ram.size())
					for (const auto& pair : memPages.ram)
					{
						if (savedPages.ram.count(pair.first) == 0)
							ERROR_LOG(NETWORK, "new page @ %x", pair.first);
						else
							DEBUG_LOG(NETWORK, "page ok @ %x", pair.first);
					}
				die("fatal");
			}
			for (const auto& pair : memPages.ram)
			{
				verify(savedPages.ram.count(pair.first) == 1);
				verify(memcmp(&pair.second[0], &savedPages.ram.find(pair.first)->second[0], PAGE_SIZE) == 0);
			}
			verify(memPages.vram.size() == savedPages.vram.size());
			for (const auto& pair : memPages.vram)
			{
				verify(savedPages.vram.count(pair.first) == 1);
				verify(memcmp(&pair.second[0], &savedPages.vram.find(pair.first)->second[0], PAGE_SIZE) == 0);
			}
			//verify(memPages.aram.size() == savedPages.aram.size());
			if (memPages.aram.size() != savedPages.aram.size())
			{
				ERROR_LOG(NETWORK, "old aram size %d new %d", (u32)savedPages.aram.size(), (u32)memPages.aram.size());
				die("fatal");
			}
			for (const auto& pair : memPages.aram)
			{
				verify(savedPages.aram.count(pair.first) == 1);
				verify(memcmp(&pair.second[0], &savedPages.aram.find(pair.first)->second[0], PAGE_SIZE) == 0);
			}
		}
#endif
		// Save the delta to frame-1
		deltaStates[frame - 1].load();
		DEBUG_LOG(NETWORK, "Saved frame %d pages: %d ram, %d vram, %d eram, %d aica ram", frame - 1, (u32)deltaStates[frame - 1].ram.size(),
				(u32)deltaStates[frame - 1].vram.size(), (u32)deltaStates[frame - 1].elanram.size(), (u32)deltaStates[frame - 1].aram.size());
	}

	return true;
}

/*
 * log_game_state - Used in diagnostic testing.  The client should use
 * the ggpo_log function to write the contents of the specified save
 * state in a human readible form.
 */
static bool log_game_state(char *filename, unsigned char *buffer, int len)
{
#ifdef SYNC_TEST
	static int lastLoggedFrame = -1;
	static u8 *lastState;
	int frame = *(u32 *)buffer;
	DEBUG_LOG(NETWORK, "log_game_state frame %d len %d", frame, len);
	if (lastLoggedFrame == frame) {
		for (int i = 0; i < len; i++)
			if (buffer[i] != lastState[i])
			{
				WARN_LOG(NETWORK, "States for frame %d differ at offset %d: now %x prev %x", frame, i, *(u32 *)&buffer[i & ~3], *(u32 *)&lastState[i & ~3]);
				break;
			}
	}
	lastState = buffer;
	lastLoggedFrame = frame;
#endif

	return true;
}

/*
 * free_buffer - Frees a game state allocated in save_game_state.  You
 * should deallocate the memory contained in the buffer.
 */
static void free_buffer(void *buffer)
{
	if (buffer != nullptr)
	{
		Deserializer deser(buffer, 1_MB, true);
		int frame;
		deser >> frame;
		deltaStates.erase(frame);
		free(buffer);
	}
}

static void on_message(u8 *msg, int len)
{
	if (len == 0)
		return;
	GameEvent *event = (GameEvent *)msg;
	switch (event->type)
	{
	case GameEvent::Chat:
		if (chatCallback != nullptr && GameEvent::chatMessageLen(len) > 0)
			chatCallback(event->u.chat.playerNum, std::string(event->u.chat.message, GameEvent::chatMessageLen(len)));
		break;

	case GameEvent::VF4Card:
		setRfidCardData(event->u.card.playerNum, event->u.card.data);
		break;

	case GameEvent::Checksum:
	{
		std::lock_guard<std::mutex> lock(checkMutex);
		peerHashes[event->u.checksum.frame] = event->u.checksum.hash;
		compareHashes();
		break;
	}

	default:
		WARN_LOG(NETWORK, "Unknown app message type %d", event->type);
		break;
	}
}

// Deadcast: lockstep netplay, in place of GGPO's rollback (no state saved each frame, none loaded back):
// a frame runs only once both Bricks have both players' inputs for it, each Brick's sent Delay frames
// ahead and again in the packets after (a lost packet costs nothing). Over the USB link (well under a
// millisecond each way) the wait costs next to nothing, where GGPO's state saved every frame cost the
// Brick 40% of its speed (2026-10-31). Same start (the disc's, deadcast/netplay.h), same inputs, same
// frames: the desync check (main RAM hashed every CheckEvery frames, swapped in the same packets) says if
// not.
// Pausing: MENU puts PauseBit in this Brick's next input, so both Bricks stop before the same frame and
// show the menu; Continue on either goes on (the resume count in every packet takes the other along).
// The other Brick silent for TimeoutMs (a cable pulled, a Brick switched off) or gone (it left): paused
// as well, until it is back (Continue then goes on with it) or this player goes on alone.
namespace lockstep
{
constexpr u32 Magic = 0x4b434f4c;		// "LOCK"
enum : u32 { Hello, Frames };
enum : u32 { LeftFlag = 1, PausedFlag = 2 };	// Packet::flags
constexpr u32 PauseBit = 1;				// in Inputs::kbModifiers (no keyboard in a Dreamcast session)
constexpr int Window = 16;				// inputs in every packet
constexpr int Ring = 64;
constexpr int TimeoutMs = 3000;			// no input from the other Brick for this long: it is gone

#pragma pack(push, 1)
struct Packet
{
	u32 magic = Magic;
	u32 kind = Frames;
	s32 frame = -1;			// the newest input's
	u32 count = 0;			// inputs, oldest first, the newest for frame
	u8 verif[32] {};		// Hello: the game's and the state's MD5s
	s32 hashFrame = -1;		// the desync check's latest
	u64 hash = 0;
	u32 resumes = 0;		// how often the session went on after a pause
	u32 flags = 0;
	Inputs inputs[Window];
};
#pragma pack(pop)

static int sock = -1;
static sockaddr_in peer;
static std::atomic<bool> running { false };
static int frame;					// the frame running
static int delay;
static int lastLocal;				// the newest frame this Brick's input is in for
static int lastRemote;				// the newest frame the other Brick's input is in for
static Inputs local[Ring], remote[Ring];
static u8 verif[32];
static int ownHashFrame = -1;
static u64 ownHash;
static int lastPeerHashFrame = -1;
// the pause (netplayPause() for the menu)
static std::atomic<bool> paused { false };
static std::atomic<int> pauseReason { (int)PauseReason::None };
static std::atomic<bool> peerPresent { true };
static std::atomic<bool> peerLeft { false };
static std::atomic<bool> wantPause { false };
static std::atomic<bool> wantResume { false };
static std::atomic<u32> resumes { 0 };
static int pausedBefore = -1;		// the frame the last menu pause stopped before
static std::atomic<s64> lastHeardMs { 0 };
static float waitedMs;				// for the other Brick's input, since the last desync check

static s64 nowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static Inputs sampleLocal()
{
	if (!config::ThreadedRendering)
		os_UpdateInputState();
	Inputs inputs {};
	inputs.kcode = ~kcode[0];
	if (rt[0] >= 0x4000)
		inputs.kcode |= BTN_TRIGGER_RIGHT;
	else
		inputs.kcode &= ~BTN_TRIGGER_RIGHT;
	if (lt[0] >= 0x4000)
		inputs.kcode |= BTN_TRIGGER_LEFT;
	else
		inputs.kcode &= ~BTN_TRIGGER_LEFT;
	if (analogAxes > 0)
	{
		inputs.u.analog.x = joyx[0] >> 8;
		if (analogAxes >= 2)
			inputs.u.analog.y = joyy[0] >> 8;
	}
	if (wantPause.exchange(false))
		inputs.kbModifiers |= PauseBit;
	return inputs;
}

static void toMaple(const Inputs& inputs, MapleInputState& state)
{
	state = {};
	state.kcode = ~inputs.kcode;
	if (analogAxes > 0)
	{
		state.fullAxes[PJAI_X1] = inputs.u.analog.x << 8;
		if (analogAxes >= 2)
			state.fullAxes[PJAI_Y1] = inputs.u.analog.y << 8;
	}
	state.halfAxes[PJTI_R] = (state.kcode & BTN_TRIGGER_RIGHT) == 0 ? 0xffff : 0;
	state.halfAxes[PJTI_L] = (state.kcode & BTN_TRIGGER_LEFT) == 0 ? 0xffff : 0;
}

static void send(u32 kind, u32 flags = 0)
{
	Packet p;
	p.kind = kind;
	std::memcpy(p.verif, verif, sizeof(verif));
	p.hashFrame = ownHashFrame;
	p.hash = ownHash;
	p.resumes = resumes;
	p.flags = flags | (paused ? PausedFlag : 0);
	if (kind == Frames)
	{
		// this Brick's inputs so far, Window of them at most
		const int newest = lastLocal;
		const int oldest = std::max(0, newest - Window + 1);
		p.frame = newest;
		p.count = newest - oldest + 1;
		for (int f = oldest; f <= newest; f++)
			p.inputs[f - oldest] = local[f % Ring];
	}
	sendto(sock, (const char *)&p, sizeof(p), 0, (sockaddr *)&peer, sizeof(peer));
}

// what came from the other Brick, waiting up to timeoutMs for something: false if nothing came
static bool receive(int timeoutMs)
{
	pollfd pfd { sock, POLLIN, 0 };
	if (poll(&pfd, 1, timeoutMs) <= 0)
		return false;
	Packet p;
	bool got = false;
	ssize_t n;
	while ((n = recv(sock, (char *)&p, sizeof(p), 0)) > 0)
	{
		if (n != sizeof(p) || p.magic != Magic)
			continue;
		got = true;
		lastHeardMs = nowMs();
		if (p.flags & LeftFlag)
			peerLeft = true;
		// the other Brick went on after a pause: this one follows
		if (p.resumes > resumes)
			resumes = p.resumes;
		if (p.kind == Hello)
		{
			if (std::memcmp(p.verif, verif, sizeof(verif)) != 0)
				throw FlycastException("The other Brick has another game, or another start");
			continue;
		}
		const int oldest = p.frame - (int)p.count + 1;
		for (int f = std::max(oldest, lastRemote + 1); f <= p.frame; f++)
			remote[f % Ring] = p.inputs[f - oldest];
		lastRemote = std::max(lastRemote, p.frame);
		if (p.hashFrame > lastPeerHashFrame)
		{
			lastPeerHashFrame = p.hashFrame;
			std::lock_guard<std::mutex> lock(checkMutex);
			peerHashes[p.hashFrame] = p.hash;
			compareHashes();
		}
	}
	return got;
}

// the session over here: the other Brick told (a few times: UDP), the socket closed
static void stop()
{
	if (sock >= 0 && running)
		for (int i = 0; i < 5; i++)
			send(Frames, LeftFlag);
	running = false;
	paused = false;
	if (sock >= 0)
		closesocket(sock);
	sock = -1;
}

static void start(int port, const VerificationData& verification)
{
	std::memcpy(verif, verification.gameMD5, 16);
	std::memcpy(verif + 16, verification.stateMD5, 16);
	const std::string server = config::NetworkServer.get();
	const size_t colon = server.find(':');
	peer = {};
	peer.sin_family = AF_INET;
	peer.sin_port = htons(colon == std::string::npos ? SERVER_PORT : atoi(server.substr(colon + 1).c_str()));
	if (inet_pton(AF_INET, server.substr(0, colon).c_str(), &peer.sin_addr) != 1)
		throw FlycastException("The other Brick's address is not set");
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in self {};
	self.sin_family = AF_INET;
	self.sin_port = htons(port);
	self.sin_addr.s_addr = htonl(INADDR_ANY);
	if (sock < 0 || bind(sock, (sockaddr *)&self, sizeof(self)) != 0)
	{
		if (sock >= 0)
			closesocket(sock);
		sock = -1;
		throw FlycastException("Netplay: no UDP port");
	}
	set_non_blocking(sock);
	delay = std::max(1, config::GGPODelay.get());
	frame = 0;
	// the first frames, before any input could get across: nothing pressed, on both
	for (int f = 0; f < Ring; f++)
		local[f] = remote[f] = Inputs {};
	lastLocal = lastRemote = delay - 1;
	ownHashFrame = lastPeerHashFrame = -1;
	{
		std::lock_guard<std::mutex> lock(checkMutex);
		ownHashes.clear();
		peerHashes.clear();
	}
	paused = false;
	pauseReason = (int)PauseReason::None;
	peerPresent = true;
	peerLeft = false;
	wantPause = false;
	wantResume = false;
	resumes = 0;
	pausedBefore = -1;
	lastHeardMs = nowMs();
	running = true;
	WARN_LOG(NETWORK, "Deadcast: lockstep netplay as player %d, input %d frame(s) ahead", localPlayerNum + 1, delay);
}

// the other Brick there (its hello or its first frames): both start at frame 0. Not there within
// HandshakeMs (it never restarted, or left): the session ends, the game goes on alone.
static void handshake()
{
	constexpr int HandshakeMs = 10000;
	const auto start = std::chrono::steady_clock::now();
	while (running)
	{
		send(Hello);
		if (receive(100))
			break;
		if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(HandshakeMs))
		{
			stop();
			throw FlycastException("The other Brick did not come");
		}
	}
	// one more, for the other Brick to know it was heard
	if (running)
		send(Hello);
}

// stopped before frame (a menu pause: both Bricks alike), until Continue here or on the other Brick
// (whose resume count then comes in), or the session's end (the menu's Leave or Quit: stop())
static void pauseLoop(PauseReason reason)
{
	const u32 at = resumes;
	pauseReason = (int)reason;
	paused = true;
	WARN_LOG(NETWORK, "Deadcast: two-player game paused before frame %d (%d)", frame, (int)reason);
	s64 heartbeat = 0;
	while (running && resumes == at)
	{
		if (nowMs() - heartbeat >= 100)
		{
			send(Frames);
			heartbeat = nowMs();
		}
		receive(50);
		const bool present = !peerLeft && nowMs() - lastHeardMs < TimeoutMs;
		peerPresent = present;
		if (peerLeft)
			pauseReason = (int)PauseReason::Left;
		else if (!present)
			pauseReason = (int)PauseReason::Lost;
		if (wantResume.exchange(false) && present)
			resumes = at + 1;
	}
	paused = false;
	pauseReason = (int)PauseReason::None;
	if (!running)
		throw FlycastException("The two-player game is over");
	WARN_LOG(NETWORK, "Deadcast: two-player game goes on at frame %d", frame);
	// the other Brick learns at once
	send(Frames);
}

static void nextFrame()
{
	// the frame just run: the desync check at its end, then this Brick's input for frame + delay
	if (frame % CheckEvery == 0)
	{
		ownHash = XXH3_64bits(&mem_b[0], settings.platform.ram_size);
		ownHashFrame = frame;
		if (frame != 0)
			WARN_LOG(NETWORK, "Deadcast: frames %d-%d waited %.0f ms for the other Brick", frame - CheckEvery, frame, waitedMs);
		waitedMs = 0.f;
		std::lock_guard<std::mutex> lock(checkMutex);
		ownHashes[frame] = ownHash;
		compareHashes();
	}
	lastLocal = frame + delay;
	local[lastLocal % Ring] = sampleLocal();
	frame++;
	send(Frames);
	// the next frame's input from the other Brick: none for TimeoutMs, or it left: paused until it is back.
	// The time waited for it is not lag for frame skipping to make up (spg.cpp)
	s64 since = nowMs();
	const auto waitStart = std::chrono::steady_clock::now();
	while (lastRemote < frame)
	{
		if (!running)
			throw FlycastException("The two-player game is over");
		if (peerLeft)
		{
			pauseLoop(PauseReason::Left);
			since = nowMs();
			continue;
		}
		const int waited = (int)(nowMs() - since);
		if (waited >= TimeoutMs)
		{
			pauseLoop(PauseReason::Lost);
			since = nowMs();
			continue;
		}
		if (!receive(std::min(20, TimeoutMs - waited)))
			send(Frames);
	}
	const float waited = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
	sh4WaitedMs += waited;
	waitedMs += waited;
	// a pause asked for this frame by either Brick: both stop before it
	const u32 mine = local[frame % Ring].kbModifiers & PauseBit;
	const u32 theirs = remote[frame % Ring].kbModifiers & PauseBit;
	if ((mine | theirs) != 0 && frame != pausedBefore)
	{
		pausedBefore = frame;
		pauseLoop(mine != 0 ? PauseReason::Menu : PauseReason::PeerMenu);
	}
	receive(0);
}

static void input(MapleInputState inputState[4])
{
	for (int player = 0; player < 4; player++)
		inputState[player] = {};
	toMaple(local[frame % Ring], inputState[localPlayerNum]);
	toMaple(remote[frame % Ring], inputState[1 - localPlayerNum]);
}

}

NetplayPause netplayPause()
{
	NetplayPause p;
	p.paused = lockstep::running && lockstep::paused;
	p.reason = p.paused ? (PauseReason)lockstep::pauseReason.load() : PauseReason::None;
	p.peerPresent = lockstep::peerPresent;
	return p;
}

void netplayRequestPause()
{
	if (lockstep::running && !lockstep::paused)
		lockstep::wantPause = true;
}

void netplayResume()
{
	lockstep::wantResume = true;
}

int sessionSide()
{
	return localPlayerNum;
}

void startSession(int localPort, int localPlayerNum)
{
	GGPOSessionCallbacks cb{};
	cb.begin_game      = begin_game;
	cb.advance_frame   = advance_frame;
	cb.load_game_state = load_game_state;
	cb.save_game_state = save_game_state;
	cb.free_buffer     = free_buffer;
	cb.on_event        = on_event;
	cb.log_game_state  = log_game_state;
	cb.on_message      = on_message;

#ifdef SYNC_TEST
	GGPOErrorCode result = ggpo_start_synctest(&ggpoSession, &cb, settings.content.gameId.c_str(), MAX_PLAYERS, sizeof(kcode[0]), 1);
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO start sync session failed: %d", result);
		ggpoSession = nullptr;
		throw FlycastException("GGPO start sync session failed");
	}
	ggpo_idle(ggpoSession, 0);
	ggpo::localPlayerNum = localPlayerNum;
	GGPOPlayer player{ sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, localPlayerNum + 1 };
	result = ggpo_add_player(ggpoSession, &player, &localPlayer);
	player.player_num = (1 - localPlayerNum) + 1;
	result = ggpo_add_player(ggpoSession, &player, &remotePlayer);
	synchronized = true;
	analogAxes = 0;
	NOTICE_LOG(NETWORK, "GGPO synctest session started");
#else
	if (settings.platform.isConsole())
		analogAxes = config::GGPOAnalogAxes;
	else
	{
		analogAxes = 0;
		absPointerPos = false;
		keyboardGame = false;
		mouseGame = false;
		if (settings.input.lightgunGame)
			absPointerPos = true;
		else if (settings.input.keyboardGame)
			keyboardGame = true;
		else if (settings.input.mouseGame)
			mouseGame = true;
		else if (NaomiGameInputs != nullptr)
		{
			for (const auto& axis : NaomiGameInputs->axes)
			{
				if (axis.name == nullptr)
					break;
				if (axis.type == Full)
					analogAxes = std::max(analogAxes, (int)axis.axis + 1);
			}
		}
		NOTICE_LOG(NETWORK, "GGPO: Using %d full analog axes", analogAxes);
	}
	inputSize = sizeof(kcode[0]) + analogAxes + (int)absPointerPos * sizeof(Inputs::u.absPos)
		+ (int)keyboardGame * sizeof(Inputs::u.keys) + (int)mouseGame * sizeof(Inputs::u.relPos);

	VerificationData verif;
	MD5Sum().add(settings.network.md5.bios)
			.add(settings.network.md5.game)
			.getDigest(verif.gameMD5);
	auto& digest = settings.network.md5.savestate;
	if (std::find_if(std::begin(digest), std::end(digest), [](u8 b) { return b != 0; }) != std::end(digest))
		memcpy(verif.stateMD5, digest, sizeof(digest));
	else
	{
		MD5Sum().add(settings.network.md5.nvmem)
				.add(settings.network.md5.nvmem2)
				.add(settings.network.md5.eeprom)
				.add(settings.network.md5.vmu)
				.getDigest(verif.stateMD5);
	}

	// Deadcast: lockstep, not GGPO's rollback (see lockstep above)
	ggpo::localPlayerNum = localPlayerNum;
	lockstep::start(localPort, verif);
	return;

	GGPOErrorCode result = ggpo_start_session(&ggpoSession, &cb, settings.content.gameId.c_str(), MAX_PLAYERS, inputSize, localPort,
			&verif, sizeof(verif));
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO start session failed: %d", result);
		ggpoSession = nullptr;
		throw FlycastException("GGPO network initialization failed");
	}

	// automatically disconnect clients after 3000 ms and start our count-down timer
	// for disconnects after 1000 ms.   To completely disable disconnects, simply use
	// a value of 0 for ggpo_set_disconnect_timeout.
	ggpo_set_disconnect_timeout(ggpoSession, 3000);
	ggpo_set_disconnect_notify_start(ggpoSession, 1000);

	ggpo::localPlayerNum = localPlayerNum;
	GGPOPlayer player{ sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, localPlayerNum + 1 };
	result = ggpo_add_player(ggpoSession, &player, &localPlayer);
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO cannot add local player: %d", result);
		stopSession();
		throw FlycastException("GGPO cannot add local player");
	}
	ggpo_set_frame_delay(ggpoSession, localPlayer, config::GGPODelay.get());

	size_t colon = config::NetworkServer.get().find(':');
	std::string peerIp = config::NetworkServer.get().substr(0, colon);
	if (peerIp.empty())
		peerIp = "127.0.0.1";
	u32 peerPort;
	if (colon == std::string::npos)
	{
		if (peerIp == "127.0.0.1")
			peerPort = localPort ^ 1;
		else
			peerPort = SERVER_PORT;
	}
	else
	{
		peerPort = atoi(config::NetworkServer.get().substr(colon + 1).c_str());
	}
	player.type = GGPO_PLAYERTYPE_REMOTE;
	// GGPO only takes a dotted IPv4 address: resolve host names here
	in_addr peerAddr;
	if (inet_pton(AF_INET, peerIp.c_str(), &peerAddr) != 1)
	{
		addrinfo hints {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo *result = nullptr;
		if (getaddrinfo(peerIp.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
		{
			stopSession();
			throw FlycastException(strprintf(i18n::T("GGPO: unknown peer address %s"), peerIp.c_str()));
		}
		char address[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &((sockaddr_in *)result->ai_addr)->sin_addr, address, sizeof(address));
		freeaddrinfo(result);
		NOTICE_LOG(NETWORK, "GGPO: peer %s resolved to %s", peerIp.c_str(), address);
		peerIp = address;
	}
	strcpy(player.u.remote.ip_address, peerIp.c_str());
	player.u.remote.port = peerPort;
	player.player_num = (1 - localPlayerNum) + 1;
	result = ggpo_add_player(ggpoSession, &player, &remotePlayer);
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO cannot add remote player: %d", result);
		stopSession();
		throw FlycastException("GGPO cannot add remote player");
	}
	DEBUG_LOG(NETWORK, "GGPO session started");
#endif
}

void stopSession()
{
	std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
	if (lockstep::running)
	{
		lockstep::stop();
		emu.setNetworkState(false);
		memwatch::unprotect();
		memwatch::reset();
		return;
	}
	if (ggpoSession == nullptr)
		return;
	ggpo_close_session(ggpoSession);
	ggpoSession = nullptr;
	miniupnp.Term();
	emu.setNetworkState(false);
	memwatch::unprotect();
	memwatch::reset();
}

void getInput(MapleInputState inputState[4])
{
	std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
	if (lockstep::running)
	{
		lockstep::input(inputState);
		return;
	}
	if (ggpoSession == nullptr)
	{
		getLocalInput(inputState);
		return;
	}
	for (int player = 0; player < 4; player++)
		inputState[player] = {};

	std::vector<u8> inputData(inputSize * MAX_PLAYERS);
	// should not call any callback
	GGPOErrorCode error = ggpo_synchronize_input(ggpoSession, (void *)&inputData[0], inputData.size(), nullptr);
	if (error != GGPO_OK)
	{
		stopSession();
		throw FlycastException("GGPO error");
	}

	for (int player = 0; player < MAX_PLAYERS; player++)
	{
		MapleInputState& state = inputState[player];
		const Inputs *inputs = (Inputs *)&inputData[player * inputSize];
		state.kcode = ~inputs->kcode;
		if (analogAxes > 0)
		{
			state.fullAxes[PJAI_X1] = inputs->u.analog.x << 8;
			if (analogAxes >= 2)
				state.fullAxes[PJAI_Y1] = inputs->u.analog.y << 8;
		}
		else if (absPointerPos)
		{
			state.absPos.x = inputs->u.absPos.x;
			state.absPos.y = inputs->u.absPos.y;
		}
		else if (keyboardGame)
		{
			memcpy(state.keyboard.key, inputs->u.keys, sizeof(state.keyboard.key));
			state.keyboard.shift = inputs->kbModifiers;
		}
		else if (mouseGame)
		{
			state.relPos.x = inputs->u.relPos.x;
			state.relPos.y = inputs->u.relPos.y;
			state.relPos.wheel = inputs->u.relPos.wheel;
			state.mouseButtons = ~inputs->mouseButtons;
		}
		state.halfAxes[PJTI_R] = (state.kcode & BTN_TRIGGER_RIGHT) == 0 ? 0xffff : 0;
		state.halfAxes[PJTI_L] = (state.kcode & BTN_TRIGGER_LEFT) == 0 ? 0xffff : 0;
	}
}

bool nextFrame()
{
	if (!_endOfFrame)
		return false;
	_endOfFrame = false;
	if (inRollback)
		return true;
	auto now = std::chrono::steady_clock::now();
	if (lastFrameTime != time_point<steady_clock>())
	{
		msPerFrame[msPerFrameIndex++] = duration_cast<milliseconds>(now - lastFrameTime).count();
		if (msPerFrameIndex >= (int)msPerFrame.size())
			msPerFrameIndex = 0;
		msPerFrameAvg = std::accumulate(msPerFrame.begin(), msPerFrame.end(), 0) / msPerFrame.size();
	}
	lastFrameTime = now;

	// (not under ggpoMutex: a pause waits in there for the menu, which takes it)
	if (lockstep::running)
	{
		// the next frame, the SH4 going again (endOfFrame stopped it), as GGPO's does below
		lockstep::nextFrame();
		emu.getSh4Executor()->Start();
		return true;
	}
	std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
	if (ggpoSession == nullptr)
		return false;
	// will call save_game_state
	GGPOErrorCode error = ggpo_advance_frame(ggpoSession);

	// may rollback
	if (error == GGPO_OK)
		error = ggpo_idle(ggpoSession, 0);
	// Deadcast: a settled frame's hash to the other Brick, and compared once both are in
	if (error == GGPO_OK && checkToSend >= 0)
	{
		GameEvent event;
		event.type = GameEvent::Checksum;
		event.u.checksum.frame = checkToSend;
		{
			std::lock_guard<std::mutex> lock(checkMutex);
			auto own = ownHashes.find(checkToSend);
			event.u.checksum.hash = own != ownHashes.end() ? own->second : 0;
		}
		checkToSend = -1;
		ggpo_send_message(ggpoSession, &event, sizeof(event.type) + sizeof(event.u.checksum), true);
		std::lock_guard<std::mutex> lock(checkMutex);
		compareHashes();
	}
	if (error != GGPO_OK)
	{
		stopSession();
		if (error == GGPO_ERRORCODE_INPUT_SIZE_DIFF)
			throw FlycastException("GGPO analog settings are different from peer");
		else
			throw FlycastException("GGPO error");
	}

	// may call save_game_state
	do {
		if (!config::ThreadedRendering)
			os_UpdateInputState();
		Inputs inputs;
		inputs.kcode = ~kcode[0];
		if (rt[0] >= 0x4000)
			inputs.kcode |= BTN_TRIGGER_RIGHT;
		else
			inputs.kcode &= ~BTN_TRIGGER_RIGHT;
		if (lt[0] >= 0x4000)
			inputs.kcode |= BTN_TRIGGER_LEFT;
		else
			inputs.kcode &= ~BTN_TRIGGER_LEFT;
		inputs.mouseButtons = 0;
		inputs.kbModifiers = 0;
		if (analogAxes > 0)
		{
			inputs.u.analog.x = joyx[0] >> 8;
			if (analogAxes >= 2)
				inputs.u.analog.y = joyy[0] >> 8;
		}
		else if (absPointerPos)
		{
			inputs.u.absPos.x = mo_x_abs[0];
			inputs.u.absPos.y = mo_y_abs[0];
		}
		else if (keyboardGame)
		{
			inputs.kbModifiers = kb_shift[0];
			memcpy(inputs.u.keys, kb_key[0], sizeof(kb_key[0]));
		}
		else if (mouseGame)
		{
			std::lock_guard<std::mutex> lock(relPosMutex);
			inputs.mouseButtons = ~mo_buttons[0];
			inputs.u.relPos.x = std::round(mo_x_delta[0]);
			inputs.u.relPos.y = std::round(mo_y_delta[0]);
			inputs.u.relPos.wheel = std::round(mo_wheel_delta[0]);
			mo_x_delta[0] -= inputs.u.relPos.x;
			mo_y_delta[0] -= inputs.u.relPos.y;
			mo_wheel_delta[0] -= inputs.u.relPos.wheel;
		}
		error = ggpo_add_local_input(ggpoSession, localPlayer, &inputs, inputSize);
		if (error == GGPO_OK)
			break;
		if (error != GGPO_ERRORCODE_PREDICTION_THRESHOLD)
		{
			WARN_LOG(NETWORK, "ggpo_add_local_input failed %d", error);
			stopSession();
			throw FlycastException("GGPO error");
		}
		DEBUG_LOG(NETWORK, "ggpo_add_local_input prediction barrier reached");
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		error = ggpo_idle(ggpoSession, 0);
		if (error != GGPO_OK)
		{
			stopSession();
			throw FlycastException("GGPO error");
		}
	} while (active());
#ifdef SYNC_TEST
	u32 input = ~kcode[1 - localPlayerNum];
	GGPOErrorCode result = ggpo_add_local_input(ggpoSession, remotePlayer, &input, sizeof(input));
	if (result != GGPO_OK)
		WARN_LOG(NETWORK, "ggpo_add_local_input(2) failed %d", result);
#endif
	if (active()) {
		emu.getSh4Executor()->Start();
		return true;
	}
	return false;
}

bool active()
{
	return ggpoSession != nullptr || lockstep::running;
}

std::future<bool> startNetwork()
{
	synchronized = false;
	return std::async(std::launch::async, []{
		{
			ThreadName _("GGPO-start");
			std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
#ifdef SYNC_TEST
			startSession(0, 0);
#else
			if (config::EnableUPnP)
			{
				miniupnp.Init();
				miniupnp.AddPortMapping(SERVER_PORT, false);
			}

			try {
				if (config::ActAsServer)
					startSession(SERVER_PORT, 0);
				else
					// Use SERVER_PORT-1 as local port if connecting to ourselves
					startSession(config::NetworkServer.get().empty() || config::NetworkServer.get() == "127.0.0.1" ? SERVER_PORT - 1 : SERVER_PORT, 1);
			} catch (...) {
				miniupnp.Term();
				throw;
			}
#endif
		}
		// Deadcast: lockstep's handshake (its own; nothing of GGPO's to wait for)
		if (lockstep::running)
		{
			lockstep::handshake();
			synchronized = lockstep::running;
		}
		while (!synchronized && active())
		{
			{
				std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
				if (ggpoSession == nullptr)
					break;
				GGPOErrorCode result = ggpo_idle(ggpoSession, 0);
				if (result == GGPO_ERRORCODE_VERIFICATION_ERROR)
					throw FlycastException("Peer verification failed");
				else if (result != GGPO_OK)
				{
					WARN_LOG(NETWORK, "ggpo_idle failed %d", result);
					throw FlycastException("GGPO error");
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
#ifdef SYNC_TEST
		// save initial state (frame 0)
		if (active())
		{
			MapleInputState state[4];
			getInput(state);
		}
#endif
		if (active() && (settings.content.gameId == "VIRTUA FIGHTER 4 JAPAN"
				|| settings.content.gameId == "VF4 EVOLUTION JAPAN"
				|| settings.content.gameId == "VF4 FINAL TUNED JAPAN"))
		{
			// Send the local P1 card
			const u8 *cardData = getRfidCardData(0);
			if (cardData != nullptr)
			{
				GameEvent event;
				event.type = GameEvent::VF4Card;
				event.u.card.playerNum = config::ActAsServer ? 0 : 1;
				memcpy(event.u.card.data, cardData, sizeof(event.u.card.data));
				if (ggpoSession != nullptr)
					ggpo_send_message(ggpoSession, &event, sizeof(event.type) + sizeof(event.u.card), true);
			}
		}
		emu.setNetworkState(active());
		return active();
	});
}

void displayStats()
{
	if (!active() || ggpoSession == nullptr)
		return;
	GGPONetworkStats stats;
	ggpo_get_network_stats(ggpoSession, remotePlayer, &stats);

	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::SetNextWindowPos(ImVec2(10, 10));
	ImGui::SetNextWindowSize(ScaledVec2(95, 0));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("##ggpostats", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
	ImguiStyleColor _2(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));

	// Send Queue
	ImGui::Text("Send Q");
	ImGui::ProgressBar(stats.network.send_queue_len / 10.f, ImVec2(-1, uiScaled(10.f)), "");

	// Frame Delay
	ImGui::Text("Delay");
	std::string delay = std::to_string(config::GGPODelay.get());
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(delay.c_str()).x);
	ImGui::Text("%s", delay.c_str());

	// Ping
	ImGui::Text("Ping");
	std::string ping = std::to_string(stats.network.ping);
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ping.c_str()).x);
	ImGui::Text("%s", ping.c_str());

	// Predicted Frames
	if (stats.sync.predicted_frames >= 7)
		// red
	    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1, 0, 0, 1));
	else if (stats.sync.predicted_frames >= 5)
		// yellow
	    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(.9f, .9f, .1f, 1));
	ImGui::Text("Predicted");
	ImGui::ProgressBar(stats.sync.predicted_frames / 7.f, ImVec2(-1, uiScaled(10.f)), "");
	if (stats.sync.predicted_frames >= 5)
		ImGui::PopStyleColor();

	// Frames behind
	int timesync = timesyncOccurred;
	if (timesync > 0)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
	ImGui::Text("Behind");
	ImGui::ProgressBar(0.5f + stats.timesync.local_frames_behind / 16.f, ImVec2(-1, uiScaled(10.f)), "");
	if (timesync > 0)
	{
		ImGui::PopStyleColor();
		timesyncOccurred--;
	}

	ImGui::End();
}

void endOfFrame()
{
	if (active())
	{
		_endOfFrame = true;
		emu.getSh4Executor()->Stop();
	}
}

void sendChatMessage(int playerNum, const std::string& msg) {
	if (!active() || ggpoSession == nullptr)
		return;
	GameEvent event;
	event.type = GameEvent::Chat;
	event.u.chat.playerNum = playerNum;
	size_t msgLen = std::min(msg.length(), sizeof(event.u.chat.message));
	memcpy(event.u.chat.message, msg.c_str(), msgLen);
	ggpo_send_message(ggpoSession, &event, sizeof(GameEvent) - sizeof(event.u.chat.message) + msgLen, true);
}

void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg))
{
	chatCallback = callback;
}

}

#else // LIBRETRO

namespace ggpo
{

void stopSession() {
}

void getInput(MapleInputState inputState[4])
{
	getLocalInput(inputState);
}

bool nextFrame() {
	return true;
}

bool active() {
	return false;
}

std::future<bool> startNetwork() {
	return std::async(std::launch::deferred, []{ return false; });
}

void displayStats() {
}

void endOfFrame() {
}

void sendChatMessage(int playerNum, const std::string& msg) {
}

void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg)) {
}

}
#endif
