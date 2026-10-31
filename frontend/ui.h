// Deadcast's frontend: the menu over a paused game and the start page, drawn on the CPU to look like
// NextUI's own emulators' menus. Shared by Deadcast and every recomp module, so that each one plays with
// the same menu: a module adds no menu of its own. A host (Deadcast, a module) gives it the screen, the
// menu's buttons, the paused game's picture and its own pages; it needs nothing of the emulator.
//
// Every page looks like the first: its title in a pill at the top, its rows (or a body of its own), the
// buttons at the bottom right and the selected row's description (or the page's status) at the bottom
// left, over the paused game blurred. Opening or leaving a page slides its rows; the pills stay in place.
#pragma once
#include "canvas.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace deadcast::ui
{

// the screen: full-screen BGRA pictures instead of the game's frames while a page is up
class Display
{
public:
	virtual ~Display() = default;
	// pictures from now on; false if there is no screen to draw on
	virtual bool begin(int& width, int& height) = 0;
	virtual void show(const u32 *pixels) = 0;
	// after the pictures: the game's last frame again (it goes on), its next one (a game starting), or the
	// last picture left up (quitting)
	enum class After { LastFrame, NextFrame, Stay };
	virtual void end(After after) = 0;
};
void setDisplay(Display *display);

// the menu's buttons, which the host maps its own onto
enum class Button { None, Up, Down, Left, Right, A, B, X, Menu };

// NextUI's layout on the Brick: its 1x sizes, three times as large
constexpr int Scale = 3;
constexpr int S(int v) { return v * Scale; }
constexpr int Padding = S(5);
constexpr int PillSize = S(30);
constexpr int ButtonSize = S(20);
constexpr int ButtonMargin = S(5);
constexpr int ButtonPadding = S(12);
constexpr u32 White = 0xffffff;
constexpr u32 Gray = 0x999999;
constexpr u32 LightGray = 0x7f7f7f;
constexpr u32 TextGray = 0xb4b4b4;		// text under a page's line
constexpr u32 Black = 0x000000;

// Noto Sans Bold at NextUI's sizes
struct Fonts
{
	Font large;		// the main list, messages
	Font medium;	// a button's letter
	Font small;		// options, hints
	Font tiny;		// values, descriptions
	Font page;		// the start page's line
	Font pageText;	// and the text under it
};
Fonts& fonts();
// what pages are drawn on, the screen's size
Canvas& canvas();

// A row of a page: does something (A), picks one of its values (Left/Right), or shows what a control is
// bound to (A waits for a control, X clears it)
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
	std::function<void()> clear;		// X on a Binding
	std::function<bool()> available;	// while false: greyed, and the cursor goes past it
	std::function<void(int ox)> aside;	// drawn at the right while selected (Save, Load: the slot's picture)
};
Item entry(const std::string& name, const std::string& desc, std::function<void()> confirm);
bool enabled(const Item& item);

using Buttons = std::vector<std::pair<const char *, const char *>>;
struct Screen
{
	std::string title;
	std::vector<Item> items;
	std::string desc;						// unless the selected item has its own
	std::function<std::string()> status;	// at the bottom left, whatever is selected (the main page's: the network)
	int selected = 0;
	int start = 0;							// the first row shown
	std::function<void()> back;				// B, and MENU on the first page; else the page before again
	std::function<void()> tick;				// every frame while shown (redraw(), finish() when something changed)
	// a page of its own (the link's): its body drawn at ox, its buttons and its input, instead of rows
	std::function<void(int ox)> body;
	std::function<Buttons()> buttons;
	std::function<void(Button)> act;
};

// what plays the game: Deadcast's emulation, or a recomp module (a recompiled build of the game)
enum class Core { Emulator, Recomp };
// what the host brings to the main page and Core Settings
struct Host
{
	Core core = Core::Emulator;					// the core this process plays the game with
	std::string disc;							// the disc played (its recomp module found by it)
	Item save;									// the emulator's save states (a module: none, disabled)
	Item load;
	std::function<Screen()> emulatorSettings;	// the emulator's pages (a module: none, disabled)
	// a recomp setting changed (key, value): a module applies it, if it can while playing
	std::function<void(const std::string&, const std::string&)> recompSetting;
};
// the first page: Continue, Save, Load, Core Settings, Quit, the network's status at the bottom left
Screen mainScreen(const std::string& title, Host host);

// the menu over the paused game: its picture (RGB24, the backdrop, blurred and darkened) and the first page
bool open(const std::vector<u8>& frame, int frameWidth, int frameHeight, Screen first);
bool isOpen();
void push(Screen screen);
void pop();
Screen& page();							// the page shown
size_t depth();							// pages open (1: the first)
void move(Screen& screen, int step);	// the selected row, to the next enabled one that way
void redraw();
// over everything until A or B
void message(const std::string& text);
// A does yes (its button saying yesHint), B no
void ask(const std::string& text, const char *yesHint, std::function<void()> yes, std::function<void()> no = {});
// a page saying what is being done (the line, the text under it) while work runs: it may take seconds
void work(const std::string& line, const std::string& under, std::function<void()> fn);
// over the page instead ("Checking...")
void busy(const std::string& text, std::function<void()> fn);
// a Binding row waiting for a control (the host reads it: the menu's buttons are not)
void awaitControl(bool waiting);
bool awaitingControl();
void button(Button button, bool pressed);

// Restart: the game again from its start (two players); Switch: in the other core (its choice stored)
enum class Result { Open, Resume, Quit, Restart, Switch };
void finish(Result result);
// every frame while open: repeats held directions, redraws when needed; closes on a result
Result frame();

// the start page, until the game's first frame: a line, the disc's name under it
bool showStart(const std::string& line, const std::string& under);
bool starting();
void startLine(const std::string& line, const std::string& under);
void endStart();
// a page of the host's own over everything (the BIOS page): drawn by draw while open, A or B quits
bool showPage(std::function<void()> draw);

// a page's line in white and text under it in grey, centered on black
void drawPage(const std::string& line, const std::string& under);
// lines of text centered in a rectangle
void drawMessage(Font& font, const std::string& message, int x, int y, int w, int h);
// text broken at spaces into lines no wider than width
std::vector<std::string> wrap(Font& font, const std::string& text, int width);

// the network: a default route other than over the link to another Brick, checked every second while open
bool network();
std::string networkStatus();

// the game's name as NextUI shows a file's: without its extensions or trailing (...) and [...]
std::string gameName(const std::string& path);

}
