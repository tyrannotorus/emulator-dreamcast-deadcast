// What the emulator says over the game: the frame counter (Show FPS) and its messages, in the menu's font. Each line
// is a small picture (ui::label) copied onto every frame just before it is shown (GraphicsContext::setCorners), at the
// bottom left: the counter, with any message above it. A hosted module's counter looks the same (ui/src/ui.h).
#pragma once
#include <string>

namespace deadcast::osd
{

// a message for this long, with an optional line under it; from any thread
void notify(const std::string& text, int durationMs, const std::string& details = "");
// each time round the main loop while the game plays, with whether it drew a frame, on the render thread. A game may
// draw nothing for a while (a still screen): text that changes then shows the last frame again, and true is returned.
bool frame(bool rendered);

}
