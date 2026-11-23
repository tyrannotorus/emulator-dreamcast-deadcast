// Deadcast's settings (rows.h). Not listed: the TV standard and the cable, kept NTSC and VGA for the Brick's screen.
#include "rows.h"
#include "types.h"

#include <utility>
#include <vector>

namespace deadcast::rows
{
namespace
{

// each row's option, by the row's key: the option's own name string, so the same pointer in every copy of the row
std::vector<std::pair<const char *, Bound>>& bound()
{
	static std::vector<std::pair<const char *, Bound>> list;
	return list;
}

deadui::options::Setting row(config::Option<bool>& o, const char *name, const char *desc)
{
	bound().push_back({ o.getName().c_str(), { &o, nullptr } });
	return { o.getSection().c_str(), o.getName().c_str(), name, desc, { { "Off", 0 }, { "On", 1 } }, true,
			o.getDefault() ? 1 : 0 };
}

deadui::options::Setting row(config::Option<int>& o, const char *name, const char *desc,
		std::vector<std::pair<const char *, int>> choices)
{
	bound().push_back({ o.getName().c_str(), { nullptr, &o } });
	return { o.getSection().c_str(), o.getName().c_str(), name, desc, std::move(choices), false, o.getDefault() };
}

}

Bound option(const deadui::options::Setting& setting)
{
	for (const auto& [key, b] : bound())
		if (key == setting.key)
			return b;
	die("settings: a row with no option");
}

}

namespace deadui::options
{

using deadcast::rows::row;

const std::vector<Page>& pages()
{
	static const std::vector<Page> list {
		{ "Console", "Dreamcast settings.", {
			row(config::Region, "Region", "Applies on restart.",
					{ { "Japan", 0 }, { "USA", 1 }, { "Europe", 2 }, { "Default", 3 } }),
			row(config::Language, "Language", "Applies on restart.",
					{ { "Japanese", 0 }, { "English", 1 }, { "German", 2 }, { "French", 3 }, { "Spanish", 4 },
					  { "Italian", 5 }, { "Default", 6 } }),
			row(config::VmuSound, "VMU Sounds", "VMU beeps."),
		} },
		{ "Emulator", "Deadcast settings.", {
			row(config::ShowFPS, "Show FPS", "Frame rate overlay."),
			row(config::TextureFiltering, "Texture Filter", "Texture smoothing.",
					{ { "Default", 0 }, { "Nearest", 1 }, { "Linear", 2 } }),
			row(config::DSPEnabled, "Audio DSP", "Sound effects such as reverb. Slower."),
			row(config::ThreadedAudio, "Threaded Audio", "Turning off can fix some games."),
		} },
	};
	return list;
}

}
