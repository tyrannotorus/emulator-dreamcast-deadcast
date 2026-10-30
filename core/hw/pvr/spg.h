#pragma once
#include <atomic>
#include "ta_ctx.h"

extern bool SH4FastEnough;
extern float emulationLagMs;
extern u32 sh4HeldBack;

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
// Deadcast: the emulation thread waited for the renderer, from start (perfNow()) until now
unsigned long long perfNow();
void perfWaited(unsigned long long start);
