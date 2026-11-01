// Recomp modules fetched from Deadcast's own release (frontend/catalog.h) and installed in
// <home>/recomp/<id>/ (frontend/recomp.h). A file is trusted by the SHA-256 this build carries, never
// by where it came from: anything else fails the check and is deleted. (The Brick has no CA
// certificates, so the connection is not verified; the checksum is what decides.)
#pragma once
#include "catalog.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace deadcast::recomp
{

// the catalog's module for a disc (its CHD's SHA-1); nullptr if none
const CatalogEntry *catalogFor(const std::string& discSha1);

// the release's file for it there (asked for, not fetched): empty if so, else why not
std::string reachable(const CatalogEntry& entry);

// fetched, checked, unpacked and installed, replacing the module installed before in one rename;
// done and total (bytes) as it goes, stopped when cancel turns true (until it is checked: then unpacking
// says so, and done and total count the archive unpacked). Empty when installed, else what went wrong.
std::string install(const CatalogEntry& entry, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
		const std::atomic<bool>& cancel, std::atomic<bool>& unpacking);

}
