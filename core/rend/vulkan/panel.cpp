#include "panel.h"
#include "hw/pvr/spg.h"
#include "log/Log.h"
#include "oslib/oslib.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace
{
constexpr u32 FirstPage = 16;		// in NextUI's mode: its pages before, ours from there
// Allwinner's dma-buf export of the framebuffer's memory
struct FbDmabuf
{
	u32 fd;
	u32 flags;
};
constexpr unsigned long FBIOGET_DMABUF = _IOR('F', 0x21, FbDmabuf);
}

bool Panel::init(vk::PhysicalDevice physicalDevice, vk::Device device, std::string& error)
{
	this->device = device;
	fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
	{
		error = std::string("/dev/fb0: ") + strerror(errno);
		return false;
	}
	fb_fix_screeninfo fix{};
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0)
	{
		error = "fb0: no screen info";
		return false;
	}
	if (var.bits_per_pixel != 32 || var.red.offset != 16 || var.green.offset != 8 || var.blue.offset != 0)
	{
		error = "fb0 is not BGRA8";
		return false;
	}
	nativeLine = fix.line_length;
	void *map = mmap(nullptr, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map != MAP_FAILED)
	{
		mapped = (u8 *)map;
		mappedSize = fix.smem_len;
	}
	else
		WARN_LOG(RENDERER, "Panel: fb0 not mapped: %s", strerror(errno));
	const double line = var.xres + var.left_margin + var.right_margin + var.hsync_len;
	const double rows = var.yres + var.upper_margin + var.lower_margin + var.vsync_len;
	const double hz = var.pixclock != 0 ? 1e12 / (var.pixclock * line * rows) : 60.0;
	refresh = hz >= 30 && hz <= 120 ? (float)hz : 60.f;

	// 640 x 480, scaled by the display engine (DEADCAST_PANEL_1024=1: the panel's own size, the GPU scaling)
	mode = var;
	if (getenv("DEADCAST_PANEL_1024") == nullptr && var.xres >= 640 && var.yres >= 480)
	{
		mode.xres = 640;
		mode.yres = 480;
		mode.xres_virtual = 640;
		mode.xoffset = mode.yoffset = 0;
		if (ioctl(fd, FBIOPUT_VSCREENINFO, &mode) != 0 || ioctl(fd, FBIOGET_VSCREENINFO, &mode) != 0
				|| ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 || mode.xres != 640 || mode.yres != 480)
		{
			WARN_LOG(RENDERER, "Panel: fb0 cannot be 640 x 480, the panel's size then");
			restoreMode();
			mode = var;
			ioctl(fd, FBIOGET_FSCREENINFO, &fix);
		}
	}
	firstRow = FirstPage * var.yres;
	lineLength = fix.line_length;
	const u32 imageWidth = fix.line_length / 4;
	const vk::DeviceSize pageBytes = (vk::DeviceSize)fix.line_length * mode.yres;
	const vk::DeviceSize size = (vk::DeviceSize)fix.line_length * (firstRow + mode.yres * Pages);
	if (fix.line_length % 4 != 0 || imageWidth < mode.xres || size > fix.smem_len || mode.yres_virtual < firstRow + mode.yres * Pages)
	{
		error = "fb0 has too few pages";
		restoreMode();
		return false;
	}
	FbDmabuf dmabuf{};
	if (ioctl(fd, FBIOGET_DMABUF, &dmabuf) != 0)
	{
		error = std::string("fb0: no dma-buf (") + strerror(errno) + ")";
		return false;
	}
	vk::MemoryFdPropertiesKHR fdProperties = device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, (int)dmabuf.fd);
	if (fdProperties.memoryTypeBits == 0)
	{
		::close((int)dmabuf.fd);
		error = "fb0's dma-buf: no memory type";
		return false;
	}
	const u32 type = __builtin_ctz(fdProperties.memoryTypeBits);
	vk::ImportMemoryFdInfoKHR import(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, (int)dmabuf.fd);	// the memory's once imported
	memory = device.allocateMemoryUnique(vk::MemoryAllocateInfo(size, type, &import));

	for (u32 i = 0; i < Pages; i++)
	{
		vk::ExternalMemoryImageCreateInfo external(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT);
		// as wide as fb0's lines: frames are drawn in its first mode.xres columns
		vk::ImageCreateInfo imageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format(), vk::Extent3D(imageWidth, mode.yres, 1), 1, 1,
				vk::SampleCountFlagBits::e1, vk::ImageTiling::eLinear, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
				vk::SharingMode::eExclusive, nullptr, vk::ImageLayout::eUndefined, &external);
		images.push_back(device.createImageUnique(imageCreateInfo));
		const vk::SubresourceLayout layout = device.getImageSubresourceLayout(*images.back(), vk::ImageSubresource(vk::ImageAspectFlagBits::eColor, 0, 0));
		const vk::MemoryRequirements req = device.getImageMemoryRequirements(*images.back());
		const vk::DeviceSize offset = (vk::DeviceSize)fix.line_length * firstRow + i * pageBytes;
		if (layout.rowPitch != fix.line_length || layout.offset != 0 || !(req.memoryTypeBits & (1u << type))
				|| offset % req.alignment != 0 || offset + req.size > size)
		{
			error = "fb0's pages cannot be images";
			images.clear();
			memory.reset();
			restoreMode();
			return false;
		}
		device.bindImageMemory(*images.back(), *memory, offset);
		views.push_back(device.createImageViewUnique(vk::ImageViewCreateInfo(vk::ImageViewCreateFlags(), *images.back(), vk::ImageViewType::e2D,
				format(), vk::ComponentMapping(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))));
	}
	matchGameRate();
	NOTICE_LOG(RENDERER, "Panel: %d x %d on %d x %d, %.2f Hz, fb0 rows %d to %d", mode.xres, mode.yres, var.xres, var.yres, refresh,
			firstRow, firstRow + mode.yres * Pages - 1);
	// NextUI's mode and picture until something is shown (the start page, then the first frame)
	if (mode.xres != var.xres || mode.yres != var.yres)
	{
		fb_var_screeninfo v = var;
		ioctl(fd, FBIOPUT_VSCREENINFO, &v);
		v = var;
		ioctl(fd, FBIOPAN_DISPLAY, &v);
		modeAtFrame = true;
	}

	stopping = false;
	queuedCount = shownCount = 0;
	thread = std::thread([this] { showLoop(); });
	return true;
}

