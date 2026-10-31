#include "menu.h"
#include "canvas.h"
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
#include <fstream>
#include <functional>
#include <map>
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
constexpr int OptionPadding = S(8);
constexpr int SlotCount = 10;		// Deadcast's save state slots

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
	config::BaseOption *option;
	std::string section;
	std::string key;
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
	return { name, desc, std::move(choices), &option, option.getSection(), option.getName(),
		[&option] { return option.get(); }, [&option](int v) { option.set(v); } };
}

Setting toggle(const char *name, const char *desc, config::Option<bool>& option)
{
	return { name, desc, { { "Off", 0 }, { "On", 1 } }, &option, option.getSection(), option.getName(),
		[&option] { return option.get() ? 1 : 0; }, [&option](int v) { option.set(v != 0); } };
}

std::vector<Setting>& frontendSettings()
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

std::vector<Setting>& emulatorSettings()
{
	static std::vector<Setting> list {
		choice("Region", "The console's region. Takes effect on restart.", config::Region,
				{ { "Japan", 0 }, { "USA", 1 }, { "Europe", 2 }, { "Default", 3 } }),
		choice("Language", "The console's language. Takes effect on restart.", config::Language,
				{ { "Japanese", 0 }, { "English", 1 }, { "German", 2 }, { "French", 3 }, { "Spanish", 4 }, { "Italian", 5 }, { "Default", 6 } }),
		choice("Broadcast", "The TV standard. Takes effect on restart.", config::Broadcast,
				{ { "NTSC", 0 }, { "PAL", 1 }, { "PAL-M", 2 }, { "PAL-N", 3 }, { "Default", 4 } }),
		choice("Cable", "The video cable. Takes effect on restart.", config::Cable,
				{ { "VGA", 0 }, { "RGB", 2 }, { "Composite", 3 } }),
		toggle("Audio DSP", "The sound chip's effects, such as reverb. Costs speed.", config::DSPEnabled),
		toggle("VMU Sounds", "The VMU's beeps.", config::VmuSound),
	};
	return list;
}

// Deadcast's own defaults: the pak's emu.cfg (DEADCAST_DEFAULTS), as a first run copies it
std::map<std::string, std::string> shippedDefaults()
{
	std::map<std::string, std::string> values;
	const char *path = std::getenv("DEADCAST_DEFAULTS");
	if (path == nullptr)
		return values;
	std::ifstream file(path);
	std::string line, section;
	auto trim = [](std::string s) {
		s.erase(0, s.find_first_not_of(" \t\r"));
		s.erase(s.find_last_not_of(" \t\r") + 1);
		return s;
	};
	while (std::getline(file, line))
	{
		line = trim(line);
		if (line.size() > 2 && line[0] == '[')
			section = line.substr(1, line.find(']') - 1);
		else if (line.find('=') != std::string::npos)
			values[section + "/" + trim(line.substr(0, line.find('=')))] = trim(line.substr(line.find('=') + 1));
	}
	return values;
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
	{ EMU_BTN_DPAD_STICK, "Swap D-Pad", "In game, makes the d-pad the analog stick, and the d-pad again. A sets, X clears." },
};
// in the mapping but not listed (R2): a control bound in Controls is taken from it too
const std::vector<Control> Unlisted {
	{ EMU_BTN_FFORWARD, "Fast Forward" },
};
// what a binding's value may read, for the lists' width
const char *const ControlNames[] { "NONE", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L1", "R1", "L2", "R2",
	"SELECT", "START", "FN1", "FN2", "MINUS", "PLUS" };

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

// a list of options, over the main menu
enum class Kind { List, Var, Input };
struct Item
{
	std::string name;
	std::string desc;
	std::vector<std::string> values;	// Var: what it may be; Input: what it is bound to
	int value = 0;
	std::function<void(int)> change;	// Var: a value picked
	std::function<void()> confirm;		// A
	DreamcastKey key = EMU_BTN_NONE;	// Input: what it is
};
struct Screen
{
	Kind kind = Kind::List;
	std::vector<Item> items;
	std::string desc;					// unless the selected item has its own
	int selected = 0;
	int start = 0;						// the first row shown
};

