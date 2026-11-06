#pragma once
#include "types.h"
#include <functional>

// Deadcast: the copies the GPU needs before a frame is submitted (textures decoded into their staging
// memory, the frame's vertices and indices) made on a thread of their own while the renderer records the
// frame, instead of on the renderer's thread. Jobs run in order, one at a time. The renderer joins them
// before what it gives the GPU is used (the end of Drawer::Draw, a command pool's submit).
namespace deadcast::upload
{
void start();
void stop();
bool active();
// run on the worker if active, else now
void run(std::function<void()> job);
// every job run
void join();
// joins so far: a job queued while epoch() was E has run once epoch() is not E
u64 epoch();
}
