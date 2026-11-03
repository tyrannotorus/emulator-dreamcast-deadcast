#pragma once


namespace fc_profiler
{
	inline static void startThread(const std::string& threadName) {}
	inline static void endThread(float warningTime = 0.0) {}
}

#define FC_PROFILE_SCOPE
#define FC_PROFILE_SCOPE_NAMED(name)
