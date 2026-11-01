// The recomp modules this Deadcast build offers (frontend/download.h): each a .tar.gz attached to the
// Deadcast release named here, installed only if its SHA-256 is the one below. Written when a release
// is prepared (shell/brick/recomp.sh MODULE DISC release), never by hand: the module's file is built
// first, then this, then Deadcast.
#pragma once
#include <cstdint>

namespace deadcast::recomp
{

struct CatalogEntry
{
	const char *id;
	const char *name;
	const char *version;
	const char *file;			// in the release
	uint64_t size;				// its bytes
	const char *sha256;
	const char *discs[4];		// the discs it plays (their CHDs' header SHA-1), as its manifest
};

// where the release's files are
constexpr const char *Release = "https://github.com/tyrannotorus/emulator-dreamcast-deadcast/releases/download/1.1.0/";

constexpr CatalogEntry Catalog[] = {
	{ "rez", "Rez", "1.0.1", "recomp-rez-1.0.1.tar.gz", 44282356, "295e8b020dafa65134ff7db59056800c8421034772eee12a15034b307d4e926a",
		{ "9711a0d0c21af3685672b9a42c72abf999fc7150", "01b7c1cc29a840b141a4ae713037eb1224fac233" } },
};

}