enum MainItem { Continue, Save, Load, Options, Quit, MainCount };
const char *const MainItems[MainCount] { "Continue", "Save", "Load", "Options", "Quit" };

struct State
{
	bool open = false;
	Canvas canvas;
	std::vector<u8> frame;			// the game's, when the menu opened (a saved state's picture)
	int frameWidth = 0;
	int frameHeight = 0;
	std::vector<u8> backdrop;		// it blurred, behind the main menu
	int backdropWidth = 0;
	int backdropHeight = 0;
	std::string title;
	int selected = Continue;
	int slot = 0;
	std::vector<Screen> screens;	// over the main menu, the top one shown
	std::string message;			// shown over everything until A or B
	int awaiting = -1;				// Input: the row waiting for a control
	int busy = -1;					// Save or Load: its screen shown, then done (it takes seconds)
	std::string biosFile;			// the missing BIOS page: the file it asks for
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

int maxRows()
{
	return (st.canvas.height() - ((Padding + PillSize) * 2 + ButtonSize)) / ButtonSize;
}

void move(Screen& screen, int step)
{
	const int count = (int)screen.items.size();
	if (count == 0)
		return;
	screen.selected = (screen.selected + step + count) % count;
	if (screen.selected < screen.start)
		screen.start = screen.selected;
	if (screen.selected >= screen.start + maxRows())
		screen.start = screen.selected - maxRows() + 1;
}

std::string saveDesc()
{
	return config::Settings::instance().hasPerGameConfig() ? "Using game config." : "Using console config.";
}

void forEachSetting(const std::function<void(Setting&)>& f)
{
	for (std::vector<Setting> *list : { &frontendSettings(), &emulatorSettings() })
		for (Setting& setting : *list)
			f(setting);
}

// minarch's Save Changes: for every game (and the game's own settings dropped), or for this one (what
// differs from every game's)
void saveSettings(bool forGame)
{
	config::Settings& settings = config::Settings::instance();
	if (forGame)
		settings.setPerGameConfig(true);
	else
		settings.dropPerGameConfig();
	config::setAutoSave(false);
	forEachSetting([](Setting& setting) { setting.option->save(); });
	config::setAutoSave(true);
}

std::string restoreDefaults()
{
	config::Settings& settings = config::Settings::instance();
	if (settings.hasPerGameConfig())
	{
		settings.dropPerGameConfig();
		forEachSetting([](Setting& setting) { setting.option->load(); });
		return "Restored console defaults.";
	}
	const std::map<std::string, std::string> defaults = shippedDefaults();
	config::setAutoSave(false);
	forEachSetting([&defaults](Setting& setting) {
		setting.option->reset();
		auto it = defaults.find(setting.section + "/" + setting.key);
		if (it != defaults.end())
			setting.set(it->second == "yes" || it->second == "true" ? 1 : it->second == "no" || it->second == "false" ? 0 : std::atoi(it->second.c_str()));
		setting.option->save();
	});
	config::setAutoSave(true);
	return "Restored defaults.";
}

Item entry(const std::string& name, const std::string& desc, std::function<void()> confirm)
{
	Item item;
	item.name = name;
	item.desc = desc;
	item.confirm = std::move(confirm);
	return item;
}

Screen settingsScreen(std::vector<Setting>& list)
{
	Screen screen;
	screen.kind = Kind::Var;
	for (Setting& setting : list)
	{
		Item item;
		item.name = setting.name;
		item.desc = setting.desc;
		for (const auto& choice : setting.choices)
			item.values.push_back(choice.first);
		item.value = setting.index();
		Setting *s = &setting;
		item.change = [s](int i) { s->set(s->choices[i].second); };
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

Screen bindingsScreen(const std::vector<Control>& controls)
{
	Screen screen;
	screen.kind = Kind::Input;
	screen.desc = "Press A to set and X to clear.";
	for (const Control& control : controls)
	{
		Item item;
		item.name = control.name;
		item.desc = control.desc != nullptr ? control.desc : "";
		item.key = control.key;
		screen.items.push_back(std::move(item));
	}
	refreshBindings(screen);
	return screen;
}

void savedSettings(const std::string& message)
{
	st.message = message;
	st.screens.pop_back();		// Save Changes, back to Options
	st.screens.back().items.back().desc = saveDesc();
}

Screen saveChangesScreen()
{
	Screen screen;
	screen.desc = saveDesc();
	screen.items.push_back(entry("Save for console", "", [] {
		saveSettings(false);
		savedSettings("Saved for console.");
	}));
	screen.items.push_back(entry("Save for game", "", [] {
		if (settings.content.gameId.empty())
			return savedSettings("This game cannot have its own config.");
		saveSettings(true);
		savedSettings("Saved for game.");
	}));
	screen.items.push_back(entry("Restore defaults", "", [] { savedSettings(restoreDefaults()); }));
	return screen;
}

Screen optionsScreen()
{
	Screen screen;
	screen.items.push_back(entry("Frontend", "Deadcast " DEADCAST_VERSION, [] { st.screens.push_back(settingsScreen(frontendSettings())); }));
	screen.items.push_back(entry("Emulator", "", [] { st.screens.push_back(settingsScreen(emulatorSettings())); }));
	screen.items.push_back(entry("Controls", "", [] { st.screens.push_back(bindingsScreen(Controls)); }));
	screen.items.push_back(entry("Save Changes", saveDesc(), [] { st.screens.push_back(saveChangesScreen()); }));
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

void actMain(u32 code)
{
	switch (code)
	{
	case PadUp:
		st.selected = (st.selected + MainCount - 1) % MainCount;
		break;
	case PadDown:
		st.selected = (st.selected + 1) % MainCount;
		break;
	case PadLeft:
	case PadRight:
		if (st.selected == Save || st.selected == Load)
			st.slot = (st.slot + (code == PadLeft ? SlotCount - 1 : 1)) % SlotCount;
		break;
	case PadB:
	case PadMenu:
		st.result = Result::Resume;
		break;
	case PadA:
		switch (st.selected)
		{
		case Continue:
			st.result = Result::Resume;
			break;
		case Save:
			st.busy = Save;
			break;
		case Load:
			updatePreview();
			if (st.stateExists)
				st.busy = Load;
			break;
		case Options:
			st.screens.push_back(optionsScreen());
			break;
		case Quit:
			st.result = Result::Quit;
			break;
		}
		break;
	}
}

void actScreen(u32 code)
{
	Screen& screen = st.screens.back();
	if (screen.items.empty())
	{
		if (code == PadB)
			st.screens.pop_back();
		return;
	}
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
		if (screen.kind == Kind::Var && !item.values.empty())
		{
			const int count = (int)item.values.size();
			item.value = (item.value + (code == PadLeft ? count - 1 : 1)) % count;
			if (item.change)
				item.change(item.value);
		}
		break;
	case PadB:
		st.screens.pop_back();
		break;
	case PadA:
		if (screen.kind == Kind::Input)
			st.awaiting = screen.selected;
		else if (item.confirm)
		{
			// it may change the screens, this one's items with them
			const std::function<void()> confirm = item.confirm;
			confirm();
		}
		break;
	case PadX:
		if (screen.kind == Kind::Input)
		{
			unbind(item.key);
			refreshBindings(screen);
			move(screen, 1);
		}
		break;
	}
}

void act(u32 code)
{
	if (!st.biosFile.empty())
	{
		if (code == PadA || code == PadB)
			st.result = Result::Quit;
		return;
	}
	if (st.busy >= 0)
		return;
	st.dirty = true;
	if (!st.message.empty())
	{
		if (code == PadA || code == PadB)
			st.message.clear();
	}
	else if (st.screens.empty())
		actMain(code);
	else
		actScreen(code);
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

// button and hint pairs in a pill at the bottom
void drawButtons(const std::vector<std::pair<const char *, const char *>>& pairs, bool right)
{
	Canvas& c = st.canvas;
	int ow = ButtonMargin;
	for (const auto& pair : pairs)
		ow += buttonWidth(pair.first, pair.second) + ButtonMargin;
	int ox = right ? c.width() - Padding - ow : Padding;
	int oy = c.height() - Padding - PillSize;
	c.pill(ox, oy, ow, PillSize, ColorAccent);
	ox += ButtonMargin;
	oy += ButtonMargin;
	for (const auto& pair : pairs)
	{
		drawButton(pair.first, pair.second, ox, oy);
		ox += buttonWidth(pair.first, pair.second) + ButtonMargin;
	}
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

void drawPreview()
{
	Canvas& c = st.canvas;
	const int hw = c.width() / 2;
	const int hh = c.height() / 2;
	const int pw = hw + S(4 * 2);
	const int ph = hh + S(4 * 2 + 6 + 4);
	int ox = c.width() - pw - Padding;
	int oy = (c.height() - ph) / 2;
	c.roundRect(ox, oy, pw, ph, S(4), White);
	ox += S(4);
	oy += S(4);
	c.fill(ox, oy, hw, hh, Black);
	updatePreview();
	if (!st.preview.empty())
		c.picture(st.preview.data(), st.previewWidth, st.previewHeight, ox, oy, hw, hh);
	else
		drawMessage(fonts->large, st.stateExists ? "No Preview" : "Empty Slot", ox, oy, hw, hh);
	// the slots: the one picked a black dot, the others small gray ones
	const int px = ox + (hw - S(15 * SlotCount)) / 2;
	const int py = oy + hh + S(4);
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

void drawMain()
{
	Canvas& c = st.canvas;
	Font& large = fonts->large;
	c.clear(ColorBackground);
	if (!st.backdrop.empty())
		c.picture(st.backdrop.data(), st.backdropWidth, st.backdropHeight, 0, 0, c.width(), c.height());

	const int maxWidth = c.width() - Padding * 2;
	const std::string title = Canvas::fit(large, st.title, maxWidth - ButtonPadding * 2);
	c.pill(Padding, Padding, std::min(maxWidth, large.measure(title) + ButtonPadding * 2), PillSize, ColorAccent);
	c.text(large, title, Padding + ButtonPadding, baseline(large, Padding, PillSize), ColorHint);

	drawButtons({ { "B", "BACK" }, { "A", "OKAY" } }, true);

	// the items, in the middle (NextUI's 1x rows)
	const int oy = ((c.height() / Scale - 5 * 2) - MainCount * 30) / 2;
	for (int i = 0; i < MainCount; i++)
	{
		const int top = S(oy + 5 + i * 30);
		u32 color = White;
		if (i == st.selected)
		{
			c.pill(Padding, top, large.measure(MainItems[i]) + ButtonPadding * 2, PillSize, ColorMain);
			color = ColorSelectedText;
		}
		c.text(large, MainItems[i], Padding + ButtonPadding, baseline(large, top, PillSize), color);
	}
	if (st.selected == Save || st.selected == Load)
		drawPreview();
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

// a description at the bottom, broken into lines that fit
void drawDescription(const std::string& desc)
{
	Canvas& c = st.canvas;
	Font& tiny = fonts->tiny;
	const std::vector<std::string> lines = wrap(tiny, desc, c.width() - Padding * 2);
	const int leading = S(12);
	int y = c.height() - Padding - leading * (int)lines.size();
	for (const std::string& line : lines)
	{
		c.text(tiny, line, (c.width() - tiny.measure(line)) / 2, baseline(tiny, y, leading), White);
		y += leading;
	}
}

void drawScreen(const Screen& screen)
{
	Canvas& c = st.canvas;
	Font& small = fonts->small;
	Font& tiny = fonts->tiny;
	c.clear(ColorBackground);

	const int count = (int)screen.items.size();
	const int end = std::min(count, screen.start + maxRows());
	const int oy = Padding + PillSize;
	std::string desc;
	// as wide as its widest row, in the middle
	int mw = 0;
	int valuesWidth = 0;
	if (screen.kind == Kind::Input)
		for (const char *name : ControlNames)
			valuesWidth = std::max(valuesWidth, tiny.measure(name));
	for (const Item& item : screen.items)
	{
		if (screen.kind == Kind::List)
			mw = std::max(mw, small.measure(item.name) + OptionPadding * 2);
		else
		{
			int rw = valuesWidth;
			for (const std::string& value : item.values)
				rw = std::max(rw, tiny.measure(value));
			mw = std::max(mw, small.measure(item.name) + rw + OptionPadding * 4);
		}
	}
	mw = std::min(mw, c.width() - Padding * 2);
	const int ox = (c.width() - mw) / 2;

	for (int i = screen.start; i < end; i++)
	{
		const Item& item = screen.items[i];
		const int top = oy + (i - screen.start) * ButtonSize;
		const int labelWidth = small.measure(item.name) + OptionPadding * 2;
		u32 color = White;
		if (i == screen.selected)
		{
			if (screen.kind != Kind::List)
				c.pill(ox, top, mw, ButtonSize, ColorAccent);
			c.pill(ox, top, labelWidth, ButtonSize, ColorMain);
			color = ColorSelectedText;
			desc = item.desc;
		}
		c.text(small, item.name, ox + OptionPadding, baseline(small, top, ButtonSize), color);
		if (screen.kind != Kind::List && !(st.awaiting == i) && item.value < (int)item.values.size())
		{
			const std::string& value = item.values[item.value];
			c.text(tiny, value, ox + mw - OptionPadding - tiny.measure(value), baseline(tiny, top, ButtonSize), White);
		}
	}
	if (count > maxRows())
	{
		// NextUI's gray arrows, 24 x 4 (1x)
		const int ax = (c.width() - S(24)) / 2;
		const int ay = (PillSize - S(4)) / 2;
		if (screen.start > 0)
		{
			const float up[] { (float)ax, (float)(Padding + ay + S(4)), (float)(ax + S(12)), (float)(Padding + ay),
				(float)(ax + S(24)), (float)(Padding + ay + S(4)) };
			c.polygon(up, 3, Gray);
		}
		if (end < count)
		{
			const int y = c.height() - (Padding + PillSize + ButtonSize) + ay;
			const float down[] { (float)ax, (float)y, (float)(ax + S(24)), (float)y, (float)(ax + S(12)), (float)(y + S(4)) };
			c.polygon(down, 3, Gray);
		}
	}
	drawDescription(desc.empty() ? screen.desc : desc);
}

void drawPage(const std::string& line, const std::string& under);
std::string discName();
void drawBiosPage();

void draw()
{
	Canvas& c = st.canvas;
	if (!st.biosFile.empty())
		drawBiosPage();
	else if (st.busy >= 0)
		drawPage(st.busy == Save ? "Saving..." : "Loading...", discName());
	else if (!st.message.empty())
	{
		c.clear(ColorBackground);
		drawMessage(fonts->large, st.message, 0, 0, c.width(), c.height());
		drawButtons({ { "A", "OKAY" } }, true);
	}
	else if (st.screens.empty())
		drawMain();
	else
		drawScreen(st.screens.back());
	GraphicsContext::Instance()->showPicture(c.data());
}

void close(bool resume)
{
	GraphicsContext::Instance()->endPictures(resume);
	st.open = false;
	st.frame.clear();
	st.backdrop.clear();
	st.preview.clear();
	st.screens.clear();
	st.biosFile.clear();
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

bool starting = false;

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
	st.message.clear();
	st.awaiting = -1;
	st.busy = -1;
	st.held = 0;
	st.result = Result::Open;
	st.dirty = true;
	st.open = true;
	return true;
}

void endStart()
{
	if (!starting)
		return;
	GraphicsContext::Instance()->endPictures(true);
	starting = false;
}

bool open()
{
	if (st.open)
		return true;
	GraphicsContext *context = GraphicsContext::Instance();
	if (context == nullptr || !loadFonts())
		return false;
	// the frame on the screen, before the menu takes it: its background and a saved state's picture
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
	st.backdrop = blur(st.frame, st.frameWidth, st.frameHeight, 4, 6, st.backdropWidth, st.backdropHeight);
	st.title = displayName();
	st.selected = Continue;
	st.slot = config::SavestateSlot >= 0 && config::SavestateSlot < SlotCount ? (int)config::SavestateSlot : 0;
	st.screens.clear();
	st.message.clear();
	st.awaiting = -1;
	st.busy = -1;
	st.result = Result::Open;
	st.dirty = true;
	st.held = 0;
	st.previewSlot = -1;
	st.open = true;
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
	if (st.busy >= 0 && !st.dirty)
	{
		const bool done = st.busy == Save ? saveState() : loadState();
		st.busy = -1;
		st.dirty = true;
		if (done)
			st.result = Result::Resume;
	}
	const u64 now = getTimeMs();
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
