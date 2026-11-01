#include "ui.h"
#include "download.h"
#include "recomp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

// Noto Sans Bold (fonts/NotoSans-Bold.ttf, SIL OFL 1.1: fonts/NotoSans-OFL.txt), in the binary
extern "C" const unsigned char deadcast_font[];
extern "C" const unsigned char deadcast_font_end[];
__asm__(".section .rodata\n"
		".global deadcast_font\n"
		".global deadcast_font_end\n"
		".balign 16\n"
		"deadcast_font:\n"
		".incbin \"" DEADCAST_FONT "\"\n"
		"deadcast_font_end:\n"
		".previous\n");

namespace deadcast::ui
{
namespace
{

using u64 = std::uint64_t;

constexpr int MaxRows = 6;			// a page's rows shown at once (arrows: there are more)
constexpr int RowPitch = S(28);		// from one row to the next (its pill, PillSize, a little taller)
constexpr int SlideMs = 150;		// a page's rows sliding in, the ones before them out
constexpr float BackdropBrightness = 0.4f;	// the paused game behind the menu (white: 40% grey)

// NextUI's "MinUI" palette
constexpr u32 ColorMain = 0xffffff;			// the selected row's pill, the buttons' circles
constexpr u32 ColorAccent = 0x262626;		// the pills behind the title, the hints, the selected option
constexpr u32 ColorAccent2 = 0x999999;		// the buttons' letters
constexpr u32 ColorSelectedText = 0x000000;
constexpr u32 ColorHint = 0xffffff;			// the title, the hints
constexpr u32 ColorBackground = 0x000000;

u64 nowMs()
{
	return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct LoadedFonts : Fonts
{
	FT_Library library = nullptr;
};
LoadedFonts *loaded;

bool loadFonts()
{
	if (loaded != nullptr)
		return true;
	auto f = std::make_unique<LoadedFonts>();
	const u8 *data = deadcast_font;
	const size_t size = (size_t)(deadcast_font_end - deadcast_font);
	if (FT_Init_FreeType(&f->library) != 0)
		return false;
	if (!f->large.init(f->library, data, size, S(16)) || !f->medium.init(f->library, data, size, S(14))
			|| !f->small.init(f->library, data, size, S(12)) || !f->tiny.init(f->library, data, size, S(10))
			|| !f->page.init(f->library, data, size, 44) || !f->pageText.init(f->library, data, size, 28))
	{
		std::fprintf(stderr, "Deadcast menu: the font cannot be read\n");
		return false;
	}
	loaded = f.release();
	return true;
}

struct State
{
	Display *display = nullptr;
	bool open = false;
	Canvas canvas;
	Canvas background;				// the game's frame blurred, behind every page
	std::vector<Screen> screens;	// the pages open, the first at the bottom
	// a page opened (sliding in from the right, the one before out to the left) or left (the other way)
	bool sliding = false;
	bool slideBack = false;
	u64 slideStart = 0;
	std::unique_ptr<Screen> leaving;	// the page left, until it is off the screen
	std::string message;			// shown over everything until A or B
	// a question instead: A does yes (its button saying yesHint), B no
	const char *yesHint = nullptr;
	std::function<void()> yes;
	std::function<void()> no;
	bool awaiting = false;			// a Binding row waiting for a control
	// work under way: its page (or text over the page, busyOver) shown, then fn run
	std::function<void()> work;
	std::string workLine;
	std::string workUnder;
	bool workOver = false;
	std::function<void()> hostPage;	// a page of the host's own, over everything (showPage)
	Result result = Result::Open;
	bool dirty = true;
	Button held = Button::None;		// a direction held: repeated
	u64 repeatAt = 0;
	bool network = false;			// a network connection, again every second while open
	u64 networkAt = 0;
	bool starting = false;			// the start page up
	Host host;						// the main page's
	Core chosen = Core::Emulator;	// the core picked in Options: the game's only once the menu is left
	bool hasModule = false;			// a recomp module installed for the disc
	recomp::Module module;
	std::string discSha1;			// the disc's (its CHD header's)
	// a module being downloaded (its page up until it is done)
	std::thread download;
	std::atomic<uint64_t> downloaded { 0 };
	std::atomic<uint64_t> downloadSize { 0 };
	std::atomic<bool> cancelDownload { false };
	std::atomic<bool> downloadDone { false };
	std::atomic<bool> installing { false };		// fetched and checked: being unpacked
	std::string downloadError;
	const recomp::CatalogEntry *downloading = nullptr;
};
State st;

// GSAP's power1.out: fast, then slowing to a stop
float easeOut(float t)
{
	return 1.f - (1.f - t) * (1.f - t);
}

bool checkNetwork()
{
	FILE *f = std::fopen("/proc/net/route", "r");
	if (f == nullptr)
		return false;
	bool up = false;
	char line[256];
	// past the header: Iface, Destination, Gateway, Flags, ...
	if (std::fgets(line, sizeof(line), f) != nullptr)
		while (!up && std::fgets(line, sizeof(line), f) != nullptr)
		{
			char iface[32];
			unsigned destination, gateway, flags;
			if (std::sscanf(line, "%31s %x %x %x", iface, &destination, &gateway, &flags) == 4)
				up = destination == 0 && (flags & 1) != 0 && std::strncmp(iface, "dclink", 6) != 0;	// RTF_UP
		}
	std::fclose(f);
	return up;
}

void clearMessage()
{
	st.message.clear();
	st.yesHint = nullptr;
	st.yes = nullptr;
	st.no = nullptr;
}

// B leaves the page (its own back if it has one), A does the row's thing, Left/Right picks a Choice's value
void actList(Button code)
{
	Screen& screen = st.screens.back();
	if (code == Button::B || (code == Button::Menu && st.screens.size() == 1))
	{
		if (screen.back)
		{
			const std::function<void()> back = screen.back;
			back();
		}
		else if (st.screens.size() > 1)
			pop();
		return;
	}
	if (screen.act)
	{
		const std::function<void(Button)> act = screen.act;
		act(code);
		return;
	}
	if (screen.items.empty())
		return;
	Item& item = screen.items[screen.selected];
	switch (code)
	{
	case Button::Up:
		move(screen, -1);
		break;
	case Button::Down:
		move(screen, 1);
		break;
	case Button::Left:
	case Button::Right:
	{
		const int step = code == Button::Left ? -1 : 1;
		if (item.row == Row::Choice && !item.values.empty())
		{
			const int count = (int)item.values.size();
			item.value = (item.value + step + count) % count;
			if (item.change)
				item.change(item.value);
		}
		else if (item.shift)
			item.shift(step);
		break;
	}
	case Button::A:
		if (item.confirm)
		{
			// it may change the pages, this one's items with them
			const std::function<void()> confirm = item.confirm;
			confirm();
		}
		break;
	case Button::X:
		if (item.row == Row::Binding && item.clear)
			item.clear();
		break;
	default:
		break;
	}
}

void act(Button code)
{
	if (st.hostPage)
	{
		if (code == Button::A || code == Button::B)
			st.result = Result::Quit;
		return;
	}
	if (st.work || st.screens.empty())
		return;
	st.dirty = true;
	if (!st.message.empty())
	{
		if (code == Button::A || code == Button::B)
		{
			const std::function<void()> then = code == Button::A ? st.yes : st.no;
			clearMessage();
			if (then)
				then();
		}
	}
	else
		actList(code);
}

//
// Drawing
//

// a line of text centered in a row, by its capitals' height
int baseline(Font& font, int top, int height)
{
	return top + (height + font.capHeight()) / 2;
}

int buttonWidth(const char *button, const char *hint)
{
	const int w = std::strlen(button) == 1 ? ButtonSize : ButtonSize / 2 + loaded->tiny.measure(button);
	return w + ButtonMargin + loaded->small.measure(hint);
}

void drawButton(const char *button, const char *hint, int x, int y)
{
	Canvas& c = st.canvas;
	int ox;
	if (std::strlen(button) == 1)
	{
		c.circle(x + ButtonSize / 2, y + ButtonSize / 2, ButtonSize / 2, ColorMain);
		Font& medium = loaded->medium;
		c.text(medium, button, x + (ButtonSize - medium.measure(button)) / 2, baseline(medium, y, ButtonSize), ColorAccent2);
		ox = ButtonSize;
	}
	else
	{
		Font& tiny = loaded->tiny;
		const int w = tiny.measure(button);
		c.pill(x, y, ButtonSize / 2 + w, ButtonSize, ColorMain);
		c.text(tiny, button, x + ButtonSize / 4, baseline(tiny, y, ButtonSize), ColorAccent2);
		ox = ButtonSize / 2 + w;
	}
	ox += ButtonMargin;
	c.text(loaded->small, hint, x + ox, baseline(loaded->small, y, ButtonSize), ColorHint);
}

// button and hint pairs in a pill at the bottom right of a page drawn at ox; the pill's width
int drawButtons(const Buttons& pairs, int ox)
{
	if (pairs.empty())
		return 0;
	Canvas& c = st.canvas;
	int ow = ButtonMargin;
	for (const auto& pair : pairs)
		ow += buttonWidth(pair.first, pair.second) + ButtonMargin;
	int x = ox + c.width() - Padding - ow;
	int y = c.height() - Padding - PillSize;
	c.pill(x, y, ow, PillSize, ColorAccent);
	x += ButtonMargin;
	y += ButtonMargin;
	for (const auto& pair : pairs)
	{
		drawButton(pair.first, pair.second, x, y);
		x += buttonWidth(pair.first, pair.second) + ButtonMargin;
	}
	return ow;
}

// the selected row's description at the bottom left, beside the buttons' pill (width wide), on its line
void drawDescription(const std::string& desc, int width)
{
	if (desc.empty())
		return;
	Canvas& c = st.canvas;
	Font& tiny = loaded->tiny;
	const std::vector<std::string> lines = wrap(tiny, desc, width);
	const int leading = S(12);
	int y = c.height() - Padding - (PillSize + leading * (int)lines.size()) / 2;
	for (const std::string& line : lines)
	{
		c.text(tiny, line, Padding, baseline(tiny, y, leading), White);
		y += leading;
	}
}

// a page's rows, from the same top on every page (pages sliding by line up), the selected one in a pill, a
// Choice's or Binding's value at the right; arrows above and below when there are more
void drawList(const Screen& screen, int ox)
{
	Canvas& c = st.canvas;
	Font& large = loaded->large;
	const int count = (int)screen.items.size();
	const int shown = std::min(count, MaxRows);
	// MaxRows of them in the middle of the room between the title and the buttons
	const int top = Padding + PillSize + (c.height() - 2 * (Padding + PillSize) - MaxRows * RowPitch) / 2;
	const int overhang = (PillSize - RowPitch) / 2;
	const int left = ox + Padding;
	const int right = ox + c.width() - Padding;
	for (int i = 0; i < shown; i++)
	{
		const int index = screen.start + i;
		const Item& item = screen.items[index];
		const bool selected = index == screen.selected;
		const int rowTop = top + i * RowPitch;
		std::string value;
		if (item.row != Row::Action && item.value < (int)item.values.size() && !(selected && st.awaiting))
			value = item.values[item.value];
		// Left and Right change the selected Choice
		if (selected && item.row == Row::Choice)
			value = "<  " + value + "  >";
		const int valueWidth = value.empty() ? 0 : large.measure(value) + ButtonPadding * 2;
		const std::string name = Canvas::fit(large, item.name, right - left - valueWidth - ButtonPadding * 2);
		u32 color = enabled(item) ? White : LightGray;
		if (selected)
		{
			if (item.row != Row::Action)
				c.pill(left, rowTop - overhang, right - left, PillSize, ColorAccent);
			c.pill(left, rowTop - overhang, large.measure(name) + ButtonPadding * 2, PillSize, ColorMain);
			color = ColorSelectedText;
		}
		c.text(large, name, left + ButtonPadding, baseline(large, rowTop, RowPitch), color);
		if (!value.empty())
			c.text(large, value, right - valueWidth + ButtonPadding, baseline(large, rowTop, RowPitch),
					enabled(item) ? White : LightGray);
	}
	if (count > shown)
	{
		// NextUI's gray arrows, 24 x 4 (1x), in the gaps above and below the rows
		const float ax = (float)(left + ButtonPadding);
		if (screen.start > 0)
		{
			const float y = (float)((Padding + PillSize + top - overhang) / 2 - S(2));
			const float up[] { ax, y + S(4), ax + S(12), y, ax + S(24), y + S(4) };
			c.polygon(up, 3, Gray);
		}
		if (screen.start + shown < count)
		{
			const float y = (float)((top + MaxRows * RowPitch + overhang + c.height() - Padding - PillSize) / 2 - S(2));
			const float down[] { ax, y, ax + S(24), y, ax + S(12), y + S(4) };
			c.polygon(down, 3, Gray);
		}
	}
	if (count > 0 && screen.items[screen.selected].aside)
		screen.items[screen.selected].aside(ox);
}

// a page's rows (or its own body) with their left edge at ox, off the screen while they slide
void drawBody(const Screen& screen, int ox)
{
	if (ox <= -st.canvas.width() || ox >= st.canvas.width())
		return;
	if (screen.body)
		screen.body(ox);
	else
		drawList(screen, ox);
}

// what a page's buttons do: its own, else by its selected row
Buttons pageButtons(const Screen& screen)
{
	if (screen.buttons)
		return screen.buttons();
	Buttons buttons { { "B", "BACK" } };
	if (!screen.items.empty() && screen.items[screen.selected].row == Row::Binding)
	{
		buttons.push_back({ "X", "CLEAR" });
		buttons.push_back({ "A", "SET" });
	}
	else if (screen.items.empty() || screen.items[screen.selected].row == Row::Action)
		buttons.push_back({ "A", "OKAY" });
	return buttons;
}

// a page's title in its pill, its buttons, its status or its selected row's description: in place
void drawPills(const Screen& screen)
{
	Canvas& c = st.canvas;
	Font& large = loaded->large;
	const int maxWidth = c.width() - Padding * 2;
	const std::string title = Canvas::fit(large, screen.title, maxWidth - ButtonPadding * 2);
	c.pill(Padding, Padding, std::min(maxWidth, large.measure(title) + ButtonPadding * 2), PillSize, ColorAccent);
	c.text(large, title, Padding + ButtonPadding, baseline(large, Padding, PillSize), ColorHint);
	const int buttonsWidth = drawButtons(pageButtons(screen), 0);
	if (!screen.body)
	{
		const std::string desc = screen.status ? screen.status()
				: !screen.items.empty() && !screen.items[screen.selected].desc.empty() ? screen.items[screen.selected].desc
				: screen.desc;
		drawDescription(desc, c.width() - Padding * 3 - buttonsWidth);
	}
}

void draw()
{
	Canvas& c = st.canvas;
	if (st.hostPage)
		st.hostPage();
	else if (st.work && !st.workOver)
		drawPage(st.workLine, st.workUnder);
	else
	{
		c.copy(st.background);
		if (st.work)
			drawMessage(loaded->large, st.workLine, 0, 0, c.width(), c.height());
		else if (!st.message.empty())
		{
			drawMessage(loaded->large, st.message, 0, 0, c.width(), c.height());
			if (st.yes)
				drawButtons({ { "B", "CANCEL" }, { "A", st.yesHint } }, 0);
			else
				drawButtons({ { "A", "OKAY" } }, 0);
		}
		else if (!st.screens.empty())
		{
			const Screen& screen = st.screens.back();
			if (st.sliding && (st.slideBack ? st.leaving != nullptr : st.screens.size() > 1))
			{
				// opened: both pages' rows to the left, the new ones in from the right; left: to the right
				const float t = easeOut(std::min(1.f, (float)(nowMs() - st.slideStart) / SlideMs));
				const int shift = (int)std::lround(t * c.width());
				const Screen& from = st.slideBack ? *st.leaving : st.screens[st.screens.size() - 2];
				drawBody(from, st.slideBack ? shift : -shift);
				drawBody(screen, st.slideBack ? shift - c.width() : c.width() - shift);
			}
			else
				drawBody(screen, 0);
			drawPills(screen);
		}
	}
	st.display->show(c.data());
}

void close(bool resume)
{
	st.display->end(resume ? Display::After::LastFrame : Display::After::Stay);
	st.open = false;
	st.screens.clear();
	st.leaving.reset();
	st.sliding = false;
	st.hostPage = nullptr;
	st.work = nullptr;
	clearMessage();
}

// the screen taken for pictures, the canvas its size
bool begin()
{
	int width, height;
	if (st.display == nullptr || !loadFonts() || !st.display->begin(width, height))
		return false;
	st.canvas.resize(width, height);
	return true;
}

// a fresh menu: no page, nothing under way
void reset()
{
	st.screens.clear();
	st.leaving.reset();
	st.sliding = false;
	clearMessage();
	st.awaiting = false;
	st.work = nullptr;
	st.hostPage = nullptr;
	st.result = Result::Open;
	st.dirty = true;
	st.held = Button::None;
	st.chosen = st.host.core;
	st.network = checkNetwork();
	st.networkAt = nowMs() + 1000;
}

bool moduleInstalled()
{
	return st.hasModule;
}

// a module as the player is told of it: never the game's title alone, which reads as the game
std::string moduleName(const std::string& title)
{
	return title + " recomp module";
}

void checkModule();

// the game's recomp module: with none installed, Download Recomp Module alone; installed, Check for
// Update, Show FPS, then the module's settings, as its manifest declares them, kept in Deadcast's store
Screen recompOptionsScreen()
{
	Screen screen;
	screen.title = "Recomp Options";
	// Check for Update is always Recomp Options' first row and Show FPS its second, in every module,
	// whatever settings it declares: keep them first. Neither is declared by a module; the module's own
	// settings (module.cfg) follow them, in the order declared.
	screen.items.push_back(entry(st.hasModule ? "Check for Update" : "Download Recomp Module", "", [] {
		if (st.network)
			busy("Checking...", checkModule);
		else
			message("No network connection.");
	}));
	if (!st.hasModule)
		return screen;
	screen.desc = moduleName(st.module.name) + " " + st.module.version;
	// the frame rate in the corner, as the emulator's Show FPS
	Item fps;
	fps.name = "Show FPS";
	fps.desc = "Shows the frame rate.";
	fps.row = Row::Choice;
	fps.values = { "Off", "On" };
	const std::string fpsKey = st.module.id + ".fps";
	fps.value = recomp::get(fpsKey, "off") == "on" ? 1 : 0;
	fps.change = [fpsKey](int i) {
		recomp::set(fpsKey, i ? "on" : "off");
		if (st.host.recompSetting)
			st.host.recompSetting("fps", i ? "on" : "off");
	};
	screen.items.push_back(std::move(fps));
	for (const recomp::Setting& setting : st.module.settings)
	{
		Item item;
		item.name = setting.name;
		item.row = Row::Choice;
		const std::string key = st.module.id + "." + setting.key;
		const std::string now = recomp::get(key, setting.def);
		std::vector<std::string> values;
		for (const auto& value : setting.values)
		{
			if (value.first == now)
				item.value = (int)values.size();
			values.push_back(value.first);
			item.values.push_back(value.second);
		}
		item.change = [key, name = setting.key, values](int i) {
			recomp::set(key, values[i]);
			if (st.host.recompSetting)
				st.host.recompSetting(name, values[i]);
		};
		screen.items.push_back(std::move(item));
	}
	return screen;
}


// a page of Emulator Options' settings (Console, Video, Audio), each kept as it changes
Screen settingsScreen(const options::Page& list)
{
	Screen screen;
	screen.title = list.name;
	for (const options::Setting& setting : list.settings)
	{
		Item item;
		item.name = setting.name;
		item.desc = setting.desc;
		item.row = Row::Choice;
		const int now = st.host.emulatorSettings.get(setting);
		for (const auto& choice : setting.choices)
		{
			if (choice.second == now)
				item.value = (int)item.values.size();
			item.values.push_back(choice.first);
		}
		item.change = [s = &setting](int i) { st.host.emulatorSettings.set(*s, s->choices[i].second); };
		screen.items.push_back(std::move(item));
	}
	return screen;
}

// the emulator's settings, from either core (a module keeps them for the emulator's next start); its
// Controls and Local Multiplayer are its own, disabled in a module
Screen emulatorOptionsScreen()
{
	Screen screen;
	screen.title = "Emulator Options";
	screen.desc = st.host.version;
	for (const options::Page& list : options::pages())
		screen.items.push_back(entry(list.name, list.desc, [&list] { push(settingsScreen(list)); }));
	Item controls = entry("Controls", "", [] { st.host.controls(); });
	controls.available = [] { return st.host.controls != nullptr; };
	screen.items.push_back(std::move(controls));
	Item link = entry("Local Multiplayer", "Two players on two Bricks.", [] { st.host.localMultiplayer(); });
	link.available = [] { return st.host.localMultiplayer != nullptr; };
	screen.items.push_back(std::move(link));
	return screen;
}

// the game's core, Deadcast's emulation or a recomp module of it, and each core's options, set from
// either core
Screen optionsScreen()
{
	Screen screen;
	screen.title = "Options";
	screen.status = networkStatus;
	Item current;
	current.name = "Current Core";
	current.row = Row::Choice;
	current.values = { "Emulator", "Recomp" };
	current.value = (int)st.chosen;
	current.available = moduleInstalled;
	// only picked here: the main page's Continue (the first page's first row) becomes Restart, which
	// starts the game again in it; leaving the menu otherwise puts the game's own core back (frame())
	current.change = [](int value) {
		st.chosen = (Core)value;
		st.screens.front().items.front().name = st.chosen != st.host.core ? "Restart" : "Continue";
	};
	screen.items.push_back(std::move(current));
	screen.items.push_back(entry("Emulator Options", "", [] { push(emulatorOptionsScreen()); }));
	screen.items.push_back(entry("Recomp Options", "", [] { push(recompOptionsScreen()); }));
	return screen;
}

std::string megabytes(uint64_t bytes)
{
	return std::to_string((bytes + (1 << 19)) >> 20) + " MB";
}

// how far the download is, then its unpacking (install()'s bytes of the archive read): what is being
// done, the module, a bar, and under it the megabytes while downloading
void drawDownload(int ox)
{
	Canvas& c = st.canvas;
	const recomp::CatalogEntry& e = *st.downloading;
	const uint64_t size = st.downloadSize > 0 ? st.downloadSize.load() : e.size;
	const std::string doing = st.installing ? "Installing" : st.cancelDownload ? "Stopping" : "Downloading";
	Font& font = loaded->large;
	Font& small = loaded->small;
	const bool counted = !st.installing;
	const int lines = font.lineHeight() * 2, barHeight = S(6), gap = S(10);
	const int block = lines + gap + barHeight + (counted ? gap / 2 + small.lineHeight() : 0);
	const int top = (c.height() - block) / 2;
	drawMessage(font, doing + "\n" + moduleName(e.name) + " " + e.version, ox, top, c.width(), lines);
	const int barWidth = c.width() / 2, barX = ox + (c.width() - barWidth) / 2, barY = top + lines + gap;
	const double done = size > 0 ? std::min(1.0, (double)st.downloaded / (double)size) : 0.0;
	c.pill(barX, barY, barWidth, barHeight, ColorAccent);
	if (done > 0)
		c.pill(barX, barY, std::max(barHeight, (int)(barWidth * done)), barHeight, ColorMain);
	if (counted)
		drawMessage(small, megabytes(st.downloaded) + " of " + megabytes(size), ox, barY + barHeight + gap / 2,
				c.width(), small.lineHeight());
}

// the download over (installed, failed or stopped): its page gone, what came of it said; installed,
// Recomp Options again with the module's rows
void downloadOver()
{
	st.download.join();
	pop();
	if (!st.downloadError.empty())
	{
		message(st.downloadError);
		return;
	}
	st.hasModule = recomp::installed(st.discSha1, st.module);
	// Recomp Options again, now with the module's rows
	if (depth() > 0 && page().title == "Recomp Options")
		page() = recompOptionsScreen();
	// in the emulator, Current Core picks it from now on
	const std::string name = moduleName(st.module.name);
	if (st.host.core == Core::Emulator)
		message(name + " is installed.");
	else
		message(name + " is updated: it plays from your game's next start.");
}

// fetched (installed by recomp::install) on a thread of its own, its page showing how far; B stops it
void startDownload(const recomp::CatalogEntry *entry)
{
	st.downloading = entry;
	st.downloaded = 0;
	st.downloadSize = entry->size;
	st.cancelDownload = false;
	st.downloadDone = false;
	st.installing = false;
	st.downloadError.clear();
	st.download = std::thread([entry] {
		st.downloadError = recomp::install(*entry, st.downloaded, st.downloadSize, st.cancelDownload, st.installing);
		st.downloadDone = true;
	});
	Screen screen;
	screen.title = "Recomp Module";
	screen.body = drawDownload;
	// stopped while it downloads; once unpacking, it finishes
	screen.buttons = [] { return st.installing ? Buttons {} : Buttons { { "B", "STOP" } }; };
	screen.back = [] { st.cancelDownload = true; };
	screen.act = [](Button) {};
	screen.tick = [] {
		if (st.downloadDone)
			downloadOver();
		else
			redraw();
	};
	push(std::move(screen));
}

// what the catalog has for this game's disc: newer than the module installed, there to fetch, then
// fetched if the player says so
void checkModule()
{
	const recomp::CatalogEntry *entry = recomp::catalogFor(st.discSha1);
	if (entry == nullptr)
	{
		message("Sorry, no recomp module found.");
		return;
	}
	if (st.hasModule && st.module.version == entry->version)
	{
		message(moduleName(st.module.name) + " is up to date.");
		return;
	}
	const std::string why = recomp::reachable(*entry);
	if (!why.empty())
	{
		message(why);
		return;
	}
	ask("Download\n" + moduleName(entry->name) + " " + entry->version + "?", "DOWNLOAD",
			[entry] { startDownload(entry); });
}

}	// namespace

// the core picked in Options stored as the game's (from its next start)
static void keepChosen()
{
	recomp::set("core." + st.module.id, st.chosen == Core::Recomp ? "recomp" : "emulator");
}

Screen mainScreen(const std::string& title, Host host)
{
	Screen screen;
	screen.title = title;
	screen.back = [] { finish(Result::Resume); };
	screen.status = networkStatus;
	// Continue; Restart with another core picked in Options: the game starts again in it
	screen.items.push_back(entry("Continue", "", [] {
		if (st.chosen == st.host.core)
			finish(Result::Resume);
		else
		{
			keepChosen();
			finish(Result::Switch);
		}
	}));
	for (Item *item : { &host.save, &host.load })
	{
		if (item->name.empty())
			item->name = item == &host.save ? "Save" : "Load";
		// the emulator's save states: none in a recomp
		item->available = [] { return st.host.core == Core::Emulator; };
		screen.items.push_back(*item);
	}
	screen.items.push_back(entry("Options", "", [] { push(optionsScreen()); }));
	screen.items.push_back(entry("Quit", "", [] { finish(Result::Quit); }));
	st.discSha1 = recomp::chdSha1(host.disc);
	st.hasModule = recomp::installed(st.discSha1, st.module);
	st.host = std::move(host);
	return screen;
}

void setDisplay(Display *display)
{
	st.display = display;
}

Fonts& fonts()
{
	return *loaded;
}

Canvas& canvas()
{
	return st.canvas;
}

Item entry(const std::string& name, const std::string& desc, std::function<void()> confirm)
{
	Item item;
	item.name = name;
	item.desc = desc;
	item.confirm = std::move(confirm);
	return item;
}

bool enabled(const Item& item)
{
	return !item.available || item.available();
}

bool open(const std::vector<u8>& frame, int frameWidth, int frameHeight, Screen first)
{
	if (st.open)
		return true;
	if (!begin())
		return false;
	const int width = st.canvas.width();
	const int height = st.canvas.height();
	// blurred (a spread, the standard deviation, of shrink * sqrt(radius * (radius + 1)): about 13 of the
	// frame's pixels) and darkened, so that white text reads over a white screen too; scaled to the screen
	// once, as every page is drawn over it
	int blurredWidth, blurredHeight;
	const std::vector<u8> blurred = blur(frame, frameWidth, frameHeight, 2, 6, blurredWidth, blurredHeight);
	st.background.resize(width, height);
	st.background.clear(ColorBackground);
	if (!blurred.empty())
		st.background.picture(blurred.data(), blurredWidth, blurredHeight, 0, 0, width, height, BackdropBrightness);
	reset();
	st.screens.push_back(std::move(first));
	st.open = true;
	return true;
}

bool isOpen()
{
	return st.open;
}

void push(Screen screen)
{
	st.screens.push_back(std::move(screen));
	st.leaving.reset();
	st.sliding = true;
	st.slideBack = false;
	st.slideStart = nowMs();
	st.dirty = true;
}

void pop()
{
	st.leaving = std::make_unique<Screen>(std::move(st.screens.back()));
	st.screens.pop_back();
	st.sliding = true;
	st.slideBack = true;
	st.slideStart = nowMs();
	st.dirty = true;
}

Screen& page()
{
	return st.screens.back();
}

size_t depth()
{
	return st.screens.size();
}

// to the next row enabled that way (around the end); none: where it is
void move(Screen& screen, int step)
{
	const int count = (int)screen.items.size();
	if (count == 0)
		return;
	for (int i = 1; i <= count; i++)
	{
		const int index = ((screen.selected + step * i) % count + count) % count;
		if (enabled(screen.items[index]))
		{
			screen.selected = index;
			break;
		}
	}
	if (screen.selected < screen.start)
		screen.start = screen.selected;
	if (screen.selected >= screen.start + MaxRows)
		screen.start = screen.selected - MaxRows + 1;
}

void redraw()
{
	st.dirty = true;
}

void message(const std::string& text)
{
	clearMessage();
	st.message = text;
	st.dirty = true;
}

void ask(const std::string& text, const char *yesHint, std::function<void()> yes, std::function<void()> no)
{
	st.message = text;
	st.yesHint = yesHint;
	st.yes = std::move(yes);
	st.no = std::move(no);
	st.dirty = true;
}

void work(const std::string& line, const std::string& under, std::function<void()> fn)
{
	st.work = std::move(fn);
	st.workLine = line;
	st.workUnder = under;
	st.workOver = false;
	st.dirty = true;
}

void busy(const std::string& text, std::function<void()> fn)
{
	work(text, "", std::move(fn));
	st.workOver = true;
}

void awaitControl(bool waiting)
{
	st.awaiting = waiting;
	st.dirty = true;
}

bool awaitingControl()
{
	return st.awaiting;
}

void button(Button code, bool pressed)
{
	if (!st.open)
		return;
	if (!pressed)
	{
		if (code == st.held)
			st.held = Button::None;
		return;
	}
	if (code == Button::Up || code == Button::Down || code == Button::Left || code == Button::Right)
	{
		st.held = code;
		st.repeatAt = nowMs() + 300;
	}
	act(code);
}

void finish(Result result)
{
	st.result = result;
}

Result frame()
{
	if (!st.open)
		return Result::Resume;
	// another core picked in Options: back to the game (B, MENU) puts the game's own core back; quitting
	// keeps the one picked for the game's next start (Restart starts the game again in it: mainScreen)
	if (st.result == Result::Resume)
		st.chosen = st.host.core;
	if (st.result == Result::Quit && st.chosen != st.host.core)
		keepChosen();
	// work once its screen is up
	if (st.work && !st.dirty)
	{
		const std::function<void()> fn = std::move(st.work);
		st.work = nullptr;
		st.dirty = true;
		fn();
	}
	if (!st.screens.empty() && st.screens.back().tick && st.result == Result::Open)
	{
		const std::function<void()> tick = st.screens.back().tick;
		tick();
	}
	const u64 now = nowMs();
	// a page sliding: drawn every frame until it is in place
	if (st.sliding)
	{
		if (now - st.slideStart >= (u64)SlideMs)
		{
			st.sliding = false;
			st.leaving.reset();
		}
		st.dirty = true;
	}
	// the network, for the pages' status
	if (now >= st.networkAt)
	{
		st.networkAt = now + 1000;
		const bool up = checkNetwork();
		if (up != st.network)
		{
			st.network = up;
			st.dirty = true;
		}
	}
	if (st.held != Button::None && st.result == Result::Open && now >= st.repeatAt)
	{
		st.repeatAt = now + 100;
		act(st.held);
	}
	if (st.result == Result::Open && st.dirty)
	{
		// off a row disabled meanwhile (a session's Continue, the other Brick gone), or as a page opens
		if (!st.screens.empty())
		{
			Screen& screen = st.screens.back();
			if (!screen.items.empty() && !enabled(screen.items[screen.selected]))
				move(screen, 1);
		}
		draw();
		st.dirty = false;
	}
	const Result result = st.result;
	if (result == Result::Switch)
	{
		// the start page left up while the game starts again in the other core
		drawPage("Loading...", st.host.disc.substr(st.host.disc.find_last_of('/') + 1));
		st.display->show(st.canvas.data());
	}
	if (result != Result::Open)
		close(result == Result::Resume);
	return result;
}

bool showStart(const std::string& line, const std::string& under)
{
	if (st.starting)
		return true;
	if (st.open || !begin())
		return false;
	drawPage(line, under);
	st.display->show(st.canvas.data());
	st.starting = true;
	return true;
}

bool starting()
{
	return st.starting;
}

void startLine(const std::string& line, const std::string& under)
{
	if (!st.starting)
		return;
	drawPage(line, under);
	st.display->show(st.canvas.data());
}

void endStart()
{
	if (!st.starting)
		return;
	st.display->end(Display::After::NextFrame);
	st.starting = false;
}

bool showPage(std::function<void()> draw)
{
	if (!st.starting && !begin())
		return false;
	st.starting = false;
	reset();
	st.hostPage = std::move(draw);
	st.open = true;
	return true;
}

void putPage(const std::string& line, const std::string& under)
{
	const int fd = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	fb_var_screeninfo var {};
	fb_fix_screeninfo fix {};
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0
			&& var.bits_per_pixel == 32 && loadFonts())
	{
		st.canvas.resize((int)var.xres, (int)var.yres);
		drawPage(line, under);
		// the canvas is fb0's BGRA already: row by row into the page on show
		for (u32 y = 0; y < var.yres; y++)
			if (pwrite(fd, st.canvas.data() + (size_t)y * var.xres, (size_t)var.xres * 4,
					(off_t)(var.yoffset + y) * fix.line_length + (off_t)var.xoffset * 4) < 0)
				break;
	}
	::close(fd);
}

void drawPage(const std::string& line, const std::string& under)
{
	Canvas& c = st.canvas;
	Font& big = loaded->page;
	Font& text = loaded->pageText;
	const int width = std::min(c.width() - 120, 904);
	const std::vector<std::string> lines = wrap(big, line, width);
	const std::vector<std::string> unders = under.empty() ? std::vector<std::string>() : wrap(text, under, width);
	const int gap = unders.empty() ? 0 : 16;
	int y = (c.height() - (int)lines.size() * big.lineHeight() - gap - (int)unders.size() * text.lineHeight()) / 2;
	c.clear(Black);
	for (const std::string& l : lines)
	{
		c.text(big, l, (c.width() - big.measure(l)) / 2, y + big.ascent(), White);
		y += big.lineHeight();
	}
	y += gap;
	for (const std::string& l : unders)
	{
		c.text(text, l, (c.width() - text.measure(l)) / 2, y + text.ascent(), TextGray);
		y += text.lineHeight();
	}
}

std::vector<u32> label(const std::string& text, int& width, int& height)
{
	width = height = 0;
	if (!loadFonts())
		return {};
	Font& font = loaded->tiny;
	const int pad = S(2);
	width = font.measure(text) + pad * 2;
	height = font.lineHeight() + pad * 2;
	Canvas c;
	c.resize(width, height);
	c.clear(0x202020);
	// the emulator's: yellow at 70% over its dark box
	c.text(font, text, pad, pad + font.ascent(), 0xbcbc0a);
	return std::vector<u32>(c.data(), c.data() + (size_t)width * height);
}

void drawMessage(Font& font, const std::string& message, int x, int y, int w, int h)
{
	// its lines, each broken to fit within the rectangle's padding
	std::vector<std::string> lines;
	for (size_t start = 0, end; start <= message.size(); start = end + 1)
	{
		end = message.find('\n', start);
		if (end == std::string::npos)
			end = message.size();
		for (const std::string& line : wrap(font, message.substr(start, end - start), w - (Padding + ButtonPadding) * 2))
			lines.push_back(line);
	}
	int top = y + (h - font.lineHeight() * (int)lines.size()) / 2;
	for (const std::string& line : lines)
	{
		st.canvas.text(font, line, x + (w - font.measure(line)) / 2, top + font.ascent(), White);
		top += font.lineHeight();
	}
}

std::vector<std::string> wrap(Font& font, const std::string& text, int width)
{
	std::vector<std::string> lines { "" };
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t end = text.find(' ', pos);
		if (end == std::string::npos)
			end = text.size();
		const std::string word = text.substr(pos, end - pos);
		const std::string longer = lines.back().empty() ? word : lines.back() + " " + word;
		if (!lines.back().empty() && font.measure(longer) > width)
			lines.push_back(word);
		else
			lines.back() = longer;
		pos = end + 1;
	}
	return lines;
}

bool network()
{
	return st.network;
}

std::string networkStatus()
{
	return st.network ? "Network Connected" : "No Network Connection";
}

std::string gameName(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	for (size_t dot; (dot = name.rfind('.')) != std::string::npos && name.size() - dot > 2 && name.size() - dot <= 5; )
		name.erase(dot);
	const std::string whole = name;
	for (size_t paren; (paren = name.find_last_of("([")) != std::string::npos && paren > 0; )
		name.erase(paren);
	name.erase(name.find_last_not_of(' ') + 1);
	return name.empty() ? whole : name;
}

}
