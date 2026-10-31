// Deadcast in a recomp module (src/framework/deadcast.cpp): its start page until the game's first
// picture, and MENU opening Deadcast's menu over the game, held (../../frontend/ui.h), the same menu
// as Deadcast's own. A module has no menu of its own: its settings are Recomp Settings', declared in
// its manifest (frontend/recomp.h).
#pragma once

namespace dreamcomp {

// The start page up (until deadcast_started())
bool deadcast_starting();
// The game's first picture: the start page gives way
void deadcast_started();

}  // namespace dreamcomp
