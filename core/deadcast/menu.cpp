#include "menu.h"
#include "canvas.h"
#include "link.h"
#include "netplay.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "input/gamepad_device.h"
#include "log/Log.h"
#include "oslib/oslib.h"
#include "oslib/resources.h"
#include "sdl/sdl_gamepad.h"
#include "version.h"
#include "wsi/context.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

namespace deadcast::menu
{
namespace
{

// NextUI's layout on the Brick: its 1x sizes, three times as large
constexpr int Scale = 3;
constexpr int S(int v) { return v * Scale; }
constexpr int Padding = S(5);
constexpr int PillSize = S(30);
constexpr int ButtonSize = S(20);
constexpr int ButtonMargin = S(5);
constexpr int ButtonPadding = S(12);
constexpr int SlotCount = 10;		// Deadcast's save state slots
constexpr int MaxRows = 6;			// a page's rows shown at once (arrows: there are more)
constexpr int RowPitch = S(28);		// from one row to the next (its pill, PillSize, a little taller)
constexpr u64 SlideMs = 150;
constexpr float BackdropBrightness = 0.4f;	// the paused game behind the menu (white: 40% grey)		// a page's rows sliding in, the ones before them out

constexpr u32 White = 0xffffff;
constexpr u32 Gray = 0x999999;
constexpr u32 LightGray = 0x7f7f7f;
constexpr u32 Black = 0x000000;

// the Brick's buttons as SDL numbers them (trimui_inputd's pad), and its d-pad (Deadcast's hat codes)
enum : u32 { PadB = 0, PadA = 1, PadY = 2, PadX = 3, PadL1 = 4, PadR1 = 5, PadSelect = 6, PadStart = 7, PadMenu = 8,
	PadFn1 = 9, PadFn2 = 10, PadMinus = 13, PadPlus = 14, PadUp = 256, PadDown, PadLeft, PadRight };
// and its triggers, as axes
enum : u32 { AxisL2 = 2, AxisR2 = 5 };

// NextUI's "MinUI" palette
constexpr u32 ColorMain = 0xffffff;			// the selected row's pill, the buttons' circles
constexpr u32 ColorAccent = 0x262626;		// the pills behind the title, the hints, the selected option
constexpr u32 ColorAccent2 = 0x999999;		// the buttons' letters
constexpr u32 ColorSelectedText = 0x000000;
constexpr u32 ColorHint = 0xffffff;			// the title, the hints
constexpr u32 ColorBackground = 0x000000;

// Noto Sans Bold (fonts/, in the binary) at NextUI's sizes
struct Fonts
{
	FT_Library library = nullptr;
	std::unique_ptr<u8[]> data;
	size_t size = 0;
	Font large;		// the main list, messages
	Font medium;	// a button's letter
	Font small;		// options, hints
	Font tiny;		// values, descriptions
	Font page;		// the start page's line
	Font pageText;	// and the text under it
};
Fonts *fonts;

bool loadFonts()
{
	if (fonts != nullptr)
		return true;
	auto f = std::make_unique<Fonts>();
	f->data = resource::load("fonts/NotoSans-Bold.ttf", f->size);
	if (f->data == nullptr || FT_Init_FreeType(&f->library) != 0)
	{
		ERROR_LOG(COMMON, "Menu: no font");
		return false;
	}
	if (!f->large.init(f->library, f->data.get(), f->size, S(16)) || !f->medium.init(f->library, f->data.get(), f->size, S(14))
			|| !f->small.init(f->library, f->data.get(), f->size, S(12)) || !f->tiny.init(f->library, f->data.get(), f->size, S(10))
			|| !f->page.init(f->library, f->data.get(), f->size, 44) || !f->pageText.init(f->library, f->data.get(), f->size, 28))
	{
		ERROR_LOG(COMMON, "Menu: the font cannot be read");
		return false;
	}
	fonts = f.release();
	return true;
}

// a setting the menu shows: an option of emu.cfg and the values it may take
struct Setting
{
	const char *name;
	const char *desc;
	std::vector<std::pair<const char *, int>> choices;
	config::BaseOption *option;			// saved at every change (the game's own config, if it has one)
	std::function<int()> get;
	std::function<void(int)> set;

