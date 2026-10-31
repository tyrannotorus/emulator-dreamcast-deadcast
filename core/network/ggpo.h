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
#pragma once
#include <future>
#include <string>

struct MapleInputState;

namespace ggpo
{

std::future<bool> startNetwork();
void startSession(int localPort, int localPlayerNum);
void stopSession();
void getInput(MapleInputState inputState[4]);
bool nextFrame();
bool active();
void displayStats();
void endOfFrame();
void sendChatMessage(int playerNum, const std::string& msg);
void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg));

// Deadcast: the lockstep session's pause (MENU on either Brick; the other Brick silent or gone), for the menu
enum class PauseReason { None, Menu, PeerMenu, Lost, Left };
struct NetplayPause
{
	bool paused = false;
	PauseReason reason = PauseReason::None;
	bool peerPresent = false;
};
NetplayPause netplayPause();
void netplayRequestPause();
void netplayResume();
int sessionSide();
// after a session: the local pad on the side it played (1: player 2's port), else 0
void setAloneSide(int side);

static inline bool rollbacking() {
	extern bool inRollback;

	return inRollback;
}

}
