// Emulator Options' settings: one list, shown the same in Deadcast's emulator and in a recomp module.
// The emulator shows and changes its own settings as it plays; a module keeps them in emu.cfg, the
// emulator's file, for the emulator's next start (the two never run at once). Not listed: the TV
// standard and the cable, NTSC and VGA for the Brick's screen (core/emulator.cpp; TV Composite for a
// game that refuses VGA).
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace deadcast::options
{

// a setting as emu.cfg keeps it, [section] key = value: a number, or yes/no for a flag
struct Setting
{
	const char *section;
	const char *key;
	const char *name;
	const char *desc;
	std::vector<std::pair<const char *, int>> choices;	// label, value
	bool flag;
	int fallback;		// the emulator's default, for a file without it
};

// Console, Video, Audio
struct Page
{
	const char *name;
	const char *desc;
	std::vector<Setting> settings;
};
const std::vector<Page>& pages();

// where the values are: the emulator's settings, or emu.cfg
struct Store
{
	std::function<int(const Setting&)> get;
	std::function<void(const Setting&, int)> set;	// kept at once
};
// emu.cfg in Deadcast's home (recomp::home()), read and written as the emulator reads it
Store file();

}
