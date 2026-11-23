// Deadcast's settings, as Options shows them: deadui::options::pages() (ui/src/options.h), each row made from the
// config::Option it changes, so its section, key and default are the option's own.
#pragma once
#include "options.h"
#include "cfg/option.h"

namespace deadcast::rows
{

// the option behind a row of pages(): flag for an on/off row, number for the others
struct Bound
{
	config::Option<bool> *flag = nullptr;
	config::Option<int> *number = nullptr;
};
// dies on a row pages() did not make
Bound option(const deadui::options::Setting& setting);

}
