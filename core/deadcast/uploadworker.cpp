#include "uploadworker.h"
#include "oslib/oslib.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace deadcast::upload
{
namespace
{
std::thread thread;
std::mutex mutex;
std::condition_variable wake;		// for the worker: a job, or stop
std::condition_variable idle;		// for join: every job run
std::deque<std::function<void()>> jobs;
std::atomic<u32> pending { 0 };		// queued and not yet run
bool stopping;
u64 joins;

void loop()
{
	ThreadName _("deadcast-upload");
	std::unique_lock<std::mutex> lock(mutex);
	while (true)
	{
		wake.wait(lock, [] { return !jobs.empty() || stopping; });
		if (jobs.empty())
			break;
		std::function<void()> job = std::move(jobs.front());
		jobs.pop_front();
		lock.unlock();
		job();
		lock.lock();
		if (--pending == 0)
			idle.notify_all();
	}
}
}

void start()
{
	if (thread.joinable())
		return;
	stopping = false;
	thread = std::thread(loop);
}

void stop()
{
	if (!thread.joinable())
		return;
	join();
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
	}
	wake.notify_one();
	thread.join();
}

bool active()
{
	return thread.joinable();
}

void run(std::function<void()> job)
{
	if (!active())
	{
		job();
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		jobs.push_back(std::move(job));
		pending++;
	}
	wake.notify_one();
}

void join()
{
	if (pending.load(std::memory_order_acquire) != 0)
	{
		std::unique_lock<std::mutex> lock(mutex);
		idle.wait(lock, [] { return pending.load() == 0; });
	}
	joins++;
}

u64 epoch()
{
	return joins;
}
}
