// The in-game menu (MENU button), styled after NextUI's minarch: Continue, Save, Load, Options, Quit.
// Drawn on the CPU (canvas.h) and shown through the graphics context's pictures. The game is stopped
// while it is open; everything runs on the render thread, where SDL input is read too.
#pragma once
#include "types.h"

#include <string>

namespace deadcast::menu
{

enum class Result { Open, Resume, Quit };

// Opens the menu over the stopped game; false if there is no screen to draw on.
bool open();
bool isOpen();
// Raw pad input (SDL button and axis codes) while the menu is open.
void button(u32 code, bool pressed);
void axis(u32 code, int value);
// Called every render loop iteration: repeats held directions and redraws when needed.
Result frame();

// The start page, "Loading..." over the disc's file name, up from launch until the game's first frame.
// showStart returns false if there is no screen for it; showStartError shows a failure for a moment.
bool showStart();
void showStartError(const std::string& what);
void endStart();
// Instead of the start page: which BIOS file is missing and where it goes, until A or B (then
// frame() returns Quit). false without a screen for it.
bool showBiosMissing(const std::string& file);

}
