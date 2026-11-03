// Deadcast: the Brick's screen. VK_KHR_display tears here, so Vulkan renders straight into /dev/fb0:
// its memory is imported as a dma-buf (Allwinner's FBIOGET_DMABUF) and its pages from FirstPage on are
// linear BGRA8 render targets. A thread pans to each frame once its fence signals (FBIOPAN_DISPLAY
// waits for the vblank). Frames are 640 x 480, with fb0 set to that mode and the display engine
// scaling it to the panel, which cuts memory traffic. NextUI's mode and page are restored at the end.
#pragma once
#include "vulkan.h"
#include "wsi/context.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <linux/fb.h>

class Panel
{
public:
	// Deadcast: one on the screen, up to two waiting for their vblank (waitWritable), the rest being drawn
	static constexpr u32 Pages = 5;

	~Panel() { term(); }
	// false, with the reason in error, without fb0 or its import
	bool init(vk::PhysicalDevice physicalDevice, vk::Device device, std::string& error);
	void term();

	vk::Extent2D extent() const { return { mode.xres, mode.yres }; }
	static constexpr vk::Format format() { return vk::Format::eB8G8R8A8Unorm; }
	vk::ImageView view(u32 page) const { return *views[page]; }
	float refreshRate() const { return refresh; }

	// before a frame is recorded into page: until it may be written (off the screen) and at most one
	// frame waits to be shown
	void waitWritable(u32 page);
	// the frame recorded into page, submitted with fence: shown once the fence has signalled
	void queue(u32 page, vk::Fence fence);
	// until every frame queued is shown
	void drain();
	// Deadcast: the frame on the screen as RGB rows, read back from fb0
	bool readShown(std::vector<u8>& rgb, int& width, int& height);

	// Deadcast: CPU-drawn pictures (the menu, the start page) between frames, at the panel's native size:
	// fb0 in NextUI's mode, two pages of their own after ours, written then shown in turn.
	// endPictures restores Deadcast's mode with its last frame (LastFrame), or with the next one (NextFrame,
	// LastFrame without one yet); Stay leaves the last picture up, in NextUI's page too (through Deadcast's
	// end).
	bool beginPictures(int& width, int& height);
	void showPicture(const u32 *bgra);
	void endPictures(GraphicsContext::AfterPictures after);

	// Deadcast: copied onto every frame once it is drawn, just before it is shown (GraphicsContext::setCorners)
	void setCorners(std::vector<GraphicsContext::Corner> corners);
	u64 framesShown();

private:
	void showLoop();
	void drawCorners(u32 page);
	void frameMode(u32 page);
	void pan(u32 page);
	void restoreMode();
	void matchGameRate();
	void restoreRate();

	vk::Device device;
	int fd = -1;
	// Deadcast: fb0 mapped uncached, as the display reads it: write() and read() go through a cached
	// kernel mapping, which left parts of pictures stale on the screen
	u8 *mapped = nullptr;
	size_t mappedSize = 0;
	fb_var_screeninfo var{};		// as found: NextUI's mode and page
	fb_var_screeninfo mode{};		// Deadcast's
	u32 firstRow = 0;				// the first page's
	u32 lineLength = 0;				// fb0's, in bytes
	std::atomic<int> shownPage { -1 };	// the page panned to last
	u32 nativeLine = 0;				// fb0's line, in bytes, in NextUI's mode
	u32 pictureRow = 0;				// the first picture page's, in NextUI's mode
	u32 picturePage = 0;			// the one written next
	bool pictures = false;			// between beginPictures and endPictures
	bool pictureMode = false;		// fb0 in NextUI's mode for them
	std::atomic<bool> modeAtFrame { false };	// back to Deadcast's mode with the next frame shown
	float refresh = 60.f;
	u32 dsiVtFound = 0;				// the DSI's frame register as found, when changed (matchGameRate)
	vk::UniqueDeviceMemory memory;
	std::vector<vk::UniqueImage> images;
	std::vector<vk::UniqueImageView> views;

	struct Frame
	{
		u32 page;
		vk::Fence fence;
	};
	std::thread thread;
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<Frame> frames;		// queued, not shown yet
	u64 queuedCount = 0;
	u64 shownCount = 0;
	bool stopping = false;
	std::mutex cornersMutex;
	std::vector<GraphicsContext::Corner> corners;
};
