#include "download.h"
#include "catalog.h"
#include "recomp.h"
#include "sha256.h"

#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace deadcast::recomp
{
namespace
{

// where the release's files are: Deadcast's release, or DEADCAST_RECOMP_URL (tests: a server of one's
// own; the checksum still decides)
std::string url(const StoreEntry& entry)
{
	const char *base = std::getenv("DEADCAST_RECOMP_URL");
	return std::string(base != nullptr && *base != '\0' ? base : Release) + entry.file;
}

CURL *session(const std::string& address)
{
	CURL *curl = curl_easy_init();
	if (curl == nullptr)
		return nullptr;
	curl_easy_setopt(curl, CURLOPT_URL, address.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);		// GitHub's release files redirect
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	// no CA certificates on the Brick: the file's SHA-256 is what is trusted (download.h)
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Deadcast");
	return curl;
}

size_t collect(char *data, size_t size, size_t count, void *context)
{
	((std::string *)context)->append(data, size * count);
	return size * count;
}

// a test server's catalog (download.h); empty if there is none to read
std::vector<StoreEntry> testCatalog(const std::string& address)
{
	std::vector<StoreEntry> entries;
	CURL *curl = session(address);
	if (curl == nullptr)
		return entries;
	std::string text;
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &text);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	const CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (res != CURLE_OK)
	{
		std::printf("Deadcast: %s: %s\n", address.c_str(), curl_easy_strerror(res));
		return entries;
	}
	for (const auto& kv : keyValues(text))
	{
		if (kv.first == "module")
		{
			entries.emplace_back();
			entries.back().id = kv.second;
		}
		if (entries.empty())
			continue;
		StoreEntry& entry = entries.back();
		if (kv.first == "name")
			entry.name = kv.second;
		else if (kv.first == "version")
			entry.version = kv.second;
		else if (kv.first == "file")
			entry.file = kv.second;
		else if (kv.first == "size")
			entry.size = std::strtoull(kv.second.c_str(), nullptr, 10);
		else if (kv.first == "sha256")
			entry.sha256 = kv.second;
		else if (kv.first == "title")
			entry.title = kv.second;
		else if (kv.first == "info")
			entry.info = kv.second;
		else if (kv.first == "needs")
			entry.needs = kv.second;
		else if (kv.first == "disc")
			entry.discs.push_back(kv.second);
	}
	// its ids name folders
	entries.erase(std::remove_if(entries.begin(), entries.end(), [](const StoreEntry& entry) {
		return entry.id.empty() || entry.id[0] == '.' || entry.id.find('/') != std::string::npos;
	}), entries.end());
	return entries;
}

struct Fetch
{
	FILE *file;
	Sha256 sha;
	std::atomic<uint64_t> *done;
	std::atomic<uint64_t> *total;
	const std::atomic<bool> *cancel;
};

size_t write(char *data, size_t size, size_t count, void *context)
{
	Fetch& f = *(Fetch *)context;
	const size_t n = size * count;
	f.sha.update(data, n);
	return std::fwrite(data, 1, n, f.file);
}

int progress(void *context, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t)
{
	Fetch& f = *(Fetch *)context;
	if (total > 0)
		*f.total = (uint64_t)total;
	*f.done = (uint64_t)done;
	return *f.cancel ? 1 : 0;
}

uint64_t octal(const char *s, size_t n)
{
	uint64_t v = 0;
	for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
		v = v * 8 + (uint64_t)(s[i] - '0');
	return v;
}

// a .tar.gz's regular files and folders into dir; nothing outside it (no absolute paths, no "..",
// nothing but files and folders). done: the archive's bytes read so far.
std::string unpack(const std::string& archive, const std::string& dir, std::atomic<uint64_t>& done)
{
	gzFile gz = gzopen(archive.c_str(), "rb");
	if (gz == nullptr)
		return "The download cannot be opened.";
	std::string error;
	char header[512];
	std::vector<char> buf(1 << 16);
	while (error.empty())
	{
		if (gzread(gz, header, sizeof(header)) != (int)sizeof(header))
		{
			error = "The download is cut short.";
			break;
		}
		if (header[0] == '\0')
			break;	// the end
		std::string name(header, strnlen(header, 100));
		if (std::memcmp(header + 257, "ustar", 5) == 0 && header[345] != '\0')
			name = std::string(header + 345, strnlen(header + 345, 155)) + "/" + name;
		while (name.rfind("./", 0) == 0)
			name.erase(0, 2);
		const uint64_t size = octal(header + 124, 12);
		const char type = header[156];
		if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos)
		{
			error = "The download holds a file it should not.";
			break;
		}
		const std::string path = dir + "/" + name;
		if (type == '5')
		{
			mkdir(path.c_str(), 0755);
			continue;
		}
		if (type != '0' && type != '\0')
		{
			error = "The download holds a file it should not.";
			break;
		}
		FILE *out = std::fopen(path.c_str(), "wb");
		if (out == nullptr)
		{
			error = "The module cannot be written.";
			break;
		}
		for (uint64_t left = size; left > 0 && error.empty(); )
		{
			const int n = gzread(gz, buf.data(), (unsigned)std::min<uint64_t>(left, buf.size()));
			if (n <= 0 || std::fwrite(buf.data(), 1, (size_t)n, out) != (size_t)n)
				error = n <= 0 ? "The download is cut short." : "The module cannot be written.";
			left -= n > 0 ? (uint64_t)n : left;
			done = (uint64_t)std::max<z_off_t>(gzoffset(gz), 0);
		}
		const bool written = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
		std::fclose(out);
		if (!written && error.empty())
			error = "The module cannot be written.";
		chmod(path.c_str(), (mode_t)(octal(header + 100, 8) & 0755));
		// past the file's padding to the next 512
		const uint64_t pad = (512 - size % 512) % 512;
		if (pad > 0 && error.empty() && gzread(gz, buf.data(), (unsigned)pad) != (int)pad)
			error = "The download is cut short.";
	}
	gzclose(gz);
	return error;
}

}	// namespace

