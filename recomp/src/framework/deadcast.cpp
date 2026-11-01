// Deadcast in a recomp module, registered as an engine extension: MENU opening Deadcast's menu over
// the game (../../frontend/ui.h), the same menu as Deadcast's own, drawn into the engine's overlay
// (an image the size of the screen, blended over the game on the GPU). While the menu is open the
// guest is held at its vblank (clock, sound and game logic stopped) and the menu reads the pad itself;
// its pages are Deadcast's: Save and Load (the emulator's) and Emulator Settings disabled, Recomp
// Settings the module's. A module has no menu of its own: its settings are Recomp Settings', declared
// in its manifest (frontend/recomp.h).
#include "dream/runtime/host_ext.h"
#include "dreamcomp/port.h"
#include "recomp.h"
#include "ui.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace dreamcomp {

namespace {

namespace ui = deadcast::ui;

// The Brick's pad, as the window reads it (engine/render/src/vk/window_nextui.cpp): opened again
// here, as the window's reading stops while the guest is held
int openPad() {
    char path[32], name[128];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && std::strcmp(name, "TRIMUI Player1") == 0)
            return fd;
        close(fd);
    }
    return -1;
}

// the disc's name, for the start page left on the screen on leaving for Deadcast (atexit)
std::string g_leaving;

class Menu final : public dream::host::Extension, public ui::Display {
public:
    const char* name() const override { return "deadcast"; }

    // `<module> <disc>`, or the disc after --disc once the port layer has turned it into flags
    void adjust_args(std::vector<std::string>& args) override {
        for (std::size_t i = 1; i < args.size(); ++i)
            if (args[i] == "--disc" && i + 1 < args.size())
                disc_ = args[i + 1];
        if (disc_.empty() && args.size() == 2 && args[1].rfind("--", 0) != 0)
            disc_ = args[1];
    }

    // MENU is the menu's, never a quit (the window then leaves it alone)
    bool wants_overlay() override { return true; }

    void on_stop(dream::System&, const char*) override { deadcast::recomp::runEnded(); }

    // The start page is Deadcast's (put on the screen as it runs the module, frontend/recomp.cpp), up
    // until the window first presents: the game's first picture. Not drawn again here: drawn before
    // the first guest instruction, the game's first textured frame came up garbled (2026-10-31).
    void on_start(dream::System&) override {
        pad_ = openPad();
        ui::setDisplay(this);
        // Show FPS (every module's Recomp Settings), kept in Deadcast's store
        const PortInfo* p = port();
        fps_ = p && p->id && deadcast::recomp::get(std::string(p->id) + ".fps", "off") == "on";
    }

    // MENU while the game runs: its picture taken at the next present, then the menu over it
    void on_vblank(dream::System&) override {
        perf();
        for (const input_event& ev : read())
            if (ev.type == EV_KEY && ev.code == BTN_MODE && ev.value == 1 && state_ == State::Closed) {
                corner(nullptr);  // not over the menu
                std::lock_guard<std::mutex> lock(mutex_);
                captured_ = false;
                state_ = State::Capturing;
            }
    }

    bool overlay_open() override { return state_ != State::Closed; }
    bool guest_paused() override { return state_ != State::Closed; }

    void on_paused_tick() override {
        if (state_ == State::Capturing) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!captured_)
                    return;
            }
            if (!ui::open(frame_, frameWidth_, frameHeight_, ui::mainScreen(title(), host()))) {
                state_ = State::Closed;
                return;
            }
            state_ = State::Open;
            vblanks_ = 0;  // the time held is not the game's
        }
        for (const input_event& ev : read())
            press(ev);
        switch (ui::frame()) {
        case ui::Result::Open:
            return;
        case ui::Result::Switch:
            // the other core chosen (Core Settings stored it): Deadcast starts the game again in it,
            // the start page up from when the window gives the screen back until Deadcast's own
            dream::host::exit_code() = deadcast::recomp::SwitchExit;
            g_leaving = disc_.substr(disc_.find_last_of('/') + 1);
            std::atexit([] { ui::putPage("Loading...", g_leaving); });
            quit();
            break;
        case ui::Result::Quit:
            // back to NextUI: the screen black until it draws (not the start page the window found)
            std::atexit([] { ui::putPage("", ""); });
            quit();
            break;
        default:
            break;
        }
        state_ = State::Closed;
    }

    // the page shown, or nothing while the game's picture is taken (draw_overlay)
    const std::uint32_t* overlay_image(unsigned view_w, unsigned view_h, bool& changed) override {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = changed_;
        changed_ = false;
        if (state_ == State::Capturing || image_.size() != static_cast<std::size_t>(view_w) * view_h)
            return nullptr;
        return image_.data();
    }

    void draw_overlay(std::uint32_t* rgba, unsigned w, unsigned h, unsigned, unsigned) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Capturing || captured_)
            return;
        // RGBA, red in the low byte: as RGB24 for the menu's backdrop
        frame_.resize(static_cast<std::size_t>(w) * h * 3);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
            frame_[i * 3] = static_cast<std::uint8_t>(rgba[i]);
            frame_[i * 3 + 1] = static_cast<std::uint8_t>(rgba[i] >> 8);
            frame_[i * 3 + 2] = static_cast<std::uint8_t>(rgba[i] >> 16);
        }
        frameWidth_ = static_cast<int>(w);
        frameHeight_ = static_cast<int>(h);
        captured_ = true;
    }

    // ui::Display: the overlay, the screen's size (the largest 4:3 picture the panel holds)
    bool begin(int& width, int& height) override {
        unsigned w = 1024, h = 768;
        if (auto& hc = dream::host::host_controls(); hc.render_size)
            hc.render_size(dream::host::HostControls::kScalePanel, w, h);
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        std::lock_guard<std::mutex> lock(mutex_);
        image_.assign(static_cast<std::size_t>(w) * h, 0);
        return true;
    }

    // the canvas's BGRA (0xRRGGBB) as the overlay's opaque RGBA, red in the low byte
    void show(const deadcast::u32* pixels) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < image_.size(); ++i) {
            const std::uint32_t p = pixels[i];
            image_[i] = 0xff000000u | (p & 0x00ff00u) | (p >> 16 & 0xffu) | (p & 0xffu) << 16;
        }
        changed_ = true;
    }

    void end(After) override {}

