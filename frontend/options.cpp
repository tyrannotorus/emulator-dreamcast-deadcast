#include "options.h"
#include "recomp.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <strings.h>
#include <unistd.h>

namespace deadcast::options
{
namespace
{

std::string trim(const std::string& s)
{
	const size_t a = s.find_first_not_of(" \t\r");
	if (a == std::string::npos)
		return "";
	return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

std::string path()
{
	return recomp::home() + "/emu.cfg";
}

std::vector<std::string> read()
{
	std::vector<std::string> lines;
	std::ifstream in(path());
	for (std::string line; std::getline(in, line); )
		lines.push_back(line);
	return lines;
}

// where a setting is in the file: its line, else the line after its section's last (-1: no section)
struct Place
{
	int line = -1;
	int end = -1;
};

Place find(const std::vector<std::string>& lines, const Setting& setting)
{
	Place place;
	bool in = false;
	for (int i = 0; i < (int)lines.size(); i++)
	{
		const std::string t = trim(lines[i]);
		if (t.empty() || t[0] == ';')
			continue;
		if (t[0] == '[')
		{
			const size_t close = t.find(']');
			in = close != std::string::npos && t.substr(1, close - 1) == setting.section;
			if (in)
				place.end = i + 1;
			continue;
		}
		if (!in)
			continue;
		place.end = i + 1;
		const size_t eq = t.find('=');
		if (eq != std::string::npos && trim(t.substr(0, eq)) == setting.key)
			place.line = i;
	}
	return place;
}

int get(const Setting& setting)
{
	const std::vector<std::string> lines = read();
	const Place place = find(lines, setting);
	if (place.line < 0)
		return setting.fallback;
	const std::string& line = lines[place.line];
	const std::string value = trim(line.substr(line.find('=') + 1));
	if (setting.flag)
		return strcasecmp(value.c_str(), "yes") == 0 || strcasecmp(value.c_str(), "true") == 0
				|| strcasecmp(value.c_str(), "on") == 0 || value == "1";
	// as the emulator reads a number: hexadecimal after 0x, else decimal
	const bool hex = value.size() > 1 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
	return (int)std::strtoul(value.c_str(), nullptr, hex ? 16 : 10);
}

// the file again, the setting's line changed or added; written beside it, then in its place
void set(const Setting& setting, int value)
{
	std::vector<std::string> lines = read();
	const std::string line = std::string(setting.key) + " = "
			+ (setting.flag ? (value != 0 ? "yes" : "no") : std::to_string(value));
	const Place place = find(lines, setting);
	if (place.line >= 0)
		lines[place.line] = line;
	else if (place.end >= 0)
		lines.insert(lines.begin() + place.end, line);
	else
	{
		if (!lines.empty() && !trim(lines.back()).empty())
			lines.push_back("");
		lines.push_back(std::string("[") + setting.section + "]");
		lines.push_back(line);
	}
	const std::string temp = path() + ".new";
	FILE *out = std::fopen(temp.c_str(), "w");
	if (out == nullptr)
	{
		std::printf("Deadcast: %s cannot be written\n", temp.c_str());
		return;
	}
	for (const std::string& l : lines)
		std::fprintf(out, "%s\n", l.c_str());
	const bool written = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
	std::fclose(out);
	if (!written || std::rename(temp.c_str(), path().c_str()) != 0)
	{
		std::remove(temp.c_str());
		std::printf("Deadcast: %s cannot be written\n", path().c_str());
	}
}

}	// namespace

const std::vector<Page>& pages()
{
	static const std::vector<Page> list {
		{ "Console", "The Dreamcast's region and language.", {
			{ "config", "Dreamcast.Region", "Region", "The console's region. Takes effect on restart.",
					{ { "Japan", 0 }, { "USA", 1 }, { "Europe", 2 }, { "Default", 3 } }, false, 1 },
			{ "config", "Dreamcast.Language", "Language", "The console's language. Takes effect on restart.",
					{ { "Japanese", 0 }, { "English", 1 }, { "German", 2 }, { "French", 3 }, { "Spanish", 4 },
					  { "Italian", 5 }, { "Default", 6 } }, false, 1 },
		} },
		{ "Video", "", {
			{ "config", "rend.ShowFPS", "Show FPS", "Shows the frame rate.", { { "Off", 0 }, { "On", 1 } }, true, 0 },
			{ "config", "pvr.AutoSkipFrame", "Frame Skip", "Skips drawing frames when the game falls behind.",
					{ { "Off", 0 }, { "Normal", 1 }, { "Maximum", 2 } }, false, 0 },
			{ "config", "rend.TextureFiltering", "Texture Filter", "How the game's textures are smoothed.",
					{ { "Default", 0 }, { "Nearest", 1 }, { "Linear", 2 } }, false, 0 },
		} },
		{ "Audio", "", {
			{ "config", "aica.DSPEnabled", "Audio DSP", "The sound chip's effects, such as reverb. Costs speed.",
					{ { "Off", 0 }, { "On", 1 } }, true, 0 },
			{ "audio", "VmuSound", "VMU Sounds", "The VMU's beeps.", { { "Off", 0 }, { "On", 1 } }, true, 0 },
		} },
	};
	return list;
}

Store file()
{
	return { get, set };
}

}
