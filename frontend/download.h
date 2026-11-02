// Recomp modules fetched from Deadcast's own release (frontend/catalog.h) and installed in
// <home>/recomp/<id>/ (frontend/recomp.h). A file is trusted by the SHA-256 this build carries, never
// by where it came from: anything else fails the check and is deleted. (The Brick has no CA
// certificates, so the connection is not verified; the checksum is what decides.)
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace deadcast::recomp
{

// a module the Recomp Store offers
struct StoreEntry
{
	std::string id;
	std::string name;
	std::string version;
	std::string file;			// in the release
	uint64_t size = 0;			// its bytes
	std::string sha256;
	std::string title;			// what the menu calls it
	std::string info;			// its store.json
	std::string needs;			// the oldest Deadcast that can run it, if any is too old
	std::vector<std::string> discs;
};

// What a module's page in the store says, from the store.json every module comes with. It travels in the
// catalog too, to be read before the module is downloaded.
struct ModuleInfo
{
	struct Format
	{
		std::string format;		// CHD, BIN/CUE, CDI...
		std::string region;
		std::string sha1;
	};
	std::string description;
	std::vector<Format> formats;			// the discs it plays
	std::vector<std::string> features;		// a line each
	std::vector<std::string> changelog;		// what changed in this version, a line each
	std::string repo;						// a link
};
// from store.json's text; whatever is missing or malformed stays empty
ModuleInfo readInfo(const std::string& json);

// what the store offers: this build's catalog. In a test, DEADCAST_RECOMP_URL's catalog.cfg instead if it
// can be fetched (network: there is a network to ask): key = value lines, a module = line starting each
// entry, the other keys named as above (info = its store.json on one line), disc = once a disc. That file carries no signature, so nothing
// but a test may ever read one.
std::vector<StoreEntry> storeCatalog(bool network);

// fetched, checked, unpacked and installed, replacing the module installed before in one rename;
// done and total (bytes) as it goes, stopped when cancel turns true (until it is checked: then unpacking
// says so, and done and total count the archive unpacked). Empty when installed, else what went wrong.
std::string install(const StoreEntry& entry, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
		const std::atomic<bool>& cancel, std::atomic<bool>& unpacking);

}
