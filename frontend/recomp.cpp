#include "recomp.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include <dirent.h>
#include <unistd.h>

namespace deadcast::recomp
{
namespace
{

std::string trim(const std::string& s)
{
	const size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return "";
	return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

std::vector<std::string> split(const std::string& s, char by)
{
	std::vector<std::string> parts;
	for (size_t start = 0, end; start <= s.size(); start = end + 1)
	{
		end = s.find(by, start);
		if (end == std::string::npos)
			end = s.size();
		parts.push_back(trim(s.substr(start, end - start)));
	}
	return parts;
}

// key = value lines, # comments; a key may come more than once
std::vector<std::pair<std::string, std::string>> readLines(const std::string& file)
{
	std::vector<std::pair<std::string, std::string>> lines;
	FILE *f = std::fopen(file.c_str(), "r");
	if (f == nullptr)
		return lines;
	char buf[1024];
	while (std::fgets(buf, sizeof(buf), f) != nullptr)
	{
		const std::string line = trim(buf);
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos)
			continue;
		lines.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
	}
	std::fclose(f);
	return lines;
}

std::string storeFile()
{
	return home() + "/recomp.cfg";
}

std::map<std::string, std::string>& store()
{
	static std::map<std::string, std::string> values;
	static bool loaded = false;
	if (!loaded)
	{
		for (const auto& kv : readLines(storeFile()))
			values[kv.first] = kv.second;
		loaded = true;
	}
	return values;
}

}	// namespace

std::string home()
{
	const char *dir = std::getenv("DEADCAST_HOME");
	return dir != nullptr ? dir : ".";
}

std::string chdSha1(const std::string& path)
{
	unsigned char h[104];
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return "";
	const bool read = std::fread(h, 1, sizeof(h), f) == sizeof(h);
	std::fclose(f);
	// "MComprHD", the header's length, its version (5): the SHA-1 of the data and metadata at 84
	if (!read || std::memcmp(h, "MComprHD", 8) != 0 || h[12] != 0 || h[13] != 0 || h[14] != 0 || h[15] != 5)
		return "";
	static const char hex[] = "0123456789abcdef";
	std::string s;
	for (int i = 84; i < 104; i++)
	{
		s += hex[h[i] >> 4];
		s += hex[h[i] & 15];
	}
	return s;
}

bool readManifest(const std::string& file, Module& module)
{
	module = Module();
	module.dir = file.substr(0, file.find_last_of('/'));
	for (const auto& kv : readLines(file))
	{
		if (kv.first == "id")
			module.id = kv.second;
		else if (kv.first == "name")
			module.name = kv.second;
		else if (kv.first == "version")
			module.version = kv.second;
		else if (kv.first == "exec")
			module.exec = kv.second;
		else if (kv.first == "disc")
			module.discs.push_back(kv.second);
		else if (kv.first == "setting")
		{
			const std::vector<std::string> fields = split(kv.second, '|');
			if (fields.size() != 4)
				continue;
			Setting setting { fields[0], fields[1], {}, fields[3] };
			for (const std::string& choice : split(fields[2], ','))
			{
				const size_t eq = choice.find('=');
				if (eq != std::string::npos)
					setting.values.emplace_back(trim(choice.substr(0, eq)), trim(choice.substr(eq + 1)));
			}
			if (!setting.values.empty())
				module.settings.push_back(std::move(setting));
		}
	}
	return !module.id.empty() && !module.exec.empty() && !module.discs.empty();
}

bool installed(const std::string& discSha1, Module& module)
{
	if (discSha1.empty())
		return false;
	const std::string dir = home() + "/recomp";
	DIR *d = opendir(dir.c_str());
	if (d == nullptr)
		return false;
	bool found = false;
	while (!found)
	{
		const dirent *e = readdir(d);
		if (e == nullptr)
			break;
		if (e->d_name[0] == '.')
			continue;
		Module m;
		if (!readManifest(dir + "/" + e->d_name + "/module.cfg", m) || access((m.dir + "/" + m.exec).c_str(), X_OK) != 0)
			continue;
		for (const std::string& disc : m.discs)
			if (disc == discSha1)
			{
				module = std::move(m);
				found = true;
				break;
			}
	}
	closedir(d);
	return found;
}

void runChosen(const std::string& disc)
{
	Module module;
	if (!installed(chdSha1(disc), module) || get("core." + module.id) != "recomp")
		return;
	const std::string exec = module.dir + "/" + module.exec;
	std::printf("Deadcast: %s %s, the recomp module\n", exec.c_str(), module.version.c_str());
	std::fflush(stdout);
	execl(exec.c_str(), exec.c_str(), disc.c_str(), (char *)nullptr);
	std::printf("Deadcast: %s cannot run (%s): emulated instead\n", exec.c_str(), std::strerror(errno));
}

std::string get(const std::string& key, const std::string& def)
{
	const auto it = store().find(key);
	return it != store().end() ? it->second : def;
}

void set(const std::string& key, const std::string& value)
{
	store()[key] = value;
	// all of it again, through a new file: never half written
	const std::string file = storeFile();
	const std::string temp = file + ".new";
	FILE *f = std::fopen(temp.c_str(), "w");
	if (f == nullptr)
		return;
	for (const auto& kv : store())
		std::fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
	const bool written = std::fflush(f) == 0 && fsync(fileno(f)) == 0;
	std::fclose(f);
	if (written)
		std::rename(temp.c_str(), file.c_str());
	else
		std::remove(temp.c_str());
}

}
