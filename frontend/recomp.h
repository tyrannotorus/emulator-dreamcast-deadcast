// Recomp modules: recompiled builds of single games, run instead of Deadcast's emulation where one is
// installed for the disc being played. Each lives in its own folder, <home>/recomp/<id>/, with its
// executable and a manifest, module.cfg:
//
//   id = rez                        the folder's name
//   name = Rez
//   version = 1.0.1
//   exec = rez                      the executable, beside the manifest
//   disc = 9711a0d0...              each disc it plays, by the SHA-1 its CHD's header records
//   title = Rez Recompiled          what the menu calls it
//   setting = scale | Resolution | 1=640x480, panel=1024x768 | 1
//                                   each setting, shown in Recomp Options: key | name | value=label, ... | default
//
// A module has no settings of its own in the game: it declares them here, Deadcast's Recomp Options
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
	std::string title;
	std::string version;
	std::string exec;
	std::string dir;
	std::vector<std::string> discs;
	std::vector<Setting> settings;
};
// key = value lines, # comments; a key may come more than once
std::vector<std::pair<std::string, std::string>> keyValues(const std::string& text);
bool readManifest(const std::string& file, Module& module);
// every module installed
std::vector<Module> allInstalled();
// the module installed for a disc (its CHD's SHA-1); false if none. Only a folder named as its manifest's
// id counts: never what an install or a deletion left half done (<id>.new, <id>.old)
bool installed(const std::string& discSha1, Module& module);

// a file, or a folder and everything in it; true if gone
bool removeTree(const std::string& path);

// Deleting a module takes everything of it: its folder, its data (<home>/recomp-data/<id>/: saves,
// settings, cache) and what the store keeps for it. It is marked first (<home>/recomp/<id>.delete) and
// the mark goes last, so a deletion cut short is finished by tidy(). A module deleting itself while it
// runs only marks itself: tidy() deletes it at Deadcast's next start, once it has ended.
void markForDeletion(const std::string& id);
bool deleteModule(const std::string& id);	// true if all of it is gone
// At Deadcast's start, before anything: the deletions marked done, and what an install cut short left
// behind (<id>.download, <id>.new, <id>.old) removed
void tidy();

// The core chosen for a disc in Options: Recomp, its module installed, runs the module in this
// process instead (exec: nothing of the caller stays), given the disc's path. Returns only if not.
void runChosen(const std::string& disc);
// A module's run ended as it should (quit, or the other core chosen): its next start runs it again.
// One that did not end so (it died, or would not start) leaves its game emulated once.
void runEnded();
constexpr const char *RunMarker = "/tmp/deadcast-recomp-run";
// The exit code that asks the launcher to start the game again, in the core now chosen
constexpr int SwitchExit = 86;

// Deadcast's store of what the menu picks for the recomps (key = value lines in <home>/recomp.cfg), each
// change saved at once: core.<module> (the game's core), <module>.<setting> (a module's settings),
// deadcast.version (the Deadcast on this Brick, which a module cannot tell by itself)
std::string get(const std::string& key, const std::string& def = "");
void set(const std::string& key, const std::string& value);

}
