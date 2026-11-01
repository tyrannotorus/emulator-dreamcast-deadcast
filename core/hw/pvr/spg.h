#pragma once
#include <atomic>
#include "ta_ctx.h"

extern bool SH4FastEnough;
extern float emulationLagMs;
extern u32 sh4HeldBack;
// Deadcast: real time the SH4 thread waited for something other than its own work (a netplay session's
// other Brick), since the last vblank: not lag
extern float sh4WaitedMs;

bool spg_Init();
void spg_Term();
void spg_Reset(bool Manual);
void spg_Serialize(Serializer& ser);
void spg_Deserialize(Deserializer& deser);

void CalculateSync();
void read_lightgun_position(int x, int y);
void scheduleRenderDone(TA_context *cntx);
void rescheduleSPG();
// Deadcast: the host presented a frame (see perfVblank in spg.cpp)
void perfPresented();
// Deadcast: a render done (renderer thread): how long it took, its draw calls and vertices
void perfRendered(unsigned long long start, unsigned draws, unsigned vertices, bool toTexture);
extern unsigned perfDrawCalls;	// the renderer counts its draw calls here
// a texture decoded and uploaded again (size: its bytes in VRAM)
void perfTextureUpdated(u32 size);
// the audio output's callbacks that found too few samples and played silence (audiobackend_sdl2.cpp)
extern std::atomic<u32> audioUnderruns;
// Deadcast perf: the audio frames the output took in (push) and played (its callback), and how full it is (per mille)
extern std::atomic<u32> audioFramesIn, audioFramesOut, audioFillPermille;
// Deadcast: the emulation thread waited for the renderer, from start (perfNow()) until now
unsigned long long perfNow();
void perfWaited(unsigned long long start);
// Deadcast perf: whether DEADCAST_PERF is on
bool perfOn();
// a frame on the screen, its pan done (the display's vblank: vulkan/panel.cpp)
void perfShown();
// the AICA thread's: its CPU clock, for its share of a core in the perf line
void perfAicaThread();
// DEADCAST_TRACE (with DEADCAST_PERF): an event of a frame's way through the threads, with its time; written
// to <DEADCAST_SHOTS or /tmp>/trace.txt by perfTraceDump() as the game ends (local/brick/trace.py)
void perfTrace(char what);
void perfTraceDump();
// the GPU's time over a frame's work (vulkan/gputimer.h): kind 0 the renderer's submission (scene, renders
// to texture, texture uploads), 1 the frame drawn into the screen's page
void perfGpu(int kind, unsigned long long us);
// the render thread blocked from start until now: kind 0 on the renderer's frame in flight (the GPU), 1 on
// a page of the screen (shown, or its copy not done), 2 on the parse thread (the frame's polygons)
void perfRenderBlocked(int kind, unsigned long long start);
// a render's first part done (polygons to vertices, Renderer::Process), from start
void perfProcessed(unsigned long long start);
// a vkQueueSubmit's time, from start: kind 0 the renderer's frame, 1 the page's copy
void perfSubmitted(int kind, unsigned long long start);
