// Deadcast: the SH4 kept to real time by the clock, frame by frame. At each emulated vblank it sleeps until
// the moment its emulated time stands for (5 ns a cycle), instead of being held back only by the audio
// output, which lets it go in the audio device's 23 ms steps and made the frames come in bursts. The audio
// output's buffer keeps the two clocks together: fuller than half, a little slower; emptier, a little
// faster (at most 0.5%; the clocks differ by parts per million). The audio output still holds the SH4
// back should it ever get that far ahead. DEADCAST_PACER=1: on (off by default: not yet shown to help).
#pragma once

namespace deadcast::pacer
{

// on the SH4 thread, at each emulated vblank
void vblank();

}
