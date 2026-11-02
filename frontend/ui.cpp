#include "ui.h"
#include "download.h"
#include "recomp.h"

#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <strings.h>
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
constexpr int RowOverhang = (PillSize - RowPitch) / 2;
constexpr int InfoLeading = S(12);	// a module's page: from one line of its text to the next
constexpr int InfoIndent = S(8);	// a list's lines, past the dot
constexpr int InfoHeadingGap = S(4);	// and under a section's name
constexpr int InfoHeight = MaxRows * RowPitch + 2 * RowOverhang;	// the room for its text, that of the rows
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

// a line of a module's page; heading: a section's name, drawn as the store's are; marked: a dot before it, a
// list entry's first line
struct InfoLine
{
	std::string text;
	u32 color = White;
	bool heading = false;
	bool indented = false;
	bool marked = false;
};

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
	std::string chosen;				// the core picked in Options: a module's id, empty for the emulator; the
									// game's only once the menu is left
	std::string running;			// the core playing now, the same way
	std::vector<recomp::Module> modules;	// the recomp modules installed for the disc, by title
	std::string discSha1;			// the disc's (its CHD header's)
	// a module being downloaded (its page up until it is done)
	std::thread download;
	std::atomic<uint64_t> downloaded { 0 };
	std::atomic<uint64_t> downloadSize { 0 };
	std::atomic<bool> cancelDownload { false };
	std::atomic<bool> downloadDone { false };
	std::atomic<bool> installing { false };		// fetched and checked: being unpacked
	std::string downloadError;
	recomp::StoreEntry downloading;
	bool updating = false;			// over a version installed
	std::vector<recomp::StoreEntry> store;		// what the Recomp Store offers, read as it opens
	bool storeOffline = false;		// no internet as it opened: it offers nothing, and says so
	std::vector<InfoLine> info;		// a module's page: its text in lines
	int infoTop = 0;				// the line at the top of it
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

// where a page's rows start, the same on every page (pages sliding by line up): MaxRows of them in the middle
// of the room between the title and the buttons
int rowsTop()
{
	return Padding + PillSize + (st.canvas.height() - 2 * (Padding + PillSize) - MaxRows * RowPitch) / 2;
}

// NextUI's gray arrows, 24 x 4 (1x), in the gaps above and below the rows: there is more that way
void drawArrows(int left, bool above, bool below)
{
	static const Sprite up = [] {
		Sprite shape(S(24), S(4));
		const float points[] { 0, (float)S(4), (float)S(12), 0, (float)S(24), (float)S(4) };
		shape.polygon(points, 3);
		return shape;
	}();
	static const Sprite down = [] {
		Sprite shape(S(24), S(4));
		const float points[] { 0, 0, (float)S(24), 0, (float)S(12), (float)S(4) };
		shape.polygon(points, 3);
		return shape;
	}();
	Canvas& c = st.canvas;
	const int top = rowsTop();
	const int x = left + ButtonPadding;
	if (above)
		c.sprite(up, x, (Padding + PillSize + top - RowOverhang) / 2 - S(2), Gray);
	if (below)
		c.sprite(down, x, (top + MaxRows * RowPitch + RowOverhang + c.height() - Padding - PillSize) / 2 - S(2), Gray);
}

// A download icon: an arrow down onto a bar, as high as the font's capitals and about as heavy. Drawn, as Noto
// Sans has no such glyph; x is its left, y the text's baseline.
void drawDownloadIcon(Font& font, int x, int y, u32 color)
{
	static const Sprite icon = [&font] {
		const float h = (float)font.capHeight();
		const float stem = h * 0.22f, bar = h * 0.18f, gap = h * 0.1f;
		const float tip = h - bar - gap;
		const float shoulder = tip * 0.45f;
		const float middle = h / 2;
		const float arrow[] {
			middle - stem / 2, 0, middle + stem / 2, 0, middle + stem / 2, shoulder,
			h * 0.92f, shoulder, middle, tip, h * 0.08f, shoulder, middle - stem / 2, shoulder
		};
		const float tray[] { 0, h - bar, h, h - bar, h, h, 0, h };
		Sprite shape(font.capHeight(), font.capHeight());
		shape.polygon(arrow, 7);
		shape.polygon(tray, 4);
		return shape;
	}();
	st.canvas.sprite(icon, x, y - font.capHeight(), color);
}

