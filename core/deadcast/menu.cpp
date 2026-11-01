#include "menu.h"
#include "link.h"
#include "netplay.h"
#include "ui.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "input/gamepad_device.h"
#include "log/Log.h"
#include "oslib/oslib.h"
#include "sdl/sdl_gamepad.h"
#include "version.h"
#include "wsi/context.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>

// Deadcast's side of the menu (frontend/ui.h draws it): its live settings for Emulator Options
// (frontend/options.h), its own pages (Controls, Local Multiplayer and the link, a two-player
// session's), its save states, the BIOS page, and the screen and pad it is shown with.
namespace deadcast::menu
{
namespace
{

using ui::Item;
using ui::Row;
using ui::Screen;
using ui::entry;
using ui::S;

constexpr int SlotCount = 10;		// Deadcast's save state slots

// the Brick's buttons as SDL numbers them (trimui_inputd's pad), and its d-pad (Deadcast's hat codes)
enum : u32 { PadB = 0, PadA = 1, PadY = 2, PadX = 3, PadL1 = 4, PadR1 = 5, PadSelect = 6, PadStart = 7, PadMenu = 8,
	PadFn1 = 9, PadFn2 = 10, PadMinus = 13, PadPlus = 14, PadUp = 256, PadDown, PadLeft, PadRight };
// and its triggers, as axes
enum : u32 { AxisL2 = 2, AxisR2 = 5 };

// the pages on the screen: the graphics context's pictures (Vulkan's panel on fb0)
class PanelDisplay : public ui::Display
{
public:
	bool begin(int& width, int& height) override
	{
		GraphicsContext *context = GraphicsContext::Instance();
		return context != nullptr && context->beginPictures(width, height);
	}
	void show(const u32 *pixels) override
	{
		GraphicsContext::Instance()->showPicture(pixels);
	}
	void end(After after) override
	{
		GraphicsContext::Instance()->endPictures(after == After::LastFrame ? GraphicsContext::AfterPictures::LastFrame
				: after == After::NextFrame ? GraphicsContext::AfterPictures::NextFrame : GraphicsContext::AfterPictures::Stay);
	}
};
PanelDisplay display;

struct State
{
	std::vector<u8> frame;			// the game's, when the menu opened (a saved state's picture)
	int frameWidth = 0;
	int frameHeight = 0;
	int slot = 0;
	int previewSlot = -1;			// the slot shown in the preview
	bool stateExists = false;
	std::vector<u8> preview;		// its picture
	int previewWidth = 0;
	int previewHeight = 0;
	DreamcastKey awaiting = EMU_BTN_NONE;	// a Controls row waiting for a control
	netplay::PauseState shownPause;	// a session's menu: the pause it shows
	bool link = false;				// the link's page open
	u32 linkVersion = 0;			// the link's status it shows
	std::string linkFailure;		// why the link cannot go on, once it cannot (kept until the page closes)
	std::chrono::steady_clock::time_point connectedAt;	// Connected shown since (the game restarts after)
	bool connected = false;
	std::string biosFile;			// the missing BIOS page: the file it asks for
	bool startingTwoPlayers = false;	// the start page says so
	bool switching = false;			// the other core chosen
};
State st;

// Emulator Options' settings (frontend/options.h): the emulator's own, live, saved as they change
template<typename T>
config::Option<T> *option(const options::Setting& setting)
{
	for (config::BaseOption *o : config::Settings::instance().list())
		if (auto *found = dynamic_cast<config::Option<T> *>(o);
				found != nullptr && found->getSection() == setting.section && found->getName() == setting.key)
			return found;
	return nullptr;
}

options::Store liveSettings()
{
	return {
		[](const options::Setting& setting) {
			if (setting.flag)
			{
				config::Option<bool> *o = option<bool>(setting);
				return o != nullptr ? (o->get() ? 1 : 0) : setting.fallback;
			}
			config::Option<int> *o = option<int>(setting);
			return o != nullptr ? o->get() : setting.fallback;
		},
		[](const options::Setting& setting, int value) {
			if (setting.flag)
			{
				if (config::Option<bool> *o = option<bool>(setting))
				{
					o->set(value != 0);
					o->save();
				}
			}
			else if (config::Option<int> *o = option<int>(setting))
			{
				o->set(value);
				o->save();
			}
		},
	};
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

// the game's name as NextUI shows the file it started
std::string displayName()
{
	const char *rom = std::getenv("DEADCAST_ROM");
	const std::string name = ui::gameName(rom != nullptr && *rom != '\0' ? rom : settings.content.path);
	return name.empty() ? settings.content.title : name;
}

// the disc being started, as its file is named
std::string discName()
{
	const std::string& path = settings.content.path;
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// the Controls page's rows, one per Controls entry: what each is bound to now
void refreshBindings(Screen& screen)
{
	for (size_t i = 0; i < screen.items.size() && i < Controls.size(); i++)
	{
		screen.items[i].values = { bindingName(Controls[i].key) };
		screen.items[i].value = 0;
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
		const DreamcastKey key = control.key;
		item.confirm = [key] {
			st.awaiting = key;
			ui::awaitControl(true);
		};
		item.clear = [key] {
			unbind(key);
			refreshBindings(ui::page());
		};
		screen.items.push_back(std::move(item));
	}
	refreshBindings(screen);
	return screen;
}

// the control pressed for the row waiting for one
void bindAwaited(const InputMapping::InputDef& input)
{
	bind(st.awaiting, input);
	st.awaiting = EMU_BTN_NONE;
	ui::awaitControl(false);
	refreshBindings(ui::page());
	ui::move(ui::page(), 1);
}

//
// Local Multiplayer: the link's page
//

// linked, tested, IP up, the same game and build on both: both start it again as two players
bool canStart(const link::Status& status)
{
	return status.state == link::State::Tested && status.role != link::Role::None && !status.address.empty()
			&& status.sameGame && status.sameVersion;
}

// why the link cannot go on (empty while it can): the other Brick on another build or game, or Bluetooth
// not starting. A link lost while connecting is not one: it looks again (link.log says why).
std::string linkFailure(const link::Status& s)
{
	if (s.state == link::State::Failed)
		return s.error;
	if (s.role != link::Role::None && !s.sameVersion)
		return "Both Bricks require the same Deadcast version.\nPlease update Deadcast to the latest version.";
	if (s.role != link::Role::None && !s.sameGame)
		return "Both Bricks must be playing the same game.";
	return "";
}

// what the link is doing, in a word or two
std::string linkHeadline(const link::Status& s)
{
	if (s.role != link::Role::None)
		return canStart(s) ? "Connected" : "Connecting to device";
	if (s.activity == link::Activity::TurningOn)
		return "Turning on Bluetooth";
	if (s.activity == link::Activity::Connecting)
		return "Connecting to device";
	return "Locating device";
}

// how to connect, under Locating device
std::string linkHint(link::Via via)
{
	if (via == link::Via::Bluetooth)
		return "Open Bluetooth on both Bricks and keep them close.";
	return "Connect the Bricks with a USB-C cable: the top port of one to the bottom port of the other, "
			"or bottom to bottom (not top to top). Open USB Link on both.";
}

// the link's page: a headline (how to connect under Locating device), or Connection failed and why;
// everything else the link finds goes to link.log
void drawLink(int ox)
{
	Canvas& c = ui::canvas();
	Font& large = ui::fonts().large;
	Font& small = ui::fonts().small;
	const link::Status s = link::status();
	const std::string title = st.linkFailure.empty() ? linkHeadline(s) : "Connection failed";
	const std::string under = !st.linkFailure.empty() ? st.linkFailure : title == "Locating device" ? linkHint(s.via) : "";
	// left-aligned with the pages' rows, in the middle of the room between the title and the buttons
	const int x = ox + ui::Padding + ui::ButtonPadding;
	const int width = c.width() - (ui::Padding + ui::ButtonPadding) * 2;
	const std::vector<std::string> lines = ui::wrap(large, title, width);
	std::vector<std::string> wrapped;
	for (size_t start = 0, end; !under.empty() && start <= under.size(); start = end + 1)
	{
		end = under.find('\n', start);
		if (end == std::string::npos)
			end = under.size();
		for (const std::string& l : ui::wrap(small, under.substr(start, end - start), width))
			wrapped.push_back(l);
	}
	const int gap = wrapped.empty() ? 0 : S(4);
	const int height = (int)lines.size() * large.lineHeight() + gap + (int)wrapped.size() * small.lineHeight();
	const int top = ui::Padding + ui::PillSize;
	int y = top + (c.height() - ui::Padding - ui::PillSize - top - height) / 2;
	for (const std::string& l : lines)
	{
		c.text(large, l, x, y + large.ascent(), ui::White);
		y += large.lineHeight();
	}
	y += gap;
	for (const std::string& l : wrapped)
	{
		c.text(small, l, x, y + small.ascent(), ui::TextGray);
		y += small.lineHeight();
	}
}

// the link's page closed: the link down, unless a session keeps it
void closeLink()
{
	if (!link::kept())
		link::stop();
	st.link = false;
	ui::pop();
}

// the link's page, the link looking for the other Brick: B leaves; failed, A (OKAY) leaves. Drawn again
// whenever the link's status changes; Connected shown a moment, then the game starts again as two players
void openLink(link::Via via)
{
	Screen screen;
	screen.title = via == link::Via::Bluetooth ? "Bluetooth" : "USB Link";
	screen.body = drawLink;
	screen.buttons = [] {
		return st.linkFailure.empty() ? ui::Buttons { { "B", "BACK" } } : ui::Buttons { { "A", "OKAY" } };
	};
	screen.back = closeLink;
	screen.act = [](ui::Button code) {
		if (code == ui::Button::A && !st.linkFailure.empty())
			closeLink();
	};
	screen.tick = [] {
		using namespace std::chrono;
		const link::Status s = link::status();
		if (s.version != st.linkVersion)
		{
			st.linkVersion = s.version;
			if (st.linkFailure.empty())
				st.linkFailure = linkFailure(s);
			ui::redraw();
		}
		// linked: the game again on both, as two players, from its start
		if (!st.linkFailure.empty() || !canStart(s))
		{
			st.connected = false;
			return;
		}
		if (!st.connected)
		{
			st.connected = true;
			st.connectedAt = steady_clock::now();
		}
		else if (steady_clock::now() - st.connectedAt > seconds(1))
		{
			netplay::begin(s.role == link::Role::Host, s.peerAddress, s.inputDelay);
			ui::finish(ui::Result::Restart);
		}
	};
	ui::push(std::move(screen));
	st.link = true;
	st.linkVersion = ~0u;
	st.linkFailure.clear();
	st.connected = false;
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

//
// Save states
//

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
		ui::message("The game cannot be saved now.");
		return false;
	}
	if (netplay::twoPlayerRun())
	{
		ui::message("No save states in a game started as two players.\nIts own saves (on the VMU) work.");
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
		ui::message("The game could not be saved.");
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
		ui::message("The game could not be loaded.");
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

// the selected slot's saved state, at the right of a page drawn at ox, the slots under it
void drawPreview(int ox)
{
	Canvas& c = ui::canvas();
	const int hw = c.width() / 2;
	const int hh = c.height() / 2;
	const int pw = hw + S(4 * 2);
	const int ph = hh + S(4 * 2 + 6 + 4);
	int x = ox + c.width() - pw - ui::Padding;
	int y = (c.height() - ph) / 2;
	c.roundRect(x, y, pw, ph, S(4), ui::White);
	x += S(4);
	y += S(4);
	c.fill(x, y, hw, hh, ui::Black);
	updatePreview();
	if (!st.preview.empty())
		c.picture(st.preview.data(), st.previewWidth, st.previewHeight, x, y, hw, hh);
	else
		ui::drawMessage(ui::fonts().large, st.stateExists ? "No Preview" : "Empty Slot", x, y, hw, hh);
	// the slots: the one picked a black dot, the others small gray ones
	const int px = x + (hw - S(15 * SlotCount)) / 2;
	const int py = y + hh + S(4);
	for (int i = 0; i < SlotCount; i++)
	{
		const int cx = px + S(15 * i) + S(15) / 2;
		const int cy = py + S(3);
		if (i == st.slot)
			c.circle(cx, cy, S(3), ui::Black);
		else
			c.circle(cx, cy, S(1), ui::LightGray);
	}
}

// the main page's Save and Load, on the slot picked with Left/Right, its saved state beside them
ui::Host host()
{
	ui::Host host;
	host.disc = settings.content.path;
	host.save = entry("Save", "", [] {
		ui::work("Saving...", discName(), [] {
			if (saveState())
				ui::finish(ui::Result::Resume);
		});
	});
	host.load = entry("Load", "", [] {
		updatePreview();
		if (st.stateExists)
			ui::work("Loading...", discName(), [] {
				if (loadState())
					ui::finish(ui::Result::Resume);
			});
	});
	for (Item *item : { &host.save, &host.load })
	{
		item->shift = [](int step) { st.slot = (st.slot + step + SlotCount) % SlotCount; };
		item->aside = drawPreview;
	}
	host.version = "Deadcast " DEADCAST_VERSION;
	host.emulatorSettings = liveSettings();
	host.controls = [] { ui::push(bindingsScreen()); };
	host.localMultiplayer = [] { ui::push(localMultiplayerScreen()); };
	return host;
}

//
// A paused two-player session
//

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
	ui::finish(ui::Result::Resume);
}

// a paused two-player session's rows (no saving or loading: the two Bricks must stay alike); closed when
// the session goes on (Continue on the other Brick), drawn again when why it is paused changes
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
	});
	resume.available = canContinue;
	screen.items.push_back(std::move(resume));
	// on alone, the game as it is (the other Brick told)
	screen.items.push_back(entry("Leave two-player game", "", [] {
		netplay::leave();
		ui::finish(ui::Result::Resume);
	}));
	screen.items.push_back(entry("Quit", "", [] { ui::finish(ui::Result::Quit); }));
	screen.tick = [] {
		const netplay::PauseState now = netplay::pauseState();
		if (now.pause == netplay::Pause::None)
			ui::finish(ui::Result::Resume);
		else if (now.pause != st.shownPause.pause || now.peerPresent != st.shownPause.peerPresent)
		{
			st.shownPause = now;
			ui::page().title = sessionTitle();
			ui::redraw();
		}
	};
	return screen;
}

//
// The BIOS page
//

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
	Canvas& c = ui::canvas();
	Font& big = ui::fonts().page;
	Font& text = ui::fonts().pageText;
	const int width = std::min(c.width() - 120, 904);
	const std::vector<std::string> title = ui::wrap(big, "Deadcast needs " + biosName(st.biosFile), width);
	const std::vector<std::string> body = ui::wrap(text, "Copy " + st.biosFile + " into this folder:", width);
	const std::string folder = biosFolder();
	const int bar = text.lineHeight() + 16;
	const int height = (int)title.size() * big.lineHeight() + 18 + (int)body.size() * text.lineHeight() + 14 + bar + 36
			+ text.lineHeight();
	int y = (c.height() - height) / 2;
	auto line = [&](Font& font, const std::string& s, u32 rgb) {
		c.text(font, s, (c.width() - font.measure(s)) / 2, y + font.ascent(), rgb);
		y += font.lineHeight();
	};
	c.clear(ui::Black);
	for (const std::string& s : title)
		line(big, s, ui::White);
	y += 18;
	for (const std::string& s : body)
		line(text, s, ui::TextGray);
	y += 14;
	const int barWidth = text.measure(folder) + 48;
	c.roundRect((c.width() - barWidth) / 2, y, barWidth, bar, 12, 0x2a2a2a);
	y += 8;
	line(text, folder, ui::White);
	y += 8 + 36;
	line(text, "Press A or B to exit", ui::White);
}

