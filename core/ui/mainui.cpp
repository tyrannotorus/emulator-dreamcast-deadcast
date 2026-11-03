/*
	Copyright 2020 flyinghead

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

#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "profiler/fc_profiler.h"
#include "oslib/i18n.h"
#include "hw/pvr/spg.h"
#include "deadcast/profiler.h"

#include <chrono>
#include <thread>

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	os_UpdateInputState();
	// Deadcast: a two-player session paused: its menu
	gui_netplay_check();

	if (gui_is_open())
	{
		// Deadcast: a frame every 16 ms, however long it took to draw (the menu's pages slide)
		const auto frameStart = std::chrono::steady_clock::now();
		try {
			gui_display_ui();
		} catch (const FlycastException& e) {
			// Assume this is a graphics API issue
			forceReinit = true;
			return false;
		}
#ifndef TARGET_IPHONE
		std::this_thread::sleep_until(frameStart + std::chrono::milliseconds(16));
#endif
	}
	else
	{
		try {
			if (!emu.render())
				return false;
		} catch (const RendererException& e) {
			gui_error(std::string("Renderer error: ") + e.what());
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			return false;
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;
	perfPresented();

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error("Renderer initialization failed");
	}
}

void mainui_term()
{
	rend_term_renderer();
}

void mainui_loop(bool forceStart)
{
	ThreadName _("deadcast-rend");
	profilerStartThread("deadcast-rend");
	if (forceStart)
		mainui_enabled = true;
	mainui_init();
	RenderType currentRenderer = config::RendererType;

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");

		const bool rendered = mainui_rend_frame();
		GraphicsContext *context = GraphicsContext::Instance();
		if (context == nullptr)
			forceReinit = true;
		else if (rendered)
		{
			try {
				context->present();
			} catch (const FlycastException& e) {
				forceReinit = true;
			}
		}

		if (config::RendererType != currentRenderer || forceReinit)
		{
			mainui_term();
			// Deadcast: Vulkan is the only API: recreated only after a failure
			if (forceReinit)
				switchRenderApi();
			mainui_init();
			forceReinit = false;
			currentRenderer = config::RendererType;
		}

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	mainui_term();
}

void mainui_start()
{
	mainui_enabled = true;
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}