// a page's rows, from the same top on every page (pages sliding by line up), the selected one in a pill, a
// Choice's or Binding's value at the right; arrows above and below when there are more
void drawList(const Screen& screen, int ox)
{
	Canvas& c = st.canvas;
	Font& large = loaded->large;
	const int count = (int)screen.items.size();
	const int shown = std::min(count, MaxRows);
	const int top = rowsTop();
	const int overhang = RowOverhang;
	const int left = ox + Padding;
	const int right = ox + c.width() - Padding;
	for (int i = 0; i < shown; i++)
	{
		const int index = screen.start + i;
		const Item& item = screen.items[index];
		const bool selected = index == screen.selected;
		const int rowTop = top + i * RowPitch;
		if (item.row == Row::Header)
		{
			c.text(loaded->small, item.name, left + ButtonPadding, baseline(loaded->small, rowTop, RowPitch), Gray);
			continue;
		}
		std::string value;
		if (item.row != Row::Action && item.value < (int)item.values.size() && !(selected && st.awaiting))
			value = item.values[item.value];
		// a long value (Core's: a module's title) cut to leave room for the row's name and the arrows
		if (item.row == Row::Choice && !value.empty())
			value = Canvas::fit(large, value, right - left - large.measure(item.name) - large.measure("<    >") - ButtonPadding * 4);
		// Left and Right change the selected Choice
		if (selected && item.row == Row::Choice)
			value = "<  " + value + "  >";
		const int valueWidth = value.empty() ? 0 : large.measure(value) + ButtonPadding * 2;
		const int iconWidth = item.downloadIcon ? S(5) + large.capHeight() : 0;
		const std::string name = Canvas::fit(large, item.name, right - left - valueWidth - iconWidth - ButtonPadding * 2);
		u32 color = enabled(item) ? White : LightGray;
		if (selected)
		{
			if (item.row != Row::Action)
				c.pill(left, rowTop - overhang, right - left, PillSize, ColorAccent);
			c.pill(left, rowTop - overhang, large.measure(name) + iconWidth + ButtonPadding * 2, PillSize, ColorMain);
			color = ColorSelectedText;
		}
		if (item.downloadIcon)
			drawDownloadIcon(large, left + ButtonPadding, baseline(large, rowTop, RowPitch), color);
		c.text(large, name, left + ButtonPadding + iconWidth, baseline(large, rowTop, RowPitch), color);
		if (!value.empty())
			c.text(large, value, right - valueWidth + ButtonPadding, baseline(large, rowTop, RowPitch),
					enabled(item) ? White : LightGray);
	}
	drawArrows(left, screen.start > 0, screen.start + shown < count);
	if (count > 0 && screen.items[screen.selected].aside)
		screen.items[screen.selected].aside(ox);
}

int infoLineHeight(const InfoLine& line)
{
	return InfoLeading + (line.heading ? InfoHeadingGap : 0);
}

// the top line that leaves the last line of a module's page at the bottom of its room
int infoLastTop()
{
	int height = 0;
	for (int i = (int)st.info.size() - 1; i >= 0; i--)
	{
		height += infoLineHeight(st.info[i]);
		if (height > InfoHeight)
			return i + 1;
	}
	return 0;
}

// a module's page: its text, from the line at the top, in the room of the rows
void drawInfo(int ox)
{
	Font& tiny = loaded->tiny;
	const int left = ox + Padding;
	const int x = left + ButtonPadding;
	const int bottom = rowsTop() + InfoHeight;
	int top = rowsTop();
	int index = st.infoTop;
	for (; index < (int)st.info.size() && top + InfoLeading <= bottom; index++)
	{
		const InfoLine& line = st.info[index];
		Font& font = line.heading ? loaded->small : tiny;
		const int y = baseline(font, top, InfoLeading);
		if (line.marked)
			st.canvas.text(font, "\xe2\x80\xa2", x, y, line.color);	// a dot
		st.canvas.text(font, line.text, line.indented ? x + InfoIndent : x, y, line.color);
		top += infoLineHeight(line);
	}
	drawArrows(left, st.infoTop > 0, index < (int)st.info.size());
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
	st.chosen = st.running;
	st.network = checkNetwork();
	st.networkAt = nowMs() + 1000;
}

bool moduleInstalled()
{
	return !st.modules.empty();
}

// a module as the player is told of it: its title (Rez Recompiled), or with none declared its name as a
// module. Never the game's title alone, which reads as the game.
std::string moduleName(const std::string& title, const std::string& name)
{
	return title.empty() ? name + " recomp module" : title;
}

