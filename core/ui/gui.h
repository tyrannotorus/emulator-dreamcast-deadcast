/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"

#include <functional>
#include <string>

// Deadcast: what stands between the emulator and its menu: which of the menu's states the game is in, starting a game,
// save states and screenshots. Everything drawn is the menu's (deadcast/menu.h) or the on-screen text (deadcast/osd.h).
void gui_init();
void gui_term();
void gui_open_settings();
// a paused two-player session's menu, once paused (called every main loop iteration)
void gui_netplay_check();
// while gui_is_open(): the start page, or the menu
void gui_display_ui();
// once for each frame drawn, on the render thread
void gui_frame();
void gui_cancel_load();

void gui_stop_game(const std::string& message = "");
void gui_start_game(const std::string& path);
void gui_error(const std::string& what);

void gui_loadState(bool inRam = false);
void gui_saveState(bool stopRestart = true, bool inRam = false);
void gui_cycleSaveStateSlot(int step);
void gui_takeScreenshot();
void gui_saveFrame(const std::string& path);
void gui_runOnUiThread(std::function<void()> function);

enum class GuiState {
	Closed,		// the game plays
	Starting,	// nothing loaded yet
	Loading,
	NetworkStart,
	Menu,
};
extern GuiState gui_state;
void gui_setState(GuiState newState);
static inline bool gui_is_open()
{
	return gui_state != GuiState::Closed;
}