	int index() const
	{
		const int v = get();
		for (size_t i = 0; i < choices.size(); i++)
			if (choices[i].second == v)
				return (int)i;
		return 0;
	}
};

Setting choice(const char *name, const char *desc, config::Option<int>& option, std::vector<std::pair<const char *, int>> choices)
{
	return { name, desc, std::move(choices), &option, [&option] { return option.get(); }, [&option](int v) { option.set(v); } };
}

Setting toggle(const char *name, const char *desc, config::Option<bool>& option)
{
	return { name, desc, { { "Off", 0 }, { "On", 1 } }, &option, [&option] { return option.get() ? 1 : 0; },
		[&option](int v) { option.set(v != 0); } };
}

// Options' pages. Not shown: the TV standard and the cable, NTSC and VGA for the Brick's screen
// (emulator.cpp; TV Composite for a game that refuses VGA)
std::vector<Setting>& consoleSettings()
{
	static std::vector<Setting> list {
		choice("Region", "The console's region. Takes effect on restart.", config::Region,
				{ { "Japan", 0 }, { "USA", 1 }, { "Europe", 2 }, { "Default", 3 } }),
		choice("Language", "The console's language. Takes effect on restart.", config::Language,
				{ { "Japanese", 0 }, { "English", 1 }, { "German", 2 }, { "French", 3 }, { "Spanish", 4 }, { "Italian", 5 }, { "Default", 6 } }),
	};
	return list;
}

std::vector<Setting>& videoSettings()
{
	static std::vector<Setting> list {
		toggle("Show FPS", "Shows the frame rate.", config::ShowFPS),
		choice("Frame Skip", "Skips drawing frames when the game falls behind.", config::AutoSkipFrame,
				{ { "Off", 0 }, { "Normal", 1 }, { "Maximum", 2 } }),
		choice("Texture Filter", "How the game's textures are smoothed.", config::TextureFiltering,
				{ { "Default", 0 }, { "Nearest", 1 }, { "Linear", 2 } }),
	};
	return list;
}

std::vector<Setting>& audioSettings()
{
	static std::vector<Setting> list {
		toggle("Audio DSP", "The sound chip's effects, such as reverb. Costs speed.", config::DSPEnabled),
		toggle("VMU Sounds", "The VMU's beeps.", config::VmuSound),
	};
	return list;
}

// the Dreamcast's buttons and the emulator's own, as the pad's mapping has them
struct Control
{
	DreamcastKey key;
	const char *name;
	const char *desc = nullptr;		// else the list's
};
const std::vector<Control> Controls {
	{ DC_DPAD_UP, "Up" }, { DC_DPAD_DOWN, "Down" }, { DC_DPAD_LEFT, "Left" }, { DC_DPAD_RIGHT, "Right" },
	{ DC_BTN_A, "A Button" }, { DC_BTN_B, "B Button" }, { DC_BTN_X, "X Button" }, { DC_BTN_Y, "Y Button" },
	{ DC_BTN_START, "Start" }, { DC_AXIS_LT, "L Trigger" }, { DC_AXIS_RT, "R Trigger" },
	{ EMU_BTN_SAVESTATE, "Save State" }, { EMU_BTN_LOADSTATE, "Load State" }, { EMU_BTN_SCREENSHOT, "Screenshot" },
	{ EMU_BTN_DPAD_STICK, "Swap D-Pad", "In game, makes the d-pad the analog stick, and the d-pad again." },
};
// in the mapping but not listed (R2): a control bound in Controls is taken from it too
const std::vector<Control> Unlisted {
	{ EMU_BTN_FFORWARD, "Fast Forward" },
};

// the Brick's pad (not SDL's keyboard)
std::shared_ptr<GamepadDevice> pad()
{
	return GamepadDevice::GetGamepad<SDLGamepad>();
}

std::string inputName(const InputMapping::InputDef& input)
{
	if (input.is_axis())
	{
		if (input.code == AxisL2 && input.type == InputMapping::InputDef::InputType::AXIS_POS)
			return "L2";
		if (input.code == AxisR2 && input.type == InputMapping::InputDef::InputType::AXIS_POS)
			return "R2";
		return "AXIS " + std::to_string(input.code) + input.get_suffix();
	}
	switch (input.code)
	{
	case PadB: return "B";
	case PadA: return "A";
	case PadY: return "Y";
	case PadX: return "X";
	case PadL1: return "L1";
	case PadR1: return "R1";
	case PadSelect: return "SELECT";
	case PadStart: return "START";
	case PadMenu: return "MENU";
	case PadFn1: return "FN1";
	case PadFn2: return "FN2";
	case PadMinus: return "MINUS";
	case PadPlus: return "PLUS";
	case PadUp: return "UP";
	case PadDown: return "DOWN";
	case PadLeft: return "LEFT";
	case PadRight: return "RIGHT";
	default: return "BUTTON " + std::to_string(input.code);
	}
}

// a function's controls: buttons (and combos of them), or a trigger (the mapping's axes)
std::string bindingName(DreamcastKey key)
{
	std::shared_ptr<GamepadDevice> gamepad = pad();
	if (gamepad == nullptr)
		return "NONE";
	std::shared_ptr<InputMapping> mapping = gamepad->get_input_mapping();
	std::string name;
	for (const InputMapping::InputDef& input : mapping->get_button_combo(0, key).inputs)
		name += (name.empty() ? "" : "+") + inputName(input);
	const std::pair<u32, bool> axis = mapping->get_axis_code(0, key);
	if (axis.first != InputMapping::InputDef::INVALID_CODE)
		name += (name.empty() ? "" : "+") + inputName(InputMapping::InputDef::from_axis(axis.first, axis.second));
	return name.empty() ? "NONE" : name;
}

// one control does one thing: taken from whatever had it
void bind(DreamcastKey key, const InputMapping::InputDef& input)
{
	std::shared_ptr<GamepadDevice> gamepad = pad();
	if (gamepad == nullptr)
		return;
	std::shared_ptr<InputMapping> mapping = gamepad->get_input_mapping();
	for (const std::vector<Control> *list : { &Controls, &Unlisted })
		for (const Control& control : *list)
		{
			if (control.key == key)
				continue;
			if (mapping->get_button_combo(0, control.key).inputs.contains(input))
				mapping->clear_button(0, control.key);
			if (input.is_axis() && mapping->get_axis_code(0, control.key) == std::make_pair(input.code, input.type == InputMapping::InputDef::InputType::AXIS_POS))
				mapping->clear_axis(0, control.key);
		}
	mapping->clear_button(0, key);
	mapping->clear_axis(0, key);
	if (input.is_axis())
		mapping->set_axis(0, key, input.code, input.type == InputMapping::InputDef::InputType::AXIS_POS);
	else
		mapping->set_button(0, key, InputMapping::ButtonCombo{ InputMapping::InputSet{ input }, false });
	gamepad->save_mapping();
}

void unbind(DreamcastKey key)
{
	std::shared_ptr<GamepadDevice> gamepad = pad();
	if (gamepad == nullptr)
		return;
	gamepad->get_input_mapping()->clear_button(0, key);
	gamepad->get_input_mapping()->clear_axis(0, key);
	gamepad->save_mapping();
}

// the game's name as NextUI shows a file's: without its extensions or trailing (...) and [...]
std::string displayName()
{
	const char *rom = std::getenv("DEADCAST_ROM");
	const std::string path = rom != nullptr && *rom != '\0' ? rom : settings.content.path;
	const size_t slash = path.find_last_of('/');
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	for (size_t dot; (dot = name.rfind('.')) != std::string::npos && name.size() - dot > 2 && name.size() - dot <= 5; )
		name.erase(dot);
	const std::string whole = name;
	for (size_t paren; (paren = name.find_last_of("([")) != std::string::npos && paren > 0; )
		name.erase(paren);
	name.erase(name.find_last_not_of(' ') + 1);
	if (name.empty())
		name = whole;
	return name.empty() ? settings.content.title : name;
}


// A row of a page: does something (A), picks one of its values (Left/Right), or shows what a control is
// bound to
enum class Row { Action, Choice, Binding };
struct Item
{
	std::string name;
	std::string desc;
	Row row = Row::Action;
	std::vector<std::string> values;	// Choice: what it may be; Binding: what it is bound to
	int value = 0;
	std::function<void(int)> change;	// Choice: a value picked
	std::function<void()> confirm;		// A
	std::function<void(int)> shift;		// Left/Right on an Action, -1 or 1 (Save, Load: the slot)
	std::function<bool()> available;	// greyed while false (a session's Continue)
	bool preview = false;				// the slot's saved state beside it (Save, Load)
	bool compact = false;				// a Choice's value after its name, in its pill (Core)
	DreamcastKey key = EMU_BTN_NONE;	// Binding: what it is
};
// Every page of the menu looks like the first: its title in a pill at the top, its rows (or the link's
// status), the buttons at the bottom right. Opening or leaving a page slides the rows; the pills stay in
// place, the new page's at once.
enum class Kind { List, Link };
struct Screen
{
	Kind kind = Kind::List;
	std::string title;
	std::vector<Item> items;
	std::string desc;					// unless the selected item has its own
	int selected = 0;
	int start = 0;						// the first row shown
	std::function<void()> back;			// B, and MENU on the first page; else the page before again
};

// a paused two-player session's rows (no saving or loading: the two Bricks must stay alike)
enum SessionItem { SessionContinue, SessionLeave, SessionQuit };
enum class Busy { None, Save, Load };

struct State
{
	bool open = false;
	Canvas canvas;
	Canvas background;				// the game's frame blurred, behind every page
	std::vector<u8> frame;			// the game's, when the menu opened (a saved state's picture)
	int frameWidth = 0;
	int frameHeight = 0;
	int slot = 0;
	std::vector<Screen> screens;	// the pages open, the first (main, or a session's) at the bottom
	// a page opened (sliding in from the right, the one before out to the left) or left (the other way)
	bool sliding = false;
	bool slideBack = false;
	u64 slideStart = 0;
	std::unique_ptr<Screen> leaving;	// the page left, until it is off the screen
	std::string message;			// shown over everything until A or B
	int awaiting = -1;				// a Binding row waiting for a control
	Busy busy = Busy::None;			// Save or Load: its screen shown, then done (it takes seconds)
	std::string biosFile;			// the missing BIOS page: the file it asks for
	bool session = false;			// a paused two-player session's menu (openSession)
	netplay::PauseState shownPause;	// the pause it shows
	u32 linkVersion = 0;			// the link's status its page shows
	Result result = Result::Open;
	bool dirty = true;
	u32 held = 0;					// a direction held: repeated
	u64 repeatAt = 0;
	int previewSlot = -1;			// the slot shown in the preview
	bool stateExists = false;
	std::vector<u8> preview;		// its picture
	int previewWidth = 0;
	int previewHeight = 0;
};
State st;

// GSAP's power1.out: fast, then slowing to a stop
float easeOut(float t)
{
	return 1.f - (1.f - t) * (1.f - t);
}

void push(Screen screen)
{
	st.screens.push_back(std::move(screen));
	st.leaving.reset();
	st.sliding = true;
	st.slideBack = false;
	st.slideStart = getTimeMs();
}

void pop()
{
	st.leaving = std::make_unique<Screen>(std::move(st.screens.back()));
	st.screens.pop_back();
	st.sliding = true;
	st.slideBack = true;
	st.slideStart = getTimeMs();
}

bool linkShown()
{
	return !st.screens.empty() && st.screens.back().kind == Kind::Link;
}

void move(Screen& screen, int step)
{
	const int count = (int)screen.items.size();
	if (count == 0)
		return;
	screen.selected = (screen.selected + step + count) % count;
	if (screen.selected < screen.start)
		screen.start = screen.selected;
	if (screen.selected >= screen.start + MaxRows)
		screen.start = screen.selected - MaxRows + 1;
}

Item entry(const std::string& name, const std::string& desc, std::function<void()> confirm)
{
	Item item;
	item.name = name;
	item.desc = desc;
	item.confirm = std::move(confirm);
	return item;
}

Screen settingsScreen(const std::string& title, std::vector<Setting>& list)
{
	Screen screen;
	screen.title = title;
	for (Setting& setting : list)
	{
		Item item;
		item.name = setting.name;
		item.desc = setting.desc;
		item.row = Row::Choice;
		for (const auto& choice : setting.choices)
			item.values.push_back(choice.first);
		item.value = setting.index();
		Setting *s = &setting;
		item.change = [s](int i) {
			s->set(s->choices[i].second);
			s->option->save();
		};
		screen.items.push_back(std::move(item));
	}
	return screen;
}

void refreshBindings(Screen& screen)
{
	for (Item& item : screen.items)
	{
		item.values = { bindingName(item.key) };
		item.value = 0;
	}
}

Screen bindingsScreen()
{
	Screen screen;
	screen.title = "Controls";
	for (const Control& control : Controls)
	{
		Item item;
		item.name = control.name;
		item.desc = control.desc != nullptr ? control.desc : "";
		item.row = Row::Binding;
		item.key = control.key;
		screen.items.push_back(std::move(item));
	}
	refreshBindings(screen);
	return screen;
}

// the link's page, the link looking for the other Brick
void openLink(link::Via via)
{
	Screen screen;
	screen.kind = Kind::Link;
	screen.title = via == link::Via::Bluetooth ? "Bluetooth" : "USB Link";
	push(std::move(screen));
	st.linkVersion = ~0u;
	// for the other Brick to check (two players only in the same game), and this player's VMU to swap
	link::setGame(settings.content.gameId, hostfs::getVmuPath("A1", false));
	link::start(via);
}

// two players on two Bricks: how the Bricks connect
Screen localMultiplayerScreen()
{
	Screen screen;
	screen.title = "Local Multiplayer";
	screen.items.push_back(entry("USB Link", "Play with another Brick over a USB cable. In testing.", [] { openLink(link::Via::Usb); }));
	screen.items.push_back(entry("Bluetooth", "Play with another Brick nearby, no cable. In testing.", [] { openLink(link::Via::Bluetooth); }));
	return screen;
}

Screen optionsScreen()
{
	Screen screen;
	screen.title = "Options";
	screen.desc = "Deadcast " DEADCAST_VERSION;
	screen.items.push_back(entry("Console", "The Dreamcast's region and language.", [] { push(settingsScreen("Console", consoleSettings())); }));
	screen.items.push_back(entry("Video", "", [] { push(settingsScreen("Video", videoSettings())); }));
	screen.items.push_back(entry("Audio", "", [] { push(settingsScreen("Audio", audioSettings())); }));
	screen.items.push_back(entry("Controls", "", [] { push(bindingsScreen()); }));
	screen.items.push_back(entry("Local Multiplayer", "Two players on two Bricks.", [] { push(localMultiplayerScreen()); }));
	return screen;
}

// the slot kept for the next time, and for the save and load buttons
void keepSlot()
{
	config::SavestateSlot = st.slot;
	config::setAutoSave(false);
	config::SavestateSlot.save();
	config::setAutoSave(true);
}

bool saveState()
{
	if (!dc_savestateAllowed())
	{
		st.message = "The game cannot be saved now.";
		return false;
	}
	if (netplay::twoPlayerRun())
	{
		st.message = "No save states in a game started as two players.\nIts own saves (on the VMU) work.";
		return false;
	}
	// its picture: the frame the menu opened over
	std::vector<u8> png;
	if (!st.frame.empty())
	{
		stbi_flip_vertically_on_write(0);
		stbi_write_png_to_func([](void *context, void *data, int size) {
			std::vector<u8>& v = *(std::vector<u8> *)context;
			v.insert(v.end(), (u8 *)data, (u8 *)data + size);
		}, &png, st.frameWidth, st.frameHeight, 3, st.frame.data(), 0);
	}
	try {
		dc_savestate(st.slot, png.empty() ? nullptr : png.data(), (u32)png.size());
	} catch (const FlycastException& e) {
		ERROR_LOG(SAVESTATE, "Menu: saving to slot %d: %s", st.slot + 1, e.what());
		st.message = "The game could not be saved.";
		return false;
	}
	keepSlot();
	return true;
}

bool loadState()
{
	try {
		dc_loadstate(st.slot);
	} catch (const FlycastException& e) {
		ERROR_LOG(SAVESTATE, "Menu: loading slot %d: %s", st.slot + 1, e.what());
		st.message = "The game could not be loaded.";
		return false;
	}
	keepSlot();
	return true;
}

void updatePreview()
{
	if (st.previewSlot == st.slot)
		return;
	st.previewSlot = st.slot;
	st.preview.clear();
	st.stateExists = dc_getStateCreationDate(st.slot) != 0;
	if (!st.stateExists)
		return;
	std::vector<u8> png;
	dc_getStateScreenshot(st.slot, png);
	if (png.empty())
		return;
	int channels;
	stbi_set_flip_vertically_on_load_thread(0);
	u8 *rgb = stbi_load_from_memory(png.data(), (int)png.size(), &st.previewWidth, &st.previewHeight, &channels, STBI_rgb);
	if (rgb == nullptr)
		return;
	st.preview.assign(rgb, rgb + (size_t)st.previewWidth * st.previewHeight * 3);
	stbi_image_free(rgb);
}

Screen mainScreen()
{
	Screen screen;
	screen.title = displayName();
	screen.back = [] { st.result = Result::Resume; };
	screen.items.push_back(entry("Continue", "", [] { st.result = Result::Resume; }));
	// what runs the game: Deadcast's emulation, or a recompiled build of it (to come: for now Emulator,
	// whichever is picked)
	Item core;
	core.name = "Core";
	core.desc = "Recomp: a recompiled build of this game, where there is one. To come.";
	core.row = Row::Choice;
	core.values = { "Emulator", "Recomp" };
	core.compact = true;
	screen.items.push_back(std::move(core));
	Item save = entry("Save", "", [] { st.busy = Busy::Save; });
	Item load = entry("Load", "", [] {
		updatePreview();
		if (st.stateExists)
			st.busy = Busy::Load;
	});
	for (Item *item : { &save, &load })
	{
		item->shift = [](int step) { st.slot = (st.slot + step + SlotCount) % SlotCount; };
		item->preview = true;
	}
	screen.items.push_back(std::move(save));
	screen.items.push_back(std::move(load));
	screen.items.push_back(entry("Options", "", [] { push(optionsScreen()); }));
	screen.items.push_back(entry("Quit", "", [] { st.result = Result::Quit; }));
	return screen;
}

// why the session is paused, for the title
std::string sessionTitle()
{
	switch (st.shownPause.pause)
	{
	case netplay::Pause::PeerMenu:
		return "Paused by the other player";
	case netplay::Pause::Lost:
		return st.shownPause.peerPresent ? "The other player is back" : "Lost the other Brick";
	default:
		return "Two players: paused";
	}
}

bool canContinue()
{
	return st.shownPause.peerPresent;
}

void continueSession()
{
	netplay::resume();
	st.result = Result::Resume;
}

Screen sessionScreen()
{
	Screen screen;
	screen.title = sessionTitle();
	screen.back = [] {
		if (canContinue())
			continueSession();
	};
	Item resume = entry("Continue", "", [] {
		if (canContinue())
			continueSession();
		else
			st.message = "The other Brick is not there.\nIt can come back while this is open.";
	});
	resume.available = canContinue;
	screen.items.push_back(std::move(resume));
	// on alone, the game as it is (the other Brick told)
	screen.items.push_back(entry("Leave two-player game", "", [] {
		netplay::leave();
		st.result = Result::Resume;
	}));
	screen.items.push_back(entry("Quit", "", [] { st.result = Result::Quit; }));
	screen.selected = canContinue() ? SessionContinue : SessionLeave;
	return screen;
}

void actList(u32 code)
{
	Screen& screen = st.screens.back();
	if (code == PadB || (code == PadMenu && st.screens.size() == 1))
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
	if (screen.items.empty())
		return;
	Item& item = screen.items[screen.selected];
	switch (code)
	{
	case PadUp:
		move(screen, -1);
		break;
	case PadDown:
		move(screen, 1);
		break;
	case PadLeft:
	case PadRight:
	{
		const int step = code == PadLeft ? -1 : 1;
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
	case PadA:
		if (item.row == Row::Binding)
			st.awaiting = screen.selected;
		else if (item.confirm)
		{
			// it may change the pages, this one's items with them
			const std::function<void()> confirm = item.confirm;
			confirm();
		}
		break;
	case PadX:
		if (item.row == Row::Binding)
		{
			unbind(item.key);
			refreshBindings(screen);
			move(screen, 1);
		}
		break;
	}
}

// linked: A tests again
bool canTest(const link::Status& status)
{
	return status.role != link::Role::None && status.state != link::State::Testing;
}

// B leaves (the link down, unless a session keeps it), A tests again
void actLink(u32 code)
{
	if (code == PadB)
	{
		if (!link::kept())
			link::stop();
		pop();
	}
	else if (code == PadA && canTest(link::status()))
		link::test();
}

// linked, tested, IP up, the same game on both: both start it again as two players
bool canStart(const link::Status& status)
{
	return status.state == link::State::Tested && status.role != link::Role::None && !status.address.empty()
			&& status.sameGame;
}

void act(u32 code)
{
	if (!st.biosFile.empty())
	{
		if (code == PadA || code == PadB)
			st.result = Result::Quit;
		return;
	}
	if (st.busy != Busy::None || st.screens.empty())
		return;
	st.dirty = true;
	if (!st.message.empty())
	{
		if (code == PadA || code == PadB)
			st.message.clear();
	}
	else if (linkShown())
		actLink(code);
	else
		actList(code);
}

// the control pressed for the row waiting for one
void bindAwaited(const InputMapping::InputDef& input)
{
	Screen& screen = st.screens.back();
	bind(screen.items[st.awaiting].key, input);
	refreshBindings(screen);
	st.awaiting = -1;
	move(screen, 1);
	st.dirty = true;
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
	const int w = std::strlen(button) == 1 ? ButtonSize : ButtonSize / 2 + fonts->tiny.measure(button);
	return w + ButtonMargin + fonts->small.measure(hint);
}

void drawButton(const char *button, const char *hint, int x, int y)
{
	Canvas& c = st.canvas;
	int ox;
	if (std::strlen(button) == 1)
	{
		c.circle(x + ButtonSize / 2, y + ButtonSize / 2, ButtonSize / 2, ColorMain);
		Font& medium = fonts->medium;
		c.text(medium, button, x + (ButtonSize - medium.measure(button)) / 2, baseline(medium, y, ButtonSize), ColorAccent2);
		ox = ButtonSize;
	}
	else
	{
		Font& tiny = fonts->tiny;
		const int w = tiny.measure(button);
		c.pill(x, y, ButtonSize / 2 + w, ButtonSize, ColorMain);
		c.text(tiny, button, x + ButtonSize / 4, baseline(tiny, y, ButtonSize), ColorAccent2);
		ox = ButtonSize / 2 + w;
	}
	ox += ButtonMargin;
	c.text(fonts->small, hint, x + ox, baseline(fonts->small, y, ButtonSize), ColorHint);
}

using Buttons = std::vector<std::pair<const char *, const char *>>;

// button and hint pairs in a pill at the bottom right of a page drawn at ox; the pill's width
int drawButtons(const Buttons& pairs, int ox)
{
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

// lines of text centered in a rectangle
void drawMessage(Font& font, const std::string& message, int x, int y, int w, int h)
{
	std::vector<std::string> lines;
	for (size_t start = 0, end; start <= message.size(); start = end + 1)
	{
		end = message.find('\n', start);
		if (end == std::string::npos)
			end = message.size();
		lines.push_back(message.substr(start, end - start));
	}
	int top = y + (h - font.lineHeight() * (int)lines.size()) / 2;
	for (const std::string& line : lines)
	{
		st.canvas.text(font, line, x + (w - font.measure(line)) / 2, top + font.ascent(), White);
		top += font.lineHeight();
	}
}

// text broken at spaces into lines no wider than width (a longer word on its own)
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

// the selected slot's saved state, at the right of a page drawn at ox, the slots under it
void drawPreview(int ox)
{
	Canvas& c = st.canvas;
	const int hw = c.width() / 2;
	const int hh = c.height() / 2;
	const int pw = hw + S(4 * 2);
	const int ph = hh + S(4 * 2 + 6 + 4);
	int x = ox + c.width() - pw - Padding;
	int y = (c.height() - ph) / 2;
	c.roundRect(x, y, pw, ph, S(4), White);
	x += S(4);
	y += S(4);
	c.fill(x, y, hw, hh, Black);
	updatePreview();
	if (!st.preview.empty())
		c.picture(st.preview.data(), st.previewWidth, st.previewHeight, x, y, hw, hh);
	else
		drawMessage(fonts->large, st.stateExists ? "No Preview" : "Empty Slot", x, y, hw, hh);
	// the slots: the one picked a black dot, the others small gray ones
	const int px = x + (hw - S(15 * SlotCount)) / 2;
	const int py = y + hh + S(4);
	for (int i = 0; i < SlotCount; i++)
	{
		const int cx = px + S(15 * i) + S(15) / 2;
		const int cy = py + S(3);
		if (i == st.slot)
			c.circle(cx, cy, S(3), Black);
		else
			c.circle(cx, cy, S(1), LightGray);
	}
}

// the selected row's description at the bottom left, beside the buttons' pill (width wide), on its line
void drawDescription(const std::string& desc, int width)
{
	if (desc.empty())
		return;
	Canvas& c = st.canvas;
	Font& tiny = fonts->tiny;
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
	Font& large = fonts->large;
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
		if (item.row != Row::Action && item.value < (int)item.values.size() && !(selected && st.awaiting >= 0))
			value = item.values[item.value];
		// Left and Right change the selected Choice
		if (selected && item.row == Row::Choice)
			value = "<  " + value + "  >";
		std::string label = item.name;
		if (item.compact)
		{
			label += "  " + value;
			value.clear();
		}
		const int valueWidth = value.empty() ? 0 : large.measure(value) + ButtonPadding * 2;
		const std::string name = Canvas::fit(large, label, right - left - valueWidth - ButtonPadding * 2);
		u32 color = item.available && !item.available() ? LightGray : White;
		if (selected)
		{
			if (item.row != Row::Action && !item.compact)
				c.pill(left, rowTop - overhang, right - left, PillSize, ColorAccent);
			c.pill(left, rowTop - overhang, large.measure(name) + ButtonPadding * 2, PillSize, ColorMain);
			color = ColorSelectedText;
		}
		c.text(large, name, left + ButtonPadding, baseline(large, rowTop, RowPitch), color);
		if (!value.empty())
			c.text(large, value, right - valueWidth + ButtonPadding, baseline(large, rowTop, RowPitch), White);
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
	if (count > 0 && screen.items[screen.selected].preview)
		drawPreview(ox);
}

// the link's page: what it is doing, and the last test's numbers
void drawLink(int ox)
{
	Canvas& c = st.canvas;
	Font& large = fonts->large;
	Font& small = fonts->small;
	const link::Status s = link::status();
	std::string title;
	std::vector<std::string> unders;
	switch (s.state)
	{
	case link::State::Off:
	case link::State::Waiting:
		title = "Looking for the other Brick";
		if (!s.activity.empty())
			unders.push_back(s.activity);
		if (s.via == link::Via::Bluetooth)
			unders.push_back("Open Local Multiplayer, Bluetooth on the other Brick too, and keep the two close.");
		else if (s.topPortInUse)
			unders.push_back("Something is on this Brick's top port: this Brick hosts there.");
		else
			unders.push_back("Cable the two Bricks: top port to bottom port, or bottom to bottom. Open USB Link on both.");
		break;
	case link::State::Testing:
		title = "Testing the link...";
		break;
	case link::State::Tested:
		title = canStart(s) ? "Starting two players..." : "Linked";
		if (!s.sameGame)
			unders.push_back("The other Brick is playing another game. Start the same game on both.");
		break;
	case link::State::Failed:
		title = "The link failed";
		break;
	}
	if (s.role != link::Role::None)
		unders.push_back("To Brick " + s.peer + " over " + s.port + ": this Brick is player "
				+ (s.role == link::Role::Host ? "1" : "2"));
	if (s.state == link::State::Tested)
		for (size_t start = 0, end; start < s.result.size(); start = end + 1)
		{
			end = s.result.find('\n', start);
			if (end == std::string::npos)
				end = s.result.size();
			unders.push_back(s.result.substr(start, end - start));
		}
	if (!s.address.empty())
		unders.push_back("IP over the link: " + s.address + " to " + s.peerAddress);
	if (!s.error.empty())
		unders.push_back(s.error);

	// left-aligned with the pages' rows, in the middle of the room between the title and the buttons
	const int x = ox + Padding + ButtonPadding;
	const int width = c.width() - (Padding + ButtonPadding) * 2;
	const std::vector<std::string> lines = wrap(large, title, width);
	std::vector<std::string> wrapped;
	for (const std::string& under : unders)
		for (const std::string& l : wrap(small, under, width))
			wrapped.push_back(l);
	const int gap = wrapped.empty() ? 0 : S(4);
	const int height = (int)lines.size() * large.lineHeight() + gap + (int)wrapped.size() * small.lineHeight();
	const int top = Padding + PillSize;
	int y = top + (c.height() - Padding - PillSize - top - height) / 2;
	for (const std::string& l : lines)
	{
		c.text(large, l, x, y + large.ascent(), White);
		y += large.lineHeight();
	}
	y += gap;
	for (const std::string& l : wrapped)
	{
		c.text(small, l, x, y + small.ascent(), 0xb4b4b4);
		y += small.lineHeight();
	}
}

// a page's rows (or the link's status) with their left edge at ox, off the screen while they slide
void drawBody(const Screen& screen, int ox)
{
	if (ox <= -st.canvas.width() || ox >= st.canvas.width())
		return;
	if (screen.kind == Kind::Link)
		drawLink(ox);
	else
		drawList(screen, ox);
}

// what a page's buttons do: the link's by where it is, a list's by its selected row
Buttons pageButtons(const Screen& screen)
{
	Buttons buttons { { "B", "BACK" } };
	if (screen.kind == Kind::Link)
	{
		if (canTest(link::status()))
			buttons.push_back({ "A", "TEST" });
	}
	else if (!screen.items.empty() && screen.items[screen.selected].row == Row::Binding)
	{
		buttons.push_back({ "X", "CLEAR" });
		buttons.push_back({ "A", "SET" });
	}
	else if (screen.items.empty() || screen.items[screen.selected].row == Row::Action)
		buttons.push_back({ "A", "OKAY" });
	return buttons;
}

// a page's title in its pill, its buttons, its selected row's description: in place
void drawPills(const Screen& screen)
{
	Canvas& c = st.canvas;
	Font& large = fonts->large;
	const int maxWidth = c.width() - Padding * 2;
	const std::string title = Canvas::fit(large, screen.title, maxWidth - ButtonPadding * 2);
	c.pill(Padding, Padding, std::min(maxWidth, large.measure(title) + ButtonPadding * 2), PillSize, ColorAccent);
	c.text(large, title, Padding + ButtonPadding, baseline(large, Padding, PillSize), ColorHint);
	const int buttonsWidth = drawButtons(pageButtons(screen), 0);
	if (screen.kind == Kind::List)
	{
		const std::string& desc = !screen.items.empty() && !screen.items[screen.selected].desc.empty()
				? screen.items[screen.selected].desc : screen.desc;
		drawDescription(desc, c.width() - Padding * 3 - buttonsWidth);
	}
}

// a page with a line in white and text under it in grey, centered
void drawPage(const std::string& line, const std::string& under)
{
	Canvas& c = st.canvas;
	Font& big = fonts->page;
	Font& text = fonts->pageText;
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
		c.text(text, l, (c.width() - text.measure(l)) / 2, y + text.ascent(), 0xb4b4b4);
		y += text.lineHeight();
	}
}

// the disc being started, as its file is named
std::string discName()
{
	const std::string& path = settings.content.path;
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// what a BIOS file is, by its name
std::string biosName(const std::string& file)
{
	if (file == "dc_boot.bin")
		return "the Dreamcast BIOS";
	if (file == "naomi.zip")
		return "the NAOMI BIOS";
	if (file == "naomi2.zip")
		return "the NAOMI 2 BIOS";
	if (file == "awbios.zip")
		return "the Atomiswave BIOS";
	return file;
}

// where a BIOS goes: the first of the BIOS folders (the pak's bios/), in full
std::string biosFolder()
{
	const char *bios = std::getenv("DEADCAST_BIOS_PATH");
	const std::string list = bios != nullptr ? bios : "";
	return list.substr(0, list.find(':'));
}

// which BIOS file to copy where, then how to leave
void drawBiosPage()
{
	Canvas& c = st.canvas;
	Font& big = fonts->page;
	Font& text = fonts->pageText;
	const int width = std::min(c.width() - 120, 904);
	const std::vector<std::string> title = wrap(big, "Deadcast needs " + biosName(st.biosFile), width);
	const std::vector<std::string> body = wrap(text, "Copy " + st.biosFile + " into this folder:", width);
	const std::string folder = biosFolder();
	const int bar = text.lineHeight() + 16;
	const int height = (int)title.size() * big.lineHeight() + 18 + (int)body.size() * text.lineHeight() + 14 + bar + 36
			+ text.lineHeight();
	int y = (c.height() - height) / 2;
	auto line = [&](Font& font, const std::string& s, u32 rgb) {
		c.text(font, s, (c.width() - font.measure(s)) / 2, y + font.ascent(), rgb);
		y += font.lineHeight();
	};
	c.clear(Black);
	for (const std::string& s : title)
		line(big, s, White);
	y += 18;
	for (const std::string& s : body)
		line(text, s, 0xb4b4b4);
	y += 14;
	const int barWidth = text.measure(folder) + 48;
	c.roundRect((c.width() - barWidth) / 2, y, barWidth, bar, 12, 0x2a2a2a);
	y += 8;
	line(text, folder, White);
	y += 8 + 36;
	line(text, "Press A or B to exit", White);
}

void draw()
{
	Canvas& c = st.canvas;
	if (!st.biosFile.empty())
		drawBiosPage();
	else if (st.busy != Busy::None)
		drawPage(st.busy == Busy::Save ? "Saving..." : "Loading...", discName());
	else
	{
		c.copy(st.background);
		if (!st.message.empty())
		{
			drawMessage(fonts->large, st.message, 0, 0, c.width(), c.height());
			drawButtons({ { "A", "OKAY" } }, 0);
		}
		else if (!st.screens.empty())
		{
			const Screen& screen = st.screens.back();
			if (st.sliding && (st.slideBack ? st.leaving != nullptr : st.screens.size() > 1))
			{
				// opened: both pages' rows to the left, the new ones in from the right; left: to the right
				const float t = easeOut(std::min(1.f, (float)(getTimeMs() - st.slideStart) / SlideMs));
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
	GraphicsContext::Instance()->showPicture(c.data());
}

void close(bool resume)
{
	if (linkShown() && !link::kept())
		link::stop();
	GraphicsContext::Instance()->endPictures(resume ? GraphicsContext::AfterPictures::LastFrame
			: GraphicsContext::AfterPictures::Stay);
	st.open = false;
	st.session = false;
	st.frame.clear();
	st.preview.clear();
	st.screens.clear();
	st.leaving.reset();
	st.sliding = false;
	st.biosFile.clear();
}

bool starting = false;
bool startingTwoPlayers = false;	// the start page says so

}	// namespace

bool showStart()
{
	if (starting)
		return true;
	GraphicsContext *context = GraphicsContext::Instance();
	int width, height;
	if (context == nullptr || st.open || !loadFonts() || !context->beginPictures(width, height))
		return false;
	st.canvas.resize(width, height);
	drawPage("Loading...", discName());
	context->showPicture(st.canvas.data());
	starting = true;
	return true;
}

void showStartError(const std::string& what)
{
	if (!starting)
		return;
	drawPage("Cannot start " + displayName(), what);
	GraphicsContext::Instance()->showPicture(st.canvas.data());
	std::this_thread::sleep_for(std::chrono::seconds(3));
}

bool showBiosMissing(const std::string& file)
{
	if (!starting)
	{
		GraphicsContext *context = GraphicsContext::Instance();
		int width, height;
		if (context == nullptr || st.open || !loadFonts() || !context->beginPictures(width, height))
			return false;
		st.canvas.resize(width, height);
	}
	starting = false;
	st.biosFile = file;
	st.screens.clear();
	st.leaving.reset();
	st.sliding = false;
	st.message.clear();
	st.awaiting = -1;
	st.busy = Busy::None;
	st.held = 0;
	st.result = Result::Open;
	st.dirty = true;
	st.open = true;
	return true;
}

bool showStartTwoPlayers()
{
	if (!starting)
		return false;
	if (!startingTwoPlayers)
	{
		drawPage("Starting two players...", "Waiting for the other Brick");
		GraphicsContext::Instance()->showPicture(st.canvas.data());
		startingTwoPlayers = true;
	}
	return true;
}

void endStart()
{
	if (!starting)
		return;
	GraphicsContext::Instance()->endPictures(GraphicsContext::AfterPictures::NextFrame);
	starting = startingTwoPlayers = false;
}

bool open()
{
	if (st.open)
		return true;
	GraphicsContext *context = GraphicsContext::Instance();
	if (context == nullptr || !loadFonts())
		return false;
	// the frame on the screen, before the menu takes it: the pages' background and a saved state's picture
	st.frameWidth = st.frameHeight = 0;
	if (renderer == nullptr || !renderer->GetLastFrame(st.frame, st.frameWidth, st.frameHeight))
		st.frame.clear();
	int width, height;
	if (!context->beginPictures(width, height))
	{
		st.frame.clear();
		return false;
	}
	st.canvas.resize(width, height);
	// blurred (a spread, the standard deviation, of shrink * sqrt(radius * (radius + 1)): about 13 of the
	// frame's pixels) and darkened, so that white text reads over a white screen too; scaled to the screen
	// once, as every page is drawn over it
	int blurredWidth, blurredHeight;
	const std::vector<u8> blurred = blur(st.frame, st.frameWidth, st.frameHeight, 2, 6, blurredWidth, blurredHeight);
	st.background.resize(width, height);
	st.background.clear(ColorBackground);
	if (!blurred.empty())
		st.background.picture(blurred.data(), blurredWidth, blurredHeight, 0, 0, width, height, BackdropBrightness);
	st.session = false;
	st.slot = config::SavestateSlot >= 0 && config::SavestateSlot < SlotCount ? (int)config::SavestateSlot : 0;
	st.screens.clear();
	st.screens.push_back(mainScreen());
	st.leaving.reset();
	st.sliding = false;
	st.message.clear();
	st.awaiting = -1;
	st.busy = Busy::None;
	st.result = Result::Open;
	st.dirty = true;
	st.held = 0;
	st.previewSlot = -1;
	st.open = true;
	return true;
}

bool openSession()
{
	if (!open())
		return false;
	st.session = true;
	st.shownPause = netplay::pauseState();
	st.screens.clear();
	st.screens.push_back(sessionScreen());
	return true;
}

bool isOpen()
{
	return st.open;
}

void button(u32 code, bool pressed)
{
	if (!st.open)
		return;
	if (!pressed)
	{
		if (code == st.held)
			st.held = 0;
		return;
	}
	if (st.awaiting >= 0)
	{
		bindAwaited(InputMapping::InputDef::from_button(code));
		return;
	}
	if (code == PadUp || code == PadDown || code == PadLeft || code == PadRight)
	{
		st.held = code;
		st.repeatAt = getTimeMs() + 300;
	}
	act(code);
}

void axis(u32 code, int value)
{
	// only a trigger pressed, for a row waiting for a control
	if (st.open && st.awaiting >= 0 && value > 16384)
		bindAwaited(InputMapping::InputDef::from_axis(code, true));
}

Result frame()
{
	if (!st.open)
		return Result::Resume;
	// a save or load once its screen is up
	if (st.busy != Busy::None && !st.dirty)
	{
		const bool done = st.busy == Busy::Save ? saveState() : loadState();
		st.busy = Busy::None;
		st.dirty = true;
		if (done)
			st.result = Result::Resume;
	}
	// a session's menu: closed when the session goes on (Continue on the other Brick), drawn again when
	// why it is paused changes (the other Brick gone, back, left)
	if (st.session && st.result == Result::Open)
	{
		const netplay::PauseState now = netplay::pauseState();
		if (now.pause == netplay::Pause::None)
			st.result = Result::Resume;
		else if (now.pause != st.shownPause.pause || now.peerPresent != st.shownPause.peerPresent)
		{
			st.shownPause = now;
			Screen& screen = st.screens.front();
			screen.title = sessionTitle();
			// on Continue, which went away (the other Brick gone): on Leave instead
			if (screen.selected == SessionContinue && !canContinue())
				screen.selected = SessionLeave;
			st.dirty = true;
		}
	}
	// the link's page again whenever its status changes
	if (linkShown())
	{
		const link::Status s = link::status();
		if (s.version != st.linkVersion)
		{
			st.linkVersion = s.version;
			st.dirty = true;
		}
		// linked: the game again on both, as two players, from its start
		if (canStart(s))
		{
			netplay::begin(s.role == link::Role::Host, s.peerAddress, s.inputDelay);
			st.result = Result::Restart;
		}
	}
	const u64 now = getTimeMs();
	// a page sliding: drawn every frame until it is in place
	if (st.sliding)
	{
		if (now - st.slideStart >= SlideMs)
		{
			st.sliding = false;
			st.leaving.reset();
		}
		st.dirty = true;
	}
	if (st.held != 0 && st.result == Result::Open && now >= st.repeatAt)
	{
		st.repeatAt = now + 100;
		act(st.held);
	}
	if (st.result == Result::Open && st.dirty)
	{
		draw();
		st.dirty = false;
	}
	const Result result = st.result;
	if (result != Result::Open)
		close(result == Result::Resume);
	return result;
}

}
