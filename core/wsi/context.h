/*
    Created on: Oct 18, 2019

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
#pragma once
#include "emulator.h"
#include <string>
#include <vector>
#include <cassert>

void initRenderApi(void *window = nullptr, void *display = nullptr);
void termRenderApi();
void switchRenderApi();

class GraphicsContext
{
public:
	virtual ~GraphicsContext();
	virtual void resize() {}
	virtual std::string getDriverName() = 0;
	virtual std::string getDriverVersion() = 0;
	virtual bool isAMD() = 0;
	virtual bool hasPerPixel() { return false; }
	// Set the game swap interval: 1 for 60 FPS, 2 for 30 FPS
	virtual void setSwapInterval(int interval) {}
	// Deadcast: the frame drawn goes to the screen
	virtual void present() {}
	// Deadcast: CPU-drawn pictures (the menu) at the screen's size, shown while no frame is drawn.
	// begin: false without a screen; show: BGRA8, width x height; end: the screen left as is (the game
	// quits or loads again), the game's last frame again (the menu closed), or its next one once drawn
	// (the start page: a game loaded again has its own first frame, not the last one of before)
	enum class AfterPictures { Stay, LastFrame, NextFrame };
	virtual bool beginPictures(int& width, int& height) { return false; }
	virtual void showPicture(const u32 *bgra) {}
	virtual void endPictures(AfterPictures after) {}
	// Deadcast: small pictures on every frame from now on, at its left edge, `bottom` rows above its bottom: the
	// counter and the messages (deadcast/osd.h); BGRA8, width x height. Their box colour (ui::label's) is left out, so
	// that only the text is written and the game shows around it
	struct Corner
	{
		std::vector<u32> pixels;
		int width = 0;
		int height = 0;
		int bottom = 0;
	};
	virtual void setCorners(std::vector<Corner> corners) {}
	virtual int frameWidth() const { return 0; }
	// frames that have reached the screen
	virtual u64 framesShown() { return 0; }
	// the last frame drawn once more, between frames, for what is on it to change on a still screen; present() follows
	virtual bool showLastFrameAgain() { return false; }

	void setWindow(void *window, void *display = nullptr) {
		this->window = window;
		this->display = display;
	}
	void getWindow(void **pwindow, void **pdisplay) const {
		*pwindow = window;
		*pdisplay = display;
	}

	static GraphicsContext *Instance() {
		return instance;
	}
	static void Term()
	{
		if (instance != nullptr) {
			delete instance;
			assert(instance == nullptr);
		}
	}

protected:
	GraphicsContext(void *window, void *display = nullptr);

	void *window = nullptr;
	void *display = nullptr;

private:
	static void onEvent(Event event, void *arg);
	static GraphicsContext *instance;
};
