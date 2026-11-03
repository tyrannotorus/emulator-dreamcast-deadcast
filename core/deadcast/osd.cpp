#include "osd.h"
#include "ui.h"
#include "cfg/option.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include "wsi/context.h"

#include <cstdio>
#include <mutex>
#include <vector>

namespace deadcast::osd
{

namespace
{

constexpr size_t MaxMessageLines = 4;

std::mutex mutex;
std::string message;		// under mutex
std::string details;
u64 messageEnd = 0;

// the render thread's
u64 lastFrameAt = 0;
u64 countedAt = 0;
u64 countedFrames = 0;
float framesPerSecond = -1.f;
std::string shownCounter;
std::string shownMessage;

// frames shown each second, from what reached the screen, as a module's counter is
std::string counter(GraphicsContext *context, u64 now)
{
	const bool fastForward = settings.input.fastForwardMode;
	if (!config::ShowFPS)
	{
		countedAt = 0;
		framesPerSecond = -1.f;
		return fastForward ? ">>" : "";
	}
	const u64 frames = context->framesShown();
	// the time in the menu is not the game's
	if (countedAt == 0 || now - lastFrameAt > 500)
	{
		countedAt = now;
		countedFrames = frames;
	}
	else if (now - countedAt >= 1000)
	{
		framesPerSecond = (float)(frames - countedFrames) * 1000.f / (float)(now - countedAt);
		countedAt = now;
		countedFrames = frames;
	}
	if (framesPerSecond < 0.f)
		return fastForward ? ">>" : "";
	char text[32];
	std::snprintf(text, sizeof(text), "%4.1f%s", framesPerSecond, fastForward ? " >>" : "");
	return text;
}

}	// namespace

void notify(const std::string& text, int durationMs, const std::string& under)
{
	std::lock_guard<std::mutex> lock(mutex);
	message = text;
	details = under;
	messageEnd = getTimeMs() + durationMs;
}

void frame()
{
	GraphicsContext *context = GraphicsContext::Instance();
	if (context == nullptr)
		return;
	const u64 now = getTimeMs();
	const std::string count = counter(context, now);
	lastFrameAt = now;
	std::string text, under;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!message.empty() && now >= messageEnd)
			message.clear();
		text = message;
		under = details;
	}
	const std::string all = text.empty() ? "" : text + "\n" + under;
	if (count == shownCounter && all == shownMessage)
		return;
	shownCounter = count;
	shownMessage = all;

	const int frameHeight = context->frameHeight();
	const int maxWidth = context->frameWidth();
	std::vector<GraphicsContext::Corner> corners;
	int bottom = 0;
	const auto add = [&](const std::string& line) {
		GraphicsContext::Corner corner;
		corner.pixels = deadui::ui::label(line, frameHeight, corner.width, corner.height);
		if (corner.pixels.empty())
			return;
		corner.bottom = bottom;
		bottom += corner.height;
		corners.push_back(std::move(corner));
	};
	if (!count.empty())
		add(count);
	if (!text.empty())
	{
		std::vector<std::string> lines = deadui::ui::labelLines(text, frameHeight, maxWidth);
		const std::vector<std::string> more = deadui::ui::labelLines(under, frameHeight, maxWidth);
		lines.insert(lines.end(), more.begin(), more.end());
		if (lines.size() > MaxMessageLines)
			lines.resize(MaxMessageLines);
		for (auto line = lines.rbegin(); line != lines.rend(); ++line)
			add(*line);
	}
	context->setCorners(std::move(corners));
}

}
