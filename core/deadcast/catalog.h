// The recomp modules this build of Deadcast offers, handed to the store as it starts. Each is a tarball its own
// repository releases, installed only if its SHA-256 is the one below. Written by shell/brick/catalog.sh.
#pragma once
#include "download.h"

namespace deadcast
{

inline std::vector<deadui::recomp::StoreEntry> catalog()
{
	return {
		{ "rez", "Rez", "1.0.2", "https://github.com/tyrannotorus/emulator-dreamcast-deadcast/releases/download/1.1.1/recomp-rez-1.0.2.tar.gz", 44298064, "1ea0aa645130c96fda3ad38b72b6ee080c931165dc92f973c1e52d9455fdd8b1", "Rez Recompiled", R"json({"description":"Rez (United Game Artists, 2001), recompiled for the TrimUI Brick.","formats":[{"format":"CHD","region":"Japan","sha1":"9711a0d0c21af3685672b9a42c72abf999fc7150"},{"format":"CHD","region":"Europe","sha1":"01b7c1cc29a840b141a4ae713037eb1224fac233"}],"features":["60 fps: the stages are drawn at 60 frames a second, where the original draws them at 30.","Resolution: 640x480 or 1024x768 (the Brick's screen), set in Recomp Options."],"changelog":["Opening the menu no longer shows garbled graphics."],"repo":"https://github.com/tyrannotorus/emulator-dreamcast-deadcast-recomp-rez-recompiled"})json", "", { "9711a0d0c21af3685672b9a42c72abf999fc7150", "01b7c1cc29a840b141a4ae713037eb1224fac233" } },
	};
}

}
