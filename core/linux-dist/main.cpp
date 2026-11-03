#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif
#include "types.h"

#if defined(__unix__) || defined(__HAIKU__)
#include "log/LogManager.h"
#include "emulator.h"
#include "ui/mainui.h"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "stdclass.h"
#include "deadcast/menu.h"
#include "recomp.h"
#include "version.h"

#include <csignal>
#include <string>
#include <unistd.h>
#include <vector>
#include <exception>


#if defined(USE_SDL)
	#include "sdl/sdl.h"
#endif

#ifdef USE_BREAKPAD
#include "breakpad/client/linux/handler/exception_handler.h"
#endif

// SDL's events are handled by its input (sdl/sdl.cpp)
void os_DoEvents()
{
}

void common_linux_setup();

// Deadcast: everything is kept in DEADCAST_HOME, if set
static std::string deadcastHome()
{
	const char *home = nowide::getenv("DEADCAST_HOME");
	if (home == nullptr || home[0] == '\0')
		return {};
	std::string path = std::string(home) + "/";
	struct stat info;
	if (flycast::stat(path.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
		flycast::mkdir(path.c_str(), 0755);
	return path;
}

// Find the user config directory.
// $DEADCAST_HOME, else $HOME/.config/Deadcast on linux
static std::string find_user_config_dir()
{
	if (std::string home = deadcastHome(); !home.empty())
		return home;
	std::string xdg_home;
	if (nowide::getenv("XDG_CONFIG_HOME") != nullptr)
		// If XDG_CONFIG_HOME is set explicitly, we'll use that instead of $HOME/.config
		xdg_home = (std::string)nowide::getenv("XDG_CONFIG_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		/* If $XDG_CONFIG_HOME is not set, we're supposed to use "$HOME/.config" instead.
		 * Consult the XDG Base Directory Specification for details:
		 *   http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html#variables
		 */
		xdg_home = (std::string)nowide::getenv("HOME") + "/.config";

	if (!xdg_home.empty())
	{
		std::string fullpath = xdg_home + "/Deadcast/";
		struct stat info;
		if (flycast::stat(fullpath.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
			// Create .config/Deadcast
			flycast::mkdir(fullpath.c_str(), 0755);

		return fullpath;
	}
	// Unable to detect config dir, use the current folder
	return ".";
}

// Find the user data directory.
// $DEADCAST_HOME, else $HOME/.local/share/Deadcast on linux
static std::string find_user_data_dir()
{
	if (std::string home = deadcastHome(); !home.empty())
		return home;
	std::string xdg_home;
	if (nowide::getenv("XDG_DATA_HOME") != nullptr)
		// If XDG_DATA_HOME is set explicitly, we'll use that instead of $HOME/.local/share
		xdg_home = (std::string)nowide::getenv("XDG_DATA_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		/* If $XDG_DATA_HOME is not set, we're supposed to use "$HOME/.local/share" instead.
		 * Consult the XDG Base Directory Specification for details:
		 *   http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html#variables
		 */
		xdg_home = (std::string)nowide::getenv("HOME") + "/.local/share";

	if (!xdg_home.empty())
	{
		std::string fullpath = xdg_home + "/Deadcast/";
		struct stat info;
		if (flycast::stat(fullpath.c_str(), &info) != 0 || (info.st_mode & S_IFDIR) == 0)
			// Create .local/share/Deadcast
			flycast::mkdir(fullpath.c_str(), 0755);

		return fullpath;
	}
	// Unable to detect data dir, use the current folder
	return ".";
}

static void addDirectoriesFromPath(std::vector<std::string>& dirs, const std::string& path, const std::string& suffix)
{
	std::string::size_type pos = 0;
	std::string::size_type n = path.find(':', pos);
	while (n != std::string::npos)
	{
		if (n != pos)
			dirs.push_back(path.substr(pos, n - pos) + suffix);
		pos = n + 1;
		n = path.find(':', pos);
	}
	// Separator not found
	if (pos < path.length())
		dirs.push_back(path.substr(pos) + suffix);
}

// Find a file in the user and system config directories.
// The following folders are checked in this order:
// $DEADCAST_HOME
// $HOME/.config/Deadcast
// if XDG_CONFIG_DIRS is defined:
//   <$XDG_CONFIG_DIRS>/Deadcast
// else
//   /etc/Deadcast/
//   /etc/xdg/Deadcast/
// .
static std::vector<std::string> find_system_config_dirs()
{
	std::vector<std::string> dirs;
	if (std::string home = deadcastHome(); !home.empty())
		dirs.push_back(home);

	std::string xdg_home;
	if (nowide::getenv("XDG_CONFIG_HOME") != nullptr)
		// If XDG_CONFIG_HOME is set explicitly, we'll use that instead of $HOME/.config
		xdg_home = (std::string)nowide::getenv("XDG_CONFIG_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		xdg_home = (std::string)nowide::getenv("HOME") + "/.config";
	if (!xdg_home.empty())
		// XDG config locations
		dirs.push_back(xdg_home + "/Deadcast/");

	if (nowide::getenv("XDG_CONFIG_DIRS") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("XDG_CONFIG_DIRS");
		addDirectoriesFromPath(dirs, path, "/Deadcast/");
	}
	else
	{
#ifdef FLYCAST_SYSCONFDIR
		const std::string config_dir(FLYCAST_SYSCONFDIR);
		dirs.push_back(config_dir);
#endif
		dirs.push_back("/etc/Deadcast/"); // This isn't part of the XDG spec, but much more common than /etc/xdg/
		dirs.push_back("/etc/xdg/Deadcast/");
	}
	dirs.push_back("./");

	return dirs;
}

// Find a file in the user data directories.
// The following folders are checked in this order:
// $DEADCAST_HOME
// $HOME/.local/share/Deadcast
// if XDG_DATA_DIRS is defined:
//   <$XDG_DATA_DIRS>/Deadcast
// else
//   /usr/local/share/Deadcast
//   /usr/share/Deadcast
// <$DEADCAST_BIOS_PATH>
// ./
// ./data
static std::vector<std::string> find_system_data_dirs()
{
	std::vector<std::string> dirs;
	if (std::string home = deadcastHome(); !home.empty())
		dirs.push_back(home);

	std::string xdg_home;
	if (nowide::getenv("XDG_DATA_HOME") != nullptr)
		// If XDG_DATA_HOME is set explicitly, we'll use that instead of $HOME/.local/share
		xdg_home = (std::string)nowide::getenv("XDG_DATA_HOME");
	else if (nowide::getenv("HOME") != nullptr)
		xdg_home = (std::string)nowide::getenv("HOME") + "/.local/share";
	if (!xdg_home.empty())
		// XDG data locations
		dirs.push_back(xdg_home + "/Deadcast/");

	if (nowide::getenv("XDG_DATA_DIRS") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("XDG_DATA_DIRS");
		addDirectoriesFromPath(dirs, path, "/Deadcast/");
	}
	else
	{
#ifdef FLYCAST_DATADIR
		const std::string data_dir(FLYCAST_DATADIR);
		dirs.push_back(data_dir);
#endif
		dirs.push_back("/usr/local/share/Deadcast/");
		dirs.push_back("/usr/share/Deadcast/");
	}
	if (nowide::getenv("DEADCAST_BIOS_PATH") != nullptr)
	{
		std::string path = (std::string)nowide::getenv("DEADCAST_BIOS_PATH");
		addDirectoriesFromPath(dirs, path, "/");
	}
	dirs.push_back("./");
	dirs.push_back("data/");

	return dirs;
}

static const char *selfPath;

void os_RunInstance(int argc, const char *argv[])
{
	if (fork() == 0)
	{
		std::vector<char *> localArgs;
		localArgs.push_back((char *)selfPath);
		for (int i = 0; i < argc; i++)
			localArgs.push_back((char *)argv[i]);
		localArgs.push_back(nullptr);
		execv(selfPath, &localArgs[0]);
	}
}

#if defined(USE_BREAKPAD)
static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	if (succeeded)
		registerCrash(descriptor.directory().c_str(), descriptor.path());

	return succeeded;
}
#endif

int main(int argc, char* argv[])
{
	selfPath = argv[0];
	// Deadcast: recomp modules marked for deletion deleted, an install cut short tidied away; then the
	// disc's recomp module hosted, if that is its chosen core (no emulator started)
	deadui::recomp::tidy();
	if (deadui::recomp::get("app.version") != DEADCAST_VERSION)
		deadui::recomp::set("app.version", DEADCAST_VERSION);
	if (argc > 1)
		deadui::recomp::runChosen(argv[1]);
#if defined(USE_BREAKPAD)
	google_breakpad::MinidumpDescriptor descriptor("/tmp");
	google_breakpad::ExceptionHandler eh(descriptor, nullptr, dumpCallback, nullptr, true, -1);
#endif

	LogManager::Init();
	i18n::init();

	// Set directories
	set_user_config_dir(find_user_config_dir());
	set_user_data_dir(find_user_data_dir());
	for (const auto& dir : find_system_config_dirs())
		add_system_config_dir(dir);
	for (const auto& dir : find_system_data_dirs())
		add_system_data_dir(dir);
	INFO_LOG(BOOT, "Config dir is: %s", get_writable_config_path("").c_str());
	INFO_LOG(BOOT, "Data dir is:   %s", get_writable_data_path("").c_str());

#if defined(USE_SDL)
	// init video now: on rpi3 it installs a sigsegv handler(?)
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		die("SDL: Initialization failed!");
	}
#endif

	common_linux_setup();

	if (flycast_init(argc, argv))
		die("Flycast initialization failed\n");

#if defined(USE_BREAKPAD)
	auto async = std::async(std::launch::async, uploadCrashes, "/tmp");
#endif

	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ERROR_LOG(BOOT, "mainui_loop error: %s", e.what());
	} catch (...) {
		ERROR_LOG(BOOT, "mainui_loop unknown exception");
	}

	flycast_term();
	os_UninstallFaultHandler();

	// Deadcast: the other core chosen in Core Settings: the launcher starts the game again in it (the
	// start page left up meanwhile, frontend/ui.cpp)
	return deadcast::menu::switching() ? deadui::recomp::SwitchExit : 0;
}

[[noreturn]] void os_DebugBreak()
{
	raise(SIGTRAP);
	std::abort();
}

#endif // __unix__
