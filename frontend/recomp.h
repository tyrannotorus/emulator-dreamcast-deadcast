// Recomp modules: recompiled builds of single games, run instead of Deadcast's emulation where one is
// installed for the disc being played. Each lives in its own folder, <home>/recomp/<id>/, with its
// executable and a manifest, module.cfg:
//
//   id = rez                        the folder's name
//   name = Rez
//   version = 1.0.1
//   exec = rez                      the executable, beside the manifest
//   disc = 9711a0d0...              each disc it plays, by the SHA-1 its CHD's header records
//   setting = scale | Resolution | 1=640x480, panel=1024x768 | 1
//                                   each setting, shown in Recomp Settings: key | name | value=label, ... | default
//
// A module has no settings of its own in the game: it declares them here, Deadcast's Recomp Settings
// shows them, and the store below keeps them (the module reads them from it).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace deadcast::recomp
{

// Deadcast's own folder (DEADCAST_HOME): its settings, the modules
std::string home();

// the SHA-1 a CHD's header records (version 5), in lowercase hex; empty for anything else
std::string chdSha1(const std::string& path);

struct Setting
{
	std::string key;
	std::string name;
	std::vector<std::pair<std::string, std::string>> values;	// value, label
	std::string def;
};
struct Module
{
	std::string id;
	std::string name;
	std::string version;
	std::string exec;
	std::string dir;
	std::vector<std::string> discs;
	std::vector<Setting> settings;
};
bool readManifest(const std::string& file, Module& module);
// the module installed for a disc (its CHD's SHA-1); false if none
bool installed(const std::string& discSha1, Module& module);

// The core chosen for a disc in Core Settings: Recomp, its module installed, runs the module in this
// process instead (exec: nothing of the caller stays), given the disc's path. Returns only if not.
void runChosen(const std::string& disc);
// The exit code that asks the launcher to start the game again, in the core now chosen
constexpr int SwitchExit = 3;

// Deadcast's store of what the menu picks for the recomps (key = value lines in <home>/recomp.cfg), each
// change saved at once: core.<module> (the game's core), <module>.<setting> (a module's settings)
std::string get(const std::string& key, const std::string& def = "");
void set(const std::string& key, const std::string& value);

}