ModuleInfo readInfo(const std::string& text)
{
	ModuleInfo info;
	const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
	if (!json.is_object())
		return info;
	const auto string = [&json](const char *key) {
		const auto it = json.find(key);
		return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
	};
	const auto lines = [&json](const char *key) {
		std::vector<std::string> found;
		const auto it = json.find(key);
		if (it != json.end() && it->is_array())
			for (const auto& line : *it)
				if (line.is_string())
					found.push_back(line.get<std::string>());
		return found;
	};
	info.description = string("description");
	const auto formats = json.find("formats");
	if (formats != json.end() && formats->is_array())
		for (const auto& disc : *formats)
			if (disc.is_object())
				info.formats.push_back({ disc.value("format", ""), disc.value("region", ""), disc.value("sha1", "") });
	info.features = lines("features");
	info.changelog = lines("changelog");
	info.repo = string("repo");
	return info;
}

std::vector<StoreEntry> storeCatalog()
{
	const char *base = std::getenv("DEADCAST_RECOMP_URL");
	if (base != nullptr && *base != '\0')
	{
		std::vector<StoreEntry> entries = testCatalog(std::string(base) + "catalog.cfg");
		if (!entries.empty())
			return entries;
	}
	std::vector<StoreEntry> entries;
	for (const CatalogEntry& built : Catalog)
	{
		if (built.id == nullptr)
			continue;
		StoreEntry entry;
		entry.id = built.id;
		entry.name = built.name;
		entry.version = built.version;
		entry.file = built.file;
		entry.size = built.size;
		entry.sha256 = built.sha256;
		entry.title = built.title;
		entry.info = built.info;
		for (const char *disc : built.discs)
			if (disc != nullptr)
				entry.discs.push_back(disc);
		entries.push_back(std::move(entry));
	}
	return entries;
}

std::string install(const StoreEntry& entry, std::atomic<uint64_t>& done, std::atomic<uint64_t>& total,
		const std::atomic<bool>& cancel, std::atomic<bool>& unpacking)
{
	const std::string dir = home() + "/recomp";
	mkdir(dir.c_str(), 0755);
	const std::string archive = dir + "/" + entry.id + ".download";
	const std::string unpacked = dir + "/" + entry.id + ".new";
	const std::string installed = dir + "/" + entry.id;
	const std::string old = dir + "/" + entry.id + ".old";
	total = entry.size;
	done = 0;

	// fetched, its SHA-256 taken as it comes
	Fetch fetch { std::fopen(archive.c_str(), "wb"), Sha256(), &done, &total, &cancel };
	if (fetch.file == nullptr)
		return "The module cannot be written.";
	CURL *curl = session(url(entry));
	if (curl == nullptr)
	{
		std::fclose(fetch.file);
		std::remove(archive.c_str());
		return "The download cannot start.";
	}
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fetch);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &fetch);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);	// stalled a minute: given up
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	const CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	const bool written = std::fflush(fetch.file) == 0 && fsync(fileno(fetch.file)) == 0;
	std::fclose(fetch.file);
	if (res != CURLE_OK || !written)
	{
		std::remove(archive.c_str());
		if (cancel)
			return "The download was stopped.";
		std::printf("Deadcast: %s: %s\n", url(entry).c_str(), curl_easy_strerror(res));
		return res != CURLE_OK ? "The download failed. Try again later." : "The module cannot be written.";
	}
	// only the very file this build knows
	const std::string sha = fetch.sha.hex();
	if (sha != entry.sha256)
	{
		std::remove(archive.c_str());
		std::printf("Deadcast: %s: SHA-256 %s, not %s\n", entry.file.c_str(), sha.c_str(), entry.sha256.c_str());
		return "The download is not the module it should be. Nothing was installed.";
	}

	// unpacked beside the one installed, then in its place at once; done counts the archive read again
	done = 0;
	total = entry.size;
	unpacking = true;
	removeTree(unpacked);
	if (mkdir(unpacked.c_str(), 0755) != 0)
	{
		std::remove(archive.c_str());
		return "The module cannot be written.";
	}
	std::string error = unpack(archive, unpacked, done);
	std::remove(archive.c_str());
	Module module;
	if (error.empty() && (!readManifest(unpacked + "/module.cfg", module) || module.id != entry.id))
		error = "The download is not the module it should be. Nothing was installed.";
	if (!error.empty())
	{
		removeTree(unpacked);
		return error;
	}
	removeTree(old);
	if (access(installed.c_str(), F_OK) == 0 && std::rename(installed.c_str(), old.c_str()) != 0)
	{
		removeTree(unpacked);
		return "The module cannot be written.";
	}
	if (std::rename(unpacked.c_str(), installed.c_str()) != 0)
	{
		std::rename(old.c_str(), installed.c_str());
		removeTree(unpacked);
		return "The module cannot be written.";
	}
	removeTree(old);
	sync();
	std::printf("Deadcast: recomp module %s %s installed\n", entry.id.c_str(), entry.version.c_str());
	return "";
}

}
