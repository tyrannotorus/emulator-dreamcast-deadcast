// Two players on two Bricks: Flycast's netplay hooks, run as lockstep (network/ggpo.cpp), over the link's
// IP (link.h: USB, Bluetooth). Once linked, both Bricks restart the game from its start as two players,
// the link's leader as player 1: nothing of either Brick's game is sent, so both start alike from the
// disc. As on a Dreamcast with two controllers, each player brings a VMU: player 1's in port A1, player
// 2's in B1 on both Bricks (link.h swaps them), each kept in its owner's file by its owner's Brick; the
// console's flash is its defaults, in memory only. When the session ends (this player leaves, or the
// other Brick's left or quit), the game goes on as it is with this Brick's player alone, as if the other
// had unplugged their controller.
// For the rest of the run (twoPlayerRun) no state is saved: one would carry the other player's VMU into a
// later game, which would write it over this player's own.
#pragma once
#include "types.h"

#include <string>
#include <vector>

namespace deadcast::netplay
{

// The session's settings (transient: kept over the game's restart, never saved); the game is to be
// restarted then. host: this Brick serves (player 1); peer: the other Brick's address.
void begin(bool host, const std::string& peer, int inputDelay);
bool inSession();
// The session left: the game goes on as it is, this player alone (the other Brick told)
void leave();
// A two-player game started in this run: its VMUs and flash (vmu()), no states saved
bool twoPlayerRun();
// In a twoPlayerRun, the VMU in a port (A1, B1, ...): this Brick's player's (Own: its file, port A1's
// when it plays alone), the other player's (Peer: peerVmu(), in memory only), or a blank one (in memory)
enum class Vmu { Own, Peer, Blank };
Vmu vmu(const std::string& port);
const std::vector<u8>& peerVmu();
// MENU in a session: both Bricks pause before the same frame and show the session's menu
void requestPause();
// Paused (MENU here or there, the other Brick silent): why, and whether the other Brick is there
enum class Pause { None, Menu, PeerMenu, Lost };
struct PauseState
{
	Pause pause = Pause::None;
	bool peerPresent = false;
};
PauseState pauseState();
// Continue: both go on (when the other Brick is there)
void resume();

}
