// Deadcast's settings, as Options shows them (ui/src/options.h): built into the shared UI as DEADUI_OPTIONS_SOURCE. Not
// listed: the TV standard and the cable, kept NTSC and VGA for the Brick's screen.
#include "options.h"

namespace deadui::options
{

const std::vector<Page>& pages()
{
	static const std::vector<Page> list {
		{ "Console", "Dreamcast settings.", {
			{ "config", "Dreamcast.Region", "Region", "Applies on restart.",
					{ { "Japan", 0 }, { "USA", 1 }, { "Europe", 2 }, { "Default", 3 } }, false, 1 },
			{ "config", "Dreamcast.Language", "Language", "Applies on restart.",
					{ { "Japanese", 0 }, { "English", 1 }, { "German", 2 }, { "French", 3 }, { "Spanish", 4 },
					  { "Italian", 5 }, { "Default", 6 } }, false, 1 },
			{ "audio", "VmuSound", "VMU Sounds", "VMU beeps.", { { "Off", 0 }, { "On", 1 } }, true, 0 },
		} },
		{ "Emulator", "Deadcast settings.", {
			{ "config", "rend.ShowFPS", "Show FPS", "Frame rate overlay.", { { "Off", 0 }, { "On", 1 } }, true, 0 },
			{ "config", "rend.TextureFiltering", "Texture Filter", "Texture smoothing.",
					{ { "Default", 0 }, { "Nearest", 1 }, { "Linear", 2 } }, false, 0 },
			{ "config", "aica.DSPEnabled", "Audio DSP", "Sound effects such as reverb. Slower.",
					{ { "Off", 0 }, { "On", 1 } }, true, 0 },
			{ "config", "aica.ThreadedAudio", "Threaded Audio", "Turning off can fix some games.",
					{ { "Off", 0 }, { "On", 1 } }, true, 1 },
		} },
	};
	return list;
}

}