// the frame on the screen, before the menu takes it: the pages' background and a saved state's picture
void grabFrame()
{
	st.frameWidth = st.frameHeight = 0;
	if (renderer == nullptr || !renderer->GetLastFrame(st.frame, st.frameWidth, st.frameHeight))
		st.frame.clear();
}

// the menu over the paused game with first as its first page
bool openWith(Screen first)
{
	ui::setDisplay(&display);
	grabFrame();
	st.previewSlot = -1;
	st.awaiting = EMU_BTN_NONE;
	st.link = false;
	if (!ui::open(st.frame, st.frameWidth, st.frameHeight, std::move(first)))
	{
		st.frame.clear();
		return false;
	}
	return true;
}

ui::Button menuButton(u32 code)
{
	switch (code)
	{
	case PadUp: return ui::Button::Up;
	case PadDown: return ui::Button::Down;
	case PadLeft: return ui::Button::Left;
	case PadRight: return ui::Button::Right;
	case PadA: return ui::Button::A;
	case PadB: return ui::Button::B;
	case PadX: return ui::Button::X;
	case PadMenu: return ui::Button::Menu;
	default: return ui::Button::None;
	}
}

}	// namespace

bool showStart()
{
	ui::setDisplay(&display);
	return ui::showStart("Loading...", discName());
}