// Deadcast: the Brick's panel refreshes at about 60.2 Hz, a Dreamcast game at 59.94 Hz, so a frame was shown
// twice every few seconds. While Deadcast runs, the DSI controller's frame is 788 lines instead of 784,
// about 0.5% slower: close to the game's rate. Only when the register holds exactly the Brick panel's
// timing; restored as found at the end. The display driver offers no way to it (DISP_LCD_SET_FPS fails).
static constexpr uintptr_t DsiBase = 0x06504000;	// DSI0 (the display node's reg)
static constexpr u32 DsiFrameReg = 0x1c;			// VT << 16 | lines shown
static constexpr u32 BrickFrame = (784u << 16) | 768u;
static constexpr u32 GameFrame = (788u << 16) | 768u;

static volatile u32 *mapDsi(int& fd, void*& map)
{
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return nullptr;
	map = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DsiBase);
	if (map == MAP_FAILED)
	{
		::close(fd);
		return nullptr;
	}
	return (volatile u32 *)((u8 *)map + DsiFrameReg);
}

static void unmapDsi(int fd, void *map)
{
	munmap(map, 0x1000);
	::close(fd);
}

void Panel::matchGameRate()
{
	int memFd;
	void *map;
	volatile u32 *frame = mapDsi(memFd, map);
	if (frame == nullptr)
	{
		WARN_LOG(RENDERER, "Panel: the DSI's timing cannot be reached: %s", strerror(errno));
		return;
	}
	const u32 found = *frame;
	if (found == BrickFrame)
	{
		*frame = GameFrame;
		dsiVtFound = found;
		refresh = (float)(refresh * 784.0 / 788.0);
		NOTICE_LOG(RENDERER, "Panel: 788 lines a frame (was 784): about %.2f Hz", refresh);
	}
	else if (found == GameFrame)
		refresh = (float)(refresh * 784.0 / 788.0);	// left so by a run that ended without restoring it
	else
		NOTICE_LOG(RENDERER, "Panel: the DSI's frame is %08x, not the Brick's: left as it is", found);
	unmapDsi(memFd, map);
}

