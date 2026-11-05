// The emulator's side of the menu (MENU button), styled after NextUI's minarch: Continue, Save, Load, Options,
// Recomp Store, Quit.
// Every page of it looks like that first one (its title in a pill, its rows, the buttons at the bottom
// right) over the paused game blurred; a page opened slides its rows in from the right, the ones before
// them out to the left (the other way back), while the pills stay in place. Drawn on the CPU by the
// shared UI library (ui/src/ui.h), shown through the graphics context's pictures.
// The game is stopped while it is open; everything runs on the render thread, where SDL input is read too.
#pragma once
#include "types.h"

#include <string>

namespace deadcast::menu
{

// Restart: the game again from the start of its load (linked to another Brick: as two players,
// deadcast/netplay.h). Switch: the game again in the other core (Options): Deadcast ends and the
// launcher starts it again (switching()).
enum class Result { Open, Resume, Quit, Restart, Switch };

// Opens the menu over the stopped game; false if there is no screen to draw on.
bool open();
// The menu of a paused two-player session (deadcast/netplay.h): Continue, Leave two-player game, Quit,
// why it is paused on top. The emulator is not stopped (the session waits); Continue there closes it here.
bool openSession();
// In a recomp module's host, whose menu is open with no emulator: Controls, with the pad open for the page's time.
void openHostedControls();

bool isOpen();
// Options switched the game's core: Deadcast ends with recomp::SwitchExit (ui/src/recomp.h)
bool switching();
// Raw pad input (SDL button and axis codes) while the menu is open.
void button(u32 code, bool pressed);
void axis(u32 code, int value);
// Called every render loop iteration: repeats held directions and redraws when needed.
Result frame();
// a picture went to the display since the last call (the menu moving): the loop goes straight on, the flip pacing it
bool shown();

// The start page, "Loading..." over the disc's file name, up from launch until the game's first frame.
// showStart returns false if there is no screen for it; showStartError shows a failure for a moment.
bool showStart();
void showStartError(const std::string& what);
// In a two-player game (deadcast/netplay.h), loaded: the start page says so until the other Brick is
// there too (then endStart()); false if no start page is up.
bool showStartTwoPlayers();
void endStart();
// Instead of the start page: which BIOS file is missing and where it goes, until A or B (then
// frame() returns Quit). false without a screen for it.
bool showBiosMissing(const std::string& file);

}