void refreshStore(const std::string& name);

// the selected row in view, and a section's name with the section's first row
void reveal(Screen& screen)
{
	int top = screen.selected;
	if (top > 0 && screen.items[top - 1].row == Row::Header)
		top--;
	if (top < screen.start)
		screen.start = top;
	if (screen.selected >= screen.start + MaxRows)
		screen.start = screen.selected - MaxRows + 1;
}

// the core picked in Options stored as the game's (from its next start): the module picked, and not the disc's
// others
void keepChosen()
{
	for (const recomp::Module& module : st.modules)
	{
		const std::string core = module.id == st.chosen ? "recomp" : "emulator";
		if (recomp::get("core." + module.id) != core)
			recomp::set("core." + module.id, core);
	}
}

// Continue, or Restart with another core picked: the first page's first row
void nameContinue()
{
	st.screens.front().items.front().name = st.chosen != st.running ? "Restart" : "Continue";
}

// what Recomp Options is about: the module picked in Options; with the emulator picked, the disc's module if it
// has just the one
const recomp::Module *optionsModule()
{
	for (const recomp::Module& module : st.modules)
		if (module.id == st.chosen)
			return &module;
	return st.modules.size() == 1 ? &st.modules.front() : nullptr;
}

// the settings of the module Core is on, as its manifest declares them, kept in Deadcast's store; the module's
// name and version at the bottom left, whatever is selected. Show FPS is always the first row, in every module;
// a module does not declare it. A setting applies at once only in the module it is for, if that one is playing.
// Getting a module, updating it and deleting it are the Recomp Store's.
Screen recompOptionsScreen()
{
	const recomp::Module module = *optionsModule();
	const bool playing = module.id == st.running;
	Screen screen;
	screen.title = "Recomp Options";
	const std::string label = moduleName(module.title, module.name) + " " + module.version;
	screen.status = [label] { return label; };
	// the frame rate in the corner, as the emulator's Show FPS
	Item fps;
	fps.name = "Show FPS";
	fps.row = Row::Choice;
	fps.values = { "Off", "On" };
	const std::string fpsKey = module.id + ".fps";
	fps.value = recomp::get(fpsKey, "off") == "on" ? 1 : 0;
	fps.change = [fpsKey, playing](int i) {
		recomp::set(fpsKey, i ? "on" : "off");
		if (playing && st.host.recompSetting)
			st.host.recompSetting("fps", i ? "on" : "off");
	};
	screen.items.push_back(std::move(fps));
	for (const recomp::Setting& setting : module.settings)
	{
		Item item;
		item.name = setting.name;
		item.row = Row::Choice;
		const std::string key = module.id + "." + setting.key;
		const std::string now = recomp::get(key, setting.def);
		std::vector<std::string> values;
		for (const auto& value : setting.values)
		{
			if (value.first == now)
				item.value = (int)values.size();
			values.push_back(value.first);
			item.values.push_back(value.second);
		}
		item.change = [key, name = setting.key, values, playing](int i) {
			recomp::set(key, values[i]);
			if (playing && st.host.recompSetting)
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
	Item core;
	core.name = "Core";
	core.row = Row::Choice;
	core.values = { "Emulator" };
	for (const recomp::Module& module : st.modules)
	{
		if (module.id == st.chosen)
			core.value = (int)core.values.size();
		core.values.push_back(moduleName(module.title, module.name));
	}
	core.available = moduleInstalled;
	// only picked here: the main page's Continue becomes Restart, which starts the game again in it; leaving
	// the menu otherwise puts the game's own core back (frame())
	core.change = [](int value) {
		st.chosen = value > 0 ? st.modules[value - 1].id : "";
		nameContinue();
	};
	screen.items.push_back(std::move(core));
	screen.items.push_back(entry("Emulator Options", "", [] { push(emulatorOptionsScreen()); }));
	Item recompOptions = entry("Recomp Options", "", [] { push(recompOptionsScreen()); });
	recompOptions.available = [] { return optionsModule() != nullptr; };
	screen.items.push_back(std::move(recompOptions));
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
	const recomp::StoreEntry& e = st.downloading;
	const uint64_t size = st.downloadSize > 0 ? st.downloadSize.load() : e.size;
	const std::string doing = st.installing ? "Installing" : st.cancelDownload ? "Stopping" : "Downloading";
	Font& font = loaded->large;
	Font& small = loaded->small;
	const bool counted = !st.installing;
	const int lines = font.lineHeight() * 2, barHeight = S(6), gap = S(10);
	const int block = lines + gap + barHeight + (counted ? gap / 2 + small.lineHeight() : 0);
	const int top = (c.height() - block) / 2;
	drawMessage(font, doing + "\n" + moduleName(e.title, e.name) + " " + e.version, ox, top, c.width(), lines);
	const int barWidth = c.width() / 2, barX = ox + (c.width() - barWidth) / 2, barY = top + lines + gap;
	const double done = size > 0 ? std::min(1.0, (double)st.downloaded / (double)size) : 0.0;
	c.pill(barX, barY, barWidth, barHeight, ColorAccent);
	if (done > 0)
		c.pill(barX, barY, std::max(barHeight, (int)(barWidth * done)), barHeight, ColorMain);
	if (counted)
		drawMessage(small, megabytes(st.downloaded) + " of " + megabytes(size), ox, barY + barHeight + gap / 2,
				c.width(), small.lineHeight());
}

// the download over (installed, failed or stopped): its page gone, what came of it said; installed, the
// store again, with the module where it now is
void downloadOver()
{
	st.download.join();
	pop();
	if (!st.downloadError.empty())
	{
		message(st.downloadError);
		return;
	}
	st.modules = recomp::installedFor(st.discSha1);
	const std::string name = moduleName(st.downloading.title, st.downloading.name);
	pop();
	refreshStore(name);
	// the module playing now goes on as it started
	if (st.downloading.id == st.running)
		message(name + " is updated: it plays from your game's next start.");
	else
		message(name + (st.updating ? " is updated." : " is installed."));
}

// fetched (installed by recomp::install) on a thread of its own, its page showing how far; B stops it
void startDownload(const recomp::StoreEntry& entry, bool updating)
{
	st.downloading = entry;
	st.updating = updating;
	st.downloaded = 0;
	st.downloadSize = entry.size;
	st.cancelDownload = false;
	st.downloadDone = false;
	st.installing = false;
	st.downloadError.clear();
	st.download = std::thread([] {
		st.downloadError = recomp::install(st.downloading, st.downloaded, st.downloadSize, st.cancelDownload, st.installing);
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

// whether version a (x.y.z) is newer than b, part by part
bool newer(const std::string& a, const std::string& b)
{
	const char *x = a.c_str(), *y = b.c_str();
	while (*x != '\0' || *y != '\0')
	{
		char *xe, *ye;
		const unsigned long xn = std::strtoul(x, &xe, 10), yn = std::strtoul(y, &ye, 10);
		if (xn != yn)
			return xn > yn;
		x = *xe == '.' ? xe + 1 : xe;
		y = *ye == '.' ? ye + 1 : ye;
		if (x == xe && y == ye)
			break;	// neither moved: not a number there
	}
	return false;
}

void getModule(const recomp::StoreEntry& entry, bool updating)
{
	if (st.network)
		startDownload(entry, updating);
	else
		message("No internet connection.");
}

// a module deleted, and everything of it, its saves too. At once, unless it is the module playing now:
// that one is only marked, and the game starts again emulated; Deadcast deletes it as it starts
// (recomp::tidy). Were it picked in Options but not restarted into, the core playing is the one picked again.
void removeModule(const recomp::Module& module, const std::string& name)
{
	if (module.id == st.running)
	{
		recomp::markForDeletion(module.id);
		st.chosen.clear();
		keepChosen();
		finish(Result::Switch);
		return;
	}
	const bool deleted = recomp::deleteModule(module.id);
	st.modules = recomp::installedFor(st.discSha1);
	if (module.id == st.chosen)
		st.chosen = st.running;
	nameContinue();
	pop();
	refreshStore(name);
	message(deleted ? name + " is deleted." : name + "\ncould not be fully deleted.\nDeadcast retries at its next start.");
}

// asked first: it takes the module's saves
void askRemove(const recomp::Module& module, const std::string& name)
{
	std::string question = "Delete\n" + name + " " + module.version + "\nand its saves?";
	if (module.id == st.running)
		question += "\nYour game restarts in the emulator.";
	ask(question, "DELETE", [module, name] { removeModule(module, name); });
}

bool playsThisDisc(const std::vector<std::string>& discs)
{
	return std::find(discs.begin(), discs.end(), st.discSha1) != discs.end();
}

// a module as the store knows it: on offer, installed, or both
struct StoreModule
{
	std::string title;
	bool offered = false;
	recomp::StoreEntry entry;
	bool installed = false;
	recomp::Module own;
	bool usable = true;		// this Deadcast can run what is on offer
	bool plays = false;		// it plays this game's disc
};

// every module offered and every one installed, offered or not, so that one can always be deleted; by title
std::vector<StoreModule> storeModules()
{
	std::vector<recomp::Module> mine = recomp::allInstalled();
	const std::string deadcast = recomp::get("deadcast.version");
	std::vector<StoreModule> modules;
	for (const recomp::StoreEntry& offer : st.store)
	{
		StoreModule m;
		m.title = moduleName(offer.title, offer.name);
		m.offered = true;
		m.entry = offer;
		m.usable = offer.needs.empty() || !newer(offer.needs, deadcast);
		m.plays = playsThisDisc(offer.discs);
		const auto own = std::find_if(mine.begin(), mine.end(), [&offer](const recomp::Module& module) {
			return module.id == offer.id;
		});
		if (own != mine.end())
		{
			m.installed = true;
			m.own = *own;
			m.plays = m.plays || playsThisDisc(own->discs);
			mine.erase(own);
		}
		modules.push_back(std::move(m));
	}
	for (const recomp::Module& module : mine)
	{
		StoreModule m;
		m.title = moduleName(module.title, module.name);
		m.installed = true;
		m.own = module;
		m.plays = playsThisDisc(module.discs);
		modules.push_back(std::move(m));
	}
	std::stable_sort(modules.begin(), modules.end(), [](const StoreModule& a, const StoreModule& b) {
		return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
	});
	return modules;
}

bool hasUpdate(const StoreModule& m)
{
	return m.installed && m.offered && m.usable && newer(m.entry.version, m.own.version);
}

// a module's page's text, from its store.json (of the version on offer, unless it is the one installed), in
// sections under their names: Description, Features, Supported Formats, Changelog (for the version named first)
// and Source. A section with nothing to say is left out.
std::vector<InfoLine> moduleInfo(const StoreModule& m)
{
	std::string json = m.installed && !hasUpdate(m) ? recomp::readText(m.own.dir + "/store.json") : "";
	if (json.empty())
		json = m.entry.info;
	const recomp::ModuleInfo info = recomp::readInfo(json);
	Font& tiny = loaded->tiny;
	const int width = st.canvas.width() - 2 * (Padding + ButtonPadding);
	std::vector<InfoLine> lines;
	// a section's name, after a gap
	const auto heading = [&lines](const char *name) {
		if (!lines.empty())
			lines.emplace_back();
		lines.push_back({ name, Gray, true, false, false });
	};
	const auto paragraph = [&](const std::string& text, u32 color) {
		for (const std::string& line : wrap(tiny, text, width))
			lines.push_back({ line, color, false, false, false });
	};
	const auto list = [&](const std::vector<std::string>& entries) {
		for (const std::string& entry : entries)
		{
			bool first = true;
			for (const std::string& line : wrap(tiny, entry, width - InfoIndent))
			{
				lines.push_back({ line, White, false, true, first });
				first = false;
			}
		}
	};
	if (!m.installed && !m.usable)
		paragraph("Needs a newer Deadcast.", White);
	if (!info.description.empty())
	{
		heading("Description");
		paragraph(info.description, White);
	}
	if (!info.features.empty())
	{
		heading("Features");
		list(info.features);
	}
	if (!info.formats.empty())
	{
		heading("Supported Formats");
		for (const recomp::ModuleInfo::Format& disc : info.formats)
		{
			paragraph(disc.region + " (" + disc.format + ")", White);
			paragraph(disc.sha1, TextGray);
		}
	}
	heading("Changelog");
	paragraph(hasUpdate(m) ? "Version " + m.entry.version + ", you have " + m.own.version
			: "Version " + (m.installed ? m.own.version : m.entry.version), White);
	list(info.changelog);
	if (!info.repo.empty())
	{
		heading("Source");
		const size_t scheme = info.repo.find("://");
		paragraph(scheme == std::string::npos ? info.repo : info.repo.substr(scheme + 3), White);
	}
	return lines;
}

// a module's own page: what it is, and on its buttons what can be done with it as it stands. Up and Down
// scroll the text.
Screen moduleScreen(const StoreModule& m)
{
	st.info = moduleInfo(m);
	st.infoTop = 0;
	const bool update = hasUpdate(m);
	const bool download = !m.installed && m.usable;
	Buttons buttons { { "B", "BACK" } };
	if (m.installed)
		buttons.push_back({ "X", "DELETE" });
	if (update)
		buttons.push_back({ "A", "UPDATE" });
	else if (download)
		buttons.push_back({ "A", "DOWNLOAD" });
	Screen screen;
	screen.title = m.title;
	screen.body = drawInfo;
	screen.buttons = [buttons] { return buttons; };
	screen.act = [m, update, download](Button code) {
		if (code == Button::Up || code == Button::Down)
			st.infoTop = std::max(0, std::min(st.infoTop + (code == Button::Up ? -1 : 1), infoLastTop()));
		else if (code == Button::A && (update || download))
			getModule(m.entry, update);
		else if (code == Button::X && m.installed)
			askRemove(m.own, m.title);
	};
	return screen;
}

// the store's page: what is installed, first and with a download icon what has an update, and what can be
// downloaded, each under its section's name unless it has nothing; every module opens its own page. The cursor on this game's
// module if it has one.
Screen storeScreen()
{
	Screen screen;
	screen.title = "Recomp Store";
	std::vector<StoreModule> modules = storeModules();
	std::stable_partition(modules.begin(), modules.end(), hasUpdate);
	const char *const sections[] = { "Installed", "Downloadable" };
	for (int section = 0; section < 2; section++)
	{
		bool named = false;
		for (const StoreModule& m : modules)
		{
			if ((m.installed ? 0 : 1) != section)
				continue;
			if (!named)
			{
				Item heading;
				heading.name = sections[section];
				heading.row = Row::Header;
				screen.items.push_back(std::move(heading));
				named = true;
			}
			if (m.plays && screen.selected == 0)
				screen.selected = (int)screen.items.size();
			Item row = entry(m.title, "", [m] { push(moduleScreen(m)); });
			row.downloadIcon = hasUpdate(m);
			screen.items.push_back(std::move(row));
		}
	}
	if (st.storeOffline)
	{
		Item heading;
		heading.name = sections[1];
		heading.row = Row::Header;
		screen.items.push_back(std::move(heading));
		Item none;
		none.name = "No internet connection";
		none.available = [] { return false; };
		screen.items.push_back(std::move(none));
	}
	if (screen.items.empty())
	{
		screen.desc = "No recomp modules yet.";
		screen.buttons = [] { return Buttons { { "B", "BACK" } }; };
	}
	else
		reveal(screen);
	return screen;
}

// the store's page again, its rows as they now are: the cursor on the module named, wherever it went, or
// where it was if it has gone
void refreshStore(const std::string& name)
{
	Screen& shown = page();
	Screen fresh = storeScreen();
	const int count = (int)fresh.items.size();
	fresh.selected = std::min(shown.selected, std::max(count - 1, 0));
	for (int i = 0; i < count; i++)
		if (fresh.items[i].row != Row::Header && fresh.items[i].name == name)
			fresh.selected = i;
	fresh.start = std::min(shown.start, std::max(count - MaxRows, 0));
	if (count > 0)
		reveal(fresh);
	shown = std::move(fresh);
}

// straight in, no page saying it is checking. With no internet it offers nothing: what is installed is listed
// to be deleted, and with nothing known of updates, nothing is said of them.
void openStore()
{
	st.storeOffline = !st.network;
	st.store = st.storeOffline ? std::vector<recomp::StoreEntry>() : recomp::storeCatalog();
	push(storeScreen());
}

}	// namespace

Screen mainScreen(const std::string& title, Host host)
{
	Screen screen;
	screen.title = title;
	screen.back = [] { finish(Result::Resume); };
	screen.status = networkStatus;
	// Continue; Restart with another core picked in Options: the game starts again in it
	screen.items.push_back(entry("Continue", "", [] {
		if (st.chosen == st.running)
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
	screen.items.push_back(entry("Recomp Store", "", openStore));
	screen.items.push_back(entry("Quit", "", [] { finish(Result::Quit); }));
	st.discSha1 = recomp::chdSha1(host.disc);
	st.modules = recomp::installedFor(st.discSha1);
	st.running = host.core == Core::Recomp ? recomp::runningModule() : "";
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
	return item.row != Row::Header && (!item.available || item.available());
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
	reveal(screen);
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
		st.chosen = st.running;
	if (st.result == Result::Quit && st.chosen != st.running)
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