void Panel::restoreRate()
{
	if (dsiVtFound == 0)
		return;
	int memFd;
	void *map;
	if (volatile u32 *frame = mapDsi(memFd, map))
	{
		if (*frame == GameFrame)
			*frame = dsiVtFound;
		unmapDsi(memFd, map);
	}
	dsiVtFound = 0;
}

void Panel::term()
{
	if (thread.joinable())
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		changed.notify_all();
		thread.join();
	}
	restoreRate();
	views.clear();
	images.clear();
	if (fd >= 0)
	{
		restoreMode();		// NextUI's mode and page again
		if (mapped != nullptr)
			munmap(mapped, mappedSize);
		mapped = nullptr;
		::close(fd);
		fd = -1;
	}
	memory.reset();
}

void Panel::waitWritable(u32 page)
{
	std::unique_lock<std::mutex> lock(mutex);
	changed.wait(lock, [this] { return queuedCount - shownCount < 2; });
}

void Panel::queue(u32 page, vk::Fence fence)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		frames.push_back({ page, fence });
		queuedCount++;
	}
	changed.notify_all();
}

void Panel::drain()
{
	std::unique_lock<std::mutex> lock(mutex);
	changed.wait(lock, [this] { return shownCount == queuedCount; });
}

void Panel::showLoop()
{
	ThreadName _("deadcast-panel");
	std::unique_lock<std::mutex> lock(mutex);
	while (true)
	{
		changed.wait(lock, [this] { return stopping || !frames.empty(); });
		if (frames.empty())
			break;
		Frame frame = frames.front();
		lock.unlock();
		vk::Result res = device.waitForFences(frame.fence, true, UINT64_MAX);
		if (res != vk::Result::eSuccess)
			WARN_LOG(RENDERER, "Panel: waitForFences failed %d", (int)res);
		if (modeAtFrame.exchange(false))
			frameMode(frame.page);
		pan(frame.page);
		perfShown();
		shownPage = (int)frame.page;
		lock.lock();
		frames.pop_front();
		shownCount++;
		changed.notify_all();
	}
}

bool Panel::readShown(std::vector<u8>& rgb, int& width, int& height)
{
	const int page = shownPage;
	if (fd < 0 || page < 0)
		return false;
	width = mode.xres;
	height = mode.yres;
	std::vector<u8> row(lineLength);
	rgb.resize((size_t)width * height * 3);
	for (int y = 0; y < height; y++)
	{
		const off_t offset = (off_t)lineLength * (firstRow + page * mode.yres + y);
		if (mapped != nullptr)
			memcpy(row.data(), mapped + offset, (size_t)width * 4);
		else if (pread(fd, row.data(), lineLength, offset) != (ssize_t)lineLength)
			return false;
		u8 *out = &rgb[(size_t)y * width * 3];
		for (int x = 0; x < width; x++)
		{
			out[x * 3] = row[x * 4 + 2];
			out[x * 3 + 1] = row[x * 4 + 1];
			out[x * 3 + 2] = row[x * 4];
		}
	}
	return true;
}

bool Panel::beginPictures(int& width, int& height)
{
	if (fd < 0 || nativeLine < var.xres * 4)
		return false;
	drain();
	// past our pages, and past the one NextUI showed last (shown again at the end)
	const u64 ours = (u64)lineLength * (firstRow + Pages * mode.yres);
	u32 row = (u32)((ours + nativeLine - 1) / nativeLine);
	if (row < var.yoffset + var.yres && row + 2 * var.yres > var.yoffset)
		row = var.yoffset + var.yres;
	if (row + 2 * var.yres > var.yres_virtual)
	{
		WARN_LOG(RENDERER, "Panel: no room in fb0 for pictures");
		return false;
	}
	pictureRow = row;
	picturePage = 0;
	pictures = true;
	// still in NextUI's mode if no frame came after the last ones
	pictureMode = modeAtFrame.exchange(false);
	width = var.xres;
	height = var.yres;
	return true;
}

