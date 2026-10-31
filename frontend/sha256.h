// SHA-256 (FIPS 180-4), for checking a downloaded recomp module against the checksum Deadcast was
// built with (frontend/catalog.h)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace deadcast
{

class Sha256
{
public:
	Sha256();
	void update(const void *data, size_t size);
	// the digest in lowercase hex; the object is spent
	std::string hex();

private:
	void block(const uint8_t *p);

	uint32_t h[8];
	uint8_t buf[64];
	size_t used = 0;
	uint64_t total = 0;
};

}
