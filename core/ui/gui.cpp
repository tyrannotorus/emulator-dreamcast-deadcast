/*
	Copyright 2019 flyinghead

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
#include "gui.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "input/gamepad_device.h"
#include "network/ggpo.h"
#include "network/net_handshake.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include "rend/CustomTexture.h"
#include "rend/osd.h"
#include "stdclass.h"
#include "deadcast/menu.h"
#include "deadcast/netplay.h"
#include "deadcast/osd.h"

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <future>
#include <mutex>

using namespace i18n;

static bool game_started;
GuiState gui_state = GuiState::Starting;

static std::recursive_mutex guiMutex;
using LockGuard = std::lock_guard<std::recursive_mutex>;
static ThreadRunner uiThreadRunner;
static std::future<bool> networkStatus;

// the game loading on a thread of its own
class BackgroundGameLoader
{
public:
	void load(const std::string& path)
	{
		progress.reset();
		future = std::async(std::launch::async, [this, path] {
			ThreadName _("GameLoader");
			emu.loadGame(path.c_str(), &progress);
		});
	}

	void cancel()
	{
		if (progress.cancelled)
			return;
		progress.cancelled = true;
		if (future.valid())
			try {
				future.get();
			} catch (const FlycastException&) {
			}
		emu.unloadGame();
		gui_setState(GuiState::Starting);
	}

	bool ready()
	{
		if (!future.valid())
			return true;
		if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			future.get();
			return true;
		}
		return false;
	}

private:
	LoadProgress progress;
	std::future<void> future;
};
static BackgroundGameLoader gameLoader;

static void emuEventCallback(Event event, void *)
{
	switch (event)
	{
	case Event::Resume:
		game_started = true;
		break;
	case Event::Start:
		GamepadDevice::load_system_mappings();
		break;
	case Event::Terminate:
		GamepadDevice::load_system_mappings();
		game_started = false;
		break;
	default:
		break;
	}
}

void gui_init()
{
	uiThreadRunner.init();
	EventManager::listen(Event::Resume, emuEventCallback);
	EventManager::listen(Event::Start, emuEventCallback);
	EventManager::listen(Event::Terminate, emuEventCallback);
}

void gui_term()
{
	EventManager::unlisten(Event::Resume, emuEventCallback);
	EventManager::unlisten(Event::Start, emuEventCallback);
	EventManager::unlisten(Event::Terminate, emuEventCallback);
}

void gui_setState(GuiState newState)
{
	gui_state = newState;
}

void gui_open_settings()
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && !settings.naomi.slave)
	{
		if (ggpo::active())
		{
			// MENU in a two-player game pauses both Bricks (the menu opens once they have stopped: gui_netplay_check)
			if (deadcast::netplay::inSession())
				deadcast::netplay::requestPause();
			return;
		}
		try {
			emu.stop();
			if (deadcast::menu::open())
				gui_setState(GuiState::Menu);
			else
			{
				// no screen or no font: said, and nothing takes its place
				ERROR_LOG(COMMON, "Deadcast: the menu cannot be opened: the game goes on");
				emu.start();
				os_notify("The menu could not be opened", 5000);
			}
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
	else if (gui_state == GuiState::Loading)
	{
		gameLoader.cancel();
		dc_exit();
	}
}

static void reset_vmus()
{
	for (u32 i = 0; i < std::size(vmu_lcd_status); i++)
		vmu_lcd_status[i] = false;
}

void gui_start_game(const std::string& path)
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Starting && gui_state != GuiState::Closed)
		return;
	emu.unloadGame();
	reset_vmus();
	gui_setState(GuiState::Loading);
	gameLoader.load(path);
}

// a two-player session paused (MENU here or on the other Brick, the other Brick silent):
// its menu, over the game stopped where both stopped (the emulator is not stopped: the session waits)
void gui_netplay_check()
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Closed || !deadcast::netplay::inSession())
		return;
	if (deadcast::netplay::pauseState().pause != deadcast::netplay::Pause::None && deadcast::menu::openSession())
		gui_setState(GuiState::Menu);
}

// the game cannot go on: said in the log, and Deadcast ends
void gui_stop_game(const std::string& message)
{
	const LockGuard lock(guiMutex);
	if (!message.empty())
		ERROR_LOG(COMMON, "Deadcast has stopped: %s", message.c_str());
	dc_exit();
}

void gui_error(const std::string& what)
{
	ERROR_LOG(COMMON, "Deadcast: %s", what.c_str());
	os_notify(what.c_str(), 8000);
}

void gui_cancel_load()
{
	gameLoader.cancel();
}

void os_notify(const char *msg, int durationMs, const char *details)
{
	deadcast::osd::notify(msg, durationMs, details != nullptr ? details : "");
}

void fatal_error(const char* text, ...)
{
	va_list args;
	char temp[2048];
	va_start(args, text);
	vsnprintf(temp, sizeof(temp), text, args);
	va_end(args);
	ERROR_LOG(COMMON, "%s", temp);
	os_notify("Fatal Error", 20000, temp);
}

// the start page cannot be shown: the game loads all the same, and the log says so once
static void startPageMissing()
{
	static bool said = false;
	if (!said)
		ERROR_LOG(COMMON, "Deadcast: the start page cannot be shown: no screen or no font");
	said = true;
}

void gui_display_ui()
{
	const LockGuard lock(guiMutex);
	uiThreadRunner.execTasks();

	switch (gui_state)
	{
	case GuiState::Closed:
		break;

	case GuiState::Starting:
		if (settings.content.path.empty() && !settings.naomi.slave)
		{
			ERROR_LOG(COMMON, "Deadcast has no disc to start");
			dc_exit();
		}
		else if (settings.content.path.substr(0, 7) == "dc_bios")
			gui_start_game("");
		else
			gui_start_game(settings.content.path);
		break;

	case GuiState::Loading:
		if (!deadcast::menu::showStart())
			startPageMissing();
		try {
			if (gameLoader.ready() && !custom_texture.isPreloading())
			{
				if (NetworkHandshake::instance != nullptr)
				{
					// the start page stays up, two players starting, until the other Brick is there too
					deadcast::menu::showStartTwoPlayers();
					networkStatus = NetworkHandshake::instance->start();
					gui_setState(GuiState::NetworkStart);
				}
				else
				{
					deadcast::menu::endStart();
					gui_setState(GuiState::Closed);
					emu.start();
				}
			}
		} catch (const BiosMissingException& ex) {
			ERROR_LOG(BOOT, "%s", ex.what());
			if (deadcast::menu::showBiosMissing(ex.file))
				gui_setState(GuiState::Menu);
			else
			{
				deadcast::menu::endStart();
				gui_stop_game(ex.what());
			}
		} catch (const FlycastException& ex) {
			ERROR_LOG(BOOT, "%s", ex.what());
			deadcast::menu::showStartError(ex.what());
			deadcast::menu::endStart();
			gui_stop_game(ex.what());
		}
		break;

	case GuiState::NetworkStart:
		// two players: the start page up until the other Brick is there too
		if (!deadcast::menu::showStartTwoPlayers())
			startPageMissing();
		if (networkStatus.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
			break;
		deadcast::menu::endStart();
		try {
			if (networkStatus.get())
			{
				gui_setState(GuiState::Closed);
				emu.start();
			}
			else
				gui_stop_game();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
		break;

	case GuiState::Menu:
		switch (deadcast::menu::frame())
		{
		case deadcast::menu::Result::Resume:
			gui_setState(GuiState::Closed);
			emu.start();
			break;
		case deadcast::menu::Result::Quit:
		// the other core chosen: this process ends, the launcher starts the game again in it
		case deadcast::menu::Result::Switch:
			dc_exit();
			break;
		case deadcast::menu::Result::Restart:
		{
			// the game again, as two players (USB Link's Play, deadcast/netplay.h); the path a
			// copy, as unloading the game clears settings.content
			const std::string path = settings.content.path;
			gui_setState(GuiState::Closed);
			gui_start_game(path);
			break;
		}
		default:
			break;
		}
		break;
	}
}

bool gui_frame(bool rendered)
{
	uiThreadRunner.execTasks();
	return deadcast::osd::frame(rendered);
}

void gui_runOnUiThread(std::function<void()> function)
{
	uiThreadRunner.runOnThread(function);
}

static void appendVectorData(void *context, void *data, int size)
{
	std::vector<u8>& v = *(std::vector<u8> *)context;
	const u8 *bytes = (const u8 *)data;
	v.insert(v.end(), bytes, bytes + size);
}

static void getScreenshot(std::vector<u8>& data, int width = 0)
{
	data.clear();
	std::vector<u8> rawData;
	int height = 0;
	if (renderer == nullptr || !renderer->GetLastFrame(rawData, width, height))
		return;
	stbi_flip_vertically_on_write(0);
	stbi_write_png_to_func(appendVectorData, &data, width, height, 3, &rawData[0], 0);
}

static void savestate()
{
	std::vector<u8> pngData;
	getScreenshot(pngData);	// the screen as shown, at its own size
	dc_savestate(config::SavestateSlot, pngData.empty() ? nullptr : &pngData[0], pngData.size());
}

void gui_loadState(bool inRam)
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && dc_savestateAllowed())
	{
		try {
			emu.stop();
			if (inRam)
				dc_loadstate(-2);  // special slot used for inRam states
			else
				dc_loadstate(config::SavestateSlot);
			emu.start();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
}

void gui_saveState(bool stopRestart, bool inRam)
{
	const LockGuard lock(guiMutex);
	// a state would carry the other player's VMU into a later game (deadcast/netplay.h)
	if (deadcast::netplay::twoPlayerRun() && !inRam)
	{
		os_notify("No save states in a game started as two players. Its own saves work.", 3000);
		return;
	}
	if ((gui_state == GuiState::Closed || !stopRestart) && dc_savestateAllowed())
	{
		try {
			if (stopRestart)
				emu.stop();

			if (inRam)
				dc_savestate(-2);
			else
				savestate();

			if (stopRestart)
				emu.start();
		} catch (const FlycastException& e) {
			if (stopRestart)
				gui_stop_game(e.what());
			else
				WARN_LOG(COMMON, "gui_saveState: %s", e.what());
		}
	}
}

void gui_cycleSaveStateSlot(int step)
{
	config::SavestateSlot = (config::SavestateSlot + 10 + step) % 10;
	SaveSettings();
	os_notify(strprintf(T("Save state slot %d"), config::SavestateSlot + 1).c_str(), 2000);
}

// the last frame rendered, written to path as a binary PPM (DEADCAST_SHOTS in spg.cpp);
// uncompressed to keep the render thread's cost low
void gui_saveFrame(const std::string& path)
{
	gui_runOnUiThread([path]() {
		std::vector<u8> rgb;
		int width = 0;
		int height = 0;
		if (renderer == nullptr || !renderer->GetLastFrame(rgb, width, height))
			return;
		if (FILE *f = fopen(path.c_str(), "wb"))
		{
			fprintf(f, "P6\n%d %d\n255\n", width, height);
			fwrite(rgb.data(), 1, rgb.size(), f);
			fclose(f);
		}
	});
}

void gui_takeScreenshot()
{
	if (!game_started)
		return;
	gui_runOnUiThread([]() {
		std::string date = timeToISO8601(time(nullptr));
		std::replace(date.begin(), date.end(), '/', '-');
		std::replace(date.begin(), date.end(), ':', '-');
		std::string name = "Deadcast-" + date + ".png";

		std::vector<u8> data;
		getScreenshot(data);
		if (data.empty()) {
			os_notify(T("No screenshot available"), 2000);
		}
		else
		{
			try {
				hostfs::saveScreenshot(name, data);
				os_notify(T("Screenshot saved"), 2000, name.c_str());
			} catch (const FlycastException& e) {
				os_notify(T("Error saving screenshot"), 5000, e.what());
			}
		}
	});
}