void Panel::showPicture(const u32 *bgra)
{
	if (!pictures)
		return;
	const u32 row = pictureRow + picturePage * var.yres;
	const size_t rowBytes = (size_t)var.xres * 4;
	if (mapped != nullptr)
	{
		for (u32 y = 0; y < var.yres; y++)
			memcpy(mapped + (size_t)nativeLine * (row + y), bgra + (size_t)y * var.xres, rowBytes);
		// all of it in memory before the display is pointed at it
		__asm__ volatile("dsb st" ::: "memory");
	}
	else if (rowBytes == nativeLine)
	{
		if (pwrite(fd, bgra, rowBytes * var.yres, (off_t)nativeLine * row) != (ssize_t)(rowBytes * var.yres))
			WARN_LOG(RENDERER, "Panel: picture not written: %s", strerror(errno));
	}
	else
	{
		for (u32 y = 0; y < var.yres; y++)
			if (pwrite(fd, bgra + (size_t)y * var.xres, rowBytes, (off_t)nativeLine * (row + y)) != (ssize_t)rowBytes)
			{
				WARN_LOG(RENDERER, "Panel: picture not written: %s", strerror(errno));
				break;
			}
	}
	fb_var_screeninfo v = var;
	v.xoffset = 0;
	v.yoffset = row;
	// the first: NextUI's mode, already at this page
	if (!pictureMode && (mode.xres != var.xres || mode.yres != var.yres) && ioctl(fd, FBIOPUT_VSCREENINFO, &v) != 0)
		WARN_LOG(RENDERER, "Panel: fb0 not in its own mode for pictures: %s", strerror(errno));
	pictureMode = true;
	v = var;
	v.xoffset = 0;
	v.yoffset = row;
	ioctl(fd, FBIOPAN_DISPLAY, &v);
	picturePage ^= 1;
}

void Panel::endPictures(GraphicsContext::AfterPictures after)
{
	if (!pictures)
		return;
	pictures = false;
	if (after == GraphicsContext::AfterPictures::Stay && pictureMode)
	{
		// the last picture into NextUI's page too, so that it stays up through the return to NextUI's
		// mode and page as Deadcast ends (a core switch's start page, a quit's last picture)
		const u32 from = pictureRow + (picturePage ^ 1) * var.yres;
		const size_t bytes = (size_t)nativeLine * var.yres;
		if (mapped != nullptr)
		{
			memmove(mapped + (size_t)nativeLine * var.yoffset, mapped + (size_t)nativeLine * from, bytes);
			__asm__ volatile("dsb st" ::: "memory");
		}
		else
		{
			std::vector<u8> rows(bytes);
			if (pread(fd, rows.data(), bytes, (off_t)nativeLine * from) != (ssize_t)bytes
					|| pwrite(fd, rows.data(), bytes, (off_t)nativeLine * var.yoffset) != (ssize_t)bytes)
				WARN_LOG(RENDERER, "Panel: the last picture not kept: %s", strerror(errno));
		}
	}
	if (after == GraphicsContext::AfterPictures::Stay || !pictureMode)
		return;
	pictureMode = false;
	const int page = shownPage;
	if (after == GraphicsContext::AfterPictures::NextFrame || page < 0)
	{
		// the pictures stay up until the next frame (the first, if none yet)
		modeAtFrame = true;
		return;
	}
	frameMode(page);
	pan(page);
}

// Deadcast's mode again (from the pictures'), at page
void Panel::frameMode(u32 page)
{
	fb_var_screeninfo v = mode;
	v.xoffset = 0;
	v.yoffset = firstRow + page * mode.yres;
	fb_fix_screeninfo fix{};
	if ((mode.xres != var.xres || mode.yres != var.yres)
			&& (ioctl(fd, FBIOPUT_VSCREENINFO, &v) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 || fix.line_length != lineLength))
		WARN_LOG(RENDERER, "Panel: fb0 not in Deadcast's mode again");
}

void Panel::restoreMode()
{
	if (mode.xres != var.xres || mode.yres != var.yres)
	{
		fb_var_screeninfo v = var;
		ioctl(fd, FBIOPUT_VSCREENINFO, &v);
		mode = var;
	}
	fb_var_screeninfo v = var;
	ioctl(fd, FBIOPAN_DISPLAY, &v);
}

void Panel::pan(u32 page)
{
	fb_var_screeninfo v = mode;
	v.xoffset = 0;
	v.yoffset = firstRow + page * mode.yres;
	if (ioctl(fd, FBIOPAN_DISPLAY, &v) != 0)
	{
		static bool told;
		if (!told)
			WARN_LOG(RENDERER, "Panel: FBIOPAN_DISPLAY failed: %s", strerror(errno));
		told = true;
	}
}