private:
    enum class State { Closed, Capturing, Open };

    // once a second, as Deadcast's own perf lines: the game's vblanks (60 at full speed)
    // once a second, as Deadcast's own perf lines: the game's vblanks (60 at full speed) and the
    // frames shown, which Show FPS puts in the corner as the emulator's does
    void perf() {
        const auto now = std::chrono::steady_clock::now();
        auto& hc = dream::host::host_controls();
        const std::uint64_t presented = hc.presented_frames ? hc.presented_frames() : 0;
        if (vblanks_++ == 0) {
            perfStart_ = since_ = now;
            presentedAt_ = presented;
            return;
        }
        const double s = std::chrono::duration<double>(now - since_).count();
        if (s < 1.0)
            return;
        const double shown = static_cast<double>(presented - presentedAt_) / s;
        std::printf("Deadcast perf %6.1f s: vblank %5.2f/s presented %5.2f/s\n",
                    std::chrono::duration<double>(now - perfStart_).count(), (vblanks_ - 1) / s, shown);
        std::fflush(stdout);
        if (fps_ && state_ == State::Closed) {
            char text[32];
            std::snprintf(text, sizeof text, "F:%4.1f", shown);
            corner(text);
        }
        vblanks_ = 1;
        since_ = now;
        presentedAt_ = presented;
    }

    // the frame counter in the corner, or none
    static void corner(const char* text) {
        auto& hc = dream::host::host_controls();
        if (!hc.set_corner)
            return;
        int w = 0, h = 0;
        const std::vector<deadcast::u32> px = text ? ui::label(text, w, h) : std::vector<deadcast::u32>();
        hc.set_corner(px.empty() ? nullptr : px.data(), static_cast<unsigned>(w), static_cast<unsigned>(h));
    }

    std::vector<input_event> read() {
        std::vector<input_event> events;
        input_event ev;
        while (pad_ >= 0 && ::read(pad_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev))
            events.push_back(ev);
        return events;
    }

    // the pad's buttons and d-pad as the menu's
    void press(const input_event& ev) {
        if (ev.type == EV_KEY && ev.value != 2) {
            const ui::Button b = ev.code == BTN_EAST ? ui::Button::A : ev.code == BTN_SOUTH ? ui::Button::B
                               : ev.code == BTN_WEST ? ui::Button::X : ev.code == BTN_MODE ? ui::Button::Menu
                               : ui::Button::None;
            if (b != ui::Button::None)
                ui::button(b, ev.value == 1);
        } else if (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)) {
            int& was = ev.code == ABS_HAT0X ? hatX_ : hatY_;
            const ui::Button less = ev.code == ABS_HAT0X ? ui::Button::Left : ui::Button::Up;
            const ui::Button more = ev.code == ABS_HAT0X ? ui::Button::Right : ui::Button::Down;
            if (was != 0)
                ui::button(was < 0 ? less : more, false);
            if (ev.value != 0)
                ui::button(ev.value < 0 ? less : more, true);
            was = ev.value;
        }
    }

    std::string title() const {
        const char* rom = std::getenv("DEADCAST_ROM");
        return ui::gameName(rom != nullptr && *rom != '\0' ? rom : disc_);
    }

    // a recomp's main page: no save states, no emulator; the module's settings applied as they change
    ui::Host host() {
        ui::Host h;
        h.core = ui::Core::Recomp;
        h.disc = disc_;
        h.recompSetting = [this](const std::string& key, const std::string& value) {
            if (key == "scale")
                set_render_scale(value == "panel" ? kScalePanel : std::atoi(value.c_str()));
            else if (key == "fps")
                fps_ = value == "on";  // shown from the next second
        };
        return h;
    }

    static void quit() {
        if (auto& hc = dream::host::host_controls(); hc.quit)
            hc.quit();
    }

    std::string disc_;
    bool fps_ = false;  // Show FPS
    std::uint64_t presentedAt_ = 0;
    unsigned vblanks_ = 0;
    std::chrono::steady_clock::time_point perfStart_, since_;
    int pad_ = -1;
    int hatX_ = 0;
    int hatY_ = 0;
    std::mutex mutex_;
    std::atomic<State> state_ { State::Closed };
    bool captured_ = false;
    std::vector<std::uint8_t> frame_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    std::vector<std::uint32_t> image_;
    bool changed_ = false;
};

Menu g_menu;
dream::host::Register g_register(g_menu);

}  // namespace

}  // namespace dreamcomp
