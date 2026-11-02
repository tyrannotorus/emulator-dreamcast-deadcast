#include "recomp.h"
#include "ui.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
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

std::string storeFile()
{
	return home() + "/recomp.cfg";
}

std::string modulesDir()
{
	return home() + "/recomp";
}

bool gone(const std::string& path)
{
	return access(path.c_str(), F_OK) != 0 && errno == ENOENT;
}

std::map<std::string, std::string>& store()
{
	static std::map<std::string, std::string> values;
	static bool loaded = false;
	if (!loaded)
	{
		for (const auto& kv : keyValues(readText(storeFile())))
			values[kv.first] = kv.second;
		loaded = true;
	}
	return values;
}

}	// namespace

std::string readText(const std::string& file)
{
	FILE *f = std::fopen(file.c_str(), "r");
	if (f == nullptr)
		return "";
	std::string text;
	char buf[1024];
	while (const size_t n = std::fread(buf, 1, sizeof(buf), f))
		text.append(buf, n);
	std::fclose(f);
	return text;
}

std::vector<std::pair<std::string, std::string>> keyValues(const std::string& text)
{
	std::vector<std::pair<std::string, std::string>> lines;
	for (size_t start = 0, end; start < text.size(); start = end + 1)
	{
		end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = trim(text.substr(start, end - start));
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos)
			continue;
		lines.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
	}
	return lines;
}

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
	for (const auto& kv : keyValues(readText(file)))
	{
		if (kv.first == "id")
			module.id = kv.second;
		else if (kv.first == "name")
			module.name = kv.second;
		else if (kv.first == "title")
			module.title = kv.second;
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

std::vector<Module> allInstalled()
{
	std::vector<Module> modules;
	const std::string dir = modulesDir();
	DIR *d = opendir(dir.c_str());
	if (d == nullptr)
		return modules;
	while (const dirent *e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		Module m;
		if (readManifest(dir + "/" + e->d_name + "/module.cfg", m) && m.id == e->d_name
				&& access((m.dir + "/" + m.exec).c_str(), X_OK) == 0)
			modules.push_back(std::move(m));
	}
	closedir(d);
	return modules;
}

bool installed(const std::string& discSha1, Module& module)
{
	if (discSha1.empty())
		return false;
	for (Module& m : allInstalled())
		if (std::find(m.discs.begin(), m.discs.end(), discSha1) != m.discs.end())
		{
			module = std::move(m);
			return true;
		}
	return false;
}

void runChosen(const std::string& disc)
{
	Module module;
	if (!installed(chdSha1(disc), module) || get("core." + module.id) != "recomp")
		return;
	// the module's last run never ended (it died, or would not start): emulated this time, so the
	// player can still reach Options
	if (access(RunMarker, F_OK) == 0)
	{
		std::remove(RunMarker);
		std::printf("Deadcast: the recomp module did not end its last run: emulated instead\n");
		return;
	}
	if (FILE *f = std::fopen(RunMarker, "w"))
		std::fclose(f);
	const std::string exec = module.dir + "/" + module.exec;
	// the start page from now until the module's own (its window takes a moment)
	ui::putPage("Loading...", disc.substr(disc.find_last_of('/') + 1));
	std::printf("Deadcast: %s %s, the recomp module\n", exec.c_str(), module.version.c_str());
	std::fflush(stdout);
	execl(exec.c_str(), exec.c_str(), disc.c_str(), (char *)nullptr);
	std::printf("Deadcast: %s cannot run (%s): emulated instead\n", exec.c_str(), std::strerror(errno));
	std::remove(RunMarker);
}

void runEnded()
{
	std::remove(RunMarker);
}

std::string get(const std::string& key, const std::string& def)
{
	const auto it = store().find(key);
	return it != store().end() ? it->second : def;
}

namespace
{

// all of the store again, through a new file: never half written
void save()
{
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

// what the store keeps for a module dropped: its core and its settings
void forget(const std::string& id)
{
	std::map<std::string, std::string>& values = store();
	for (auto it = values.begin(); it != values.end(); )
		it = it->first == "core." + id || it->first.rfind(id + ".", 0) == 0 ? values.erase(it) : std::next(it);
	save();
}

}	// namespace

void set(const std::string& key, const std::string& value)
{
	store()[key] = value;
	save();
}

bool removeTree(const std::string& path)
{
	DIR *d = opendir(path.c_str());
	if (d == nullptr)
		return std::remove(path.c_str()) == 0 || gone(path);
	while (const dirent *e = readdir(d))
	{
		if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
			continue;
		removeTree(path + "/" + e->d_name);
	}
	closedir(d);
	return rmdir(path.c_str()) == 0 || gone(path);
}

void markForDeletion(const std::string& id)
{
	const std::string mark = modulesDir() + "/" + id + ".delete";
	if (FILE *f = std::fopen(mark.c_str(), "w"))
	{
		fsync(fileno(f));
		std::fclose(f);
	}
}

bool deleteModule(const std::string& id)
{
	if (id.empty() || id.find('/') != std::string::npos || id[0] == '.')
		return false;
	markForDeletion(id);
	// out of sight first (installed() reads <id>/ only), then removed
	const std::string dir = modulesDir() + "/" + id;
	const std::string old = dir + ".old";
	removeTree(old);
	std::rename(dir.c_str(), old.c_str());
	const std::string data = home() + "/recomp-data/" + id;
	const bool all = removeTree(old) && removeTree(dir) && removeTree(data);
	forget(id);
	if (all)
		std::remove((dir + ".delete").c_str());
	sync();
	std::printf("Deadcast: recomp module %s %s\n", id.c_str(), all ? "deleted" : "not all deleted");
	return all;
}

void tidy()
{
	const std::string dir = modulesDir();
	DIR *d = opendir(dir.c_str());
	if (d == nullptr)
		return;
	std::vector<std::string> names;
	while (const dirent *e = readdir(d))
		names.push_back(e->d_name);
	closedir(d);
	for (const std::string& name : names)
	{
		const size_t dot = name.find_last_of('.');
		if (dot == std::string::npos || dot == 0)
			continue;
		const std::string kind = name.substr(dot);
		if (kind == ".delete")
			deleteModule(name.substr(0, dot));
		else if (kind == ".download" || kind == ".new" || kind == ".old")
		{
			removeTree(dir + "/" + name);
			std::printf("Deadcast: %s/%s left by an install cut short: removed\n", dir.c_str(), name.c_str());
		}
	}
}

}