void showStartError(const std::string& what)
{
	if (!ui::starting())
		return;
	ui::startLine("Cannot start " + displayName(), what);
	std::this_thread::sleep_for(std::chrono::seconds(3));
}

bool showBiosMissing(const std::string& file)
{
	ui::setDisplay(&display);
	st.biosFile = file;
	st.startingTwoPlayers = false;
	return ui::showPage(drawBiosPage);
}

bool showStartTwoPlayers()
{
	if (!ui::starting())
		return false;
	if (!st.startingTwoPlayers)
	{
		ui::startLine("Starting two players...", "Waiting for the other Brick");
		st.startingTwoPlayers = true;
	}
	return true;
}

void endStart()
{
	ui::endStart();
	st.startingTwoPlayers = false;
}

bool open()
{
	if (ui::isOpen())
		return true;
	st.slot = config::SavestateSlot >= 0 && config::SavestateSlot < SlotCount ? (int)config::SavestateSlot : 0;
	return openWith(ui::mainScreen(displayName(), host()));
}

bool openSession()
{
	if (ui::isOpen())
		return true;
	st.shownPause = netplay::pauseState();
	return openWith(sessionScreen());
}

bool isOpen()
{
	return ui::isOpen();
}

bool switching()
{
	return st.switching;
}

void button(u32 code, bool pressed)
{
	if (!ui::isOpen())
		return;
	if (pressed && st.awaiting != EMU_BTN_NONE)
	{
		bindAwaited(InputMapping::InputDef::from_button(code));
		return;
	}
	ui::button(menuButton(code), pressed);
}

void axis(u32 code, int value)
{
	// only a trigger pressed, for a row waiting for a control
	if (ui::isOpen() && st.awaiting != EMU_BTN_NONE && value > 16384)
		bindAwaited(InputMapping::InputDef::from_axis(code, true));
}

Result frame()
{
	const bool link = st.link;
	const ui::Result result = ui::frame();
	if (result == ui::Result::Open)
		return Result::Open;
	// closed: the link down unless a session keeps it
	if (link && !link::kept())
		link::stop();
	st.link = false;
	st.frame.clear();
	st.preview.clear();
	st.biosFile.clear();
	switch (result)
	{
	case ui::Result::Quit:
		return Result::Quit;
	case ui::Result::Restart:
		return Result::Restart;
	case ui::Result::Switch:
		st.switching = true;
		return Result::Switch;
	default:
		return Result::Resume;
	}
}

}
