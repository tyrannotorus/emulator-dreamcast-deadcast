#include "ta_ctx.h"
#include "spg.h"
#include "cfg/option.h"
#include "Renderer_if.h"
#include "serialize.h"
#include "stdclass.h"

#include <mutex>
#include <vector>

extern u32 fskip;
static int RenderCount;

TA_context* ta_ctx;
tad_context ta_tad;

static void tactx_Recycle(TA_context* ctx);
static TA_context *tactx_Find(u32 addr, bool allocnew = false);

void SetCurrentTARC(u32 addr)
{
	if (addr != TACTX_NONE)
	{
		if (ta_ctx)
			SetCurrentTARC(TACTX_NONE);

		verify(ta_ctx == 0);
		//set new context
		ta_ctx = tactx_Find(addr,true);

		//copy cached params
		ta_tad = ta_ctx->tad;
	}
	else
	{
		//Flush cache to context
		verify(ta_ctx != 0);
		ta_ctx->tad=ta_tad;
		
		//clear context
		ta_ctx=0;
		ta_tad.Reset(0);
	}
}

static TA_context* rqueue;
static cResetEvent frame_finished;

bool QueueRender(TA_context* ctx)
{
	verify(ctx != 0);
	
	bool skipFrame = !rend_is_enabled();
	// Deadcast: auto frame skip (always on, option.h): frames are not rendered while emulation is behind real
	// time, up to AutoSkipFrame in a row, so the SH4 can catch up (renders to texture are never skipped)
	static int lagSkips;
	if (!skipFrame)
	{
		RenderCount++;
		if (RenderCount % (config::SkipFrame + 1) != 0)
			skipFrame = true;
		else if (!ctx->rend.isRTT && emulationLagMs > 8.f
				&& lagSkips < config::AutoSkipFrame
				// Deadcast: and not while the audio output has a third of its buffer or more still to play:
				// emulation is then ahead of real time, whatever the waits for the screen added to the lag
				&& audioFillPermille.load(std::memory_order_relaxed) < 333)
		{
			skipFrame = true;
			lagSkips++;
		}
		else if (config::ThreadedRendering && rqueue != nullptr)
			// The previous render hasn't completed yet so we wait.
			// Deadcast: whatever the frame skip: a frame is dropped only by the lag above (emulation more than
			// 8 ms behind real time, the waits for the renderer included). Dropped here, as upstream's Maximum
			// did whenever the renderer was still busy, NBA 2K2 lost 13 frames a second with the SH4 62% busy.
		{
			const auto waitStart = perfNow();
			perfTrace('q');
			frame_finished.Wait();
			perfTrace('Q');
			perfWaited(waitStart);
		}
	}

	if (!skipFrame && !rqueue && !ctx->rend.isRTT)
		lagSkips = 0;
	if (skipFrame || rqueue)
	{
		tactx_Recycle(ctx);
		if (rend_is_enabled())
			fskip++;
		return false;
	}
	// disable net rollbacks until the render thread has processed the frame
	rend_disable_rollback();
	frame_finished.Reset();
	verify(rqueue == nullptr);
	rqueue = ctx;


	return true;
}

TA_context* DequeueRender()
{
	if (rqueue != nullptr)
		FrameCount++;

	return rqueue;
}

// Deadcast: frees the queue slot while ctx is still being drawn (Renderer_if.cpp), so the next frame
// can be queued; RecycleRender() once drawn
void ReleaseRender(TA_context* ctx)
{
	verify(rqueue == ctx);
	rqueue = nullptr;
	frame_finished.Set();
}

void RecycleRender(TA_context* ctx)
{
	tactx_Recycle(ctx);
}

void FinishRender(TA_context* ctx)
{
	if (ctx != nullptr)
	{
		verify(rqueue == ctx);
		rqueue = nullptr;
		tactx_Recycle(ctx);
	}
	frame_finished.Set();
}

static std::mutex mtx_pool;
using Lock = std::lock_guard<std::mutex>;

static std::vector<TA_context*> ctx_pool;
static std::vector<TA_context*> ctx_list;

TA_context *tactx_Alloc()
{
	TA_context *ctx = nullptr;
	{
		Lock _(mtx_pool);
		if (!ctx_pool.empty()) {
			ctx = ctx_pool.back();
			ctx_pool.pop_back();
		}
	}

	if (ctx == nullptr) {
		ctx = new TA_context();
		ctx->Alloc();
	}
	return ctx;
}

static void tactx_Recycle(TA_context* ctx)
{
	if (ctx->nextContext != nullptr)
		tactx_Recycle(ctx->nextContext);
	Lock _(mtx_pool);
	if (ctx_pool.size() > 3) {
		delete ctx;
	}
	else {
		ctx->Reset();
		ctx_pool.push_back(ctx);
	}
}

static TA_context *tactx_Find(u32 addr, bool allocnew)
{
	TA_context *oldCtx = nullptr;
	for (TA_context *ctx : ctx_list)
	{
		if (ctx->Address == addr) {
			ctx->lastFrameUsed = FrameCount;
			return ctx;
		}
		if (FrameCount - ctx->lastFrameUsed > 60)
			oldCtx = ctx;
	}

	if (allocnew)
	{
		TA_context *ctx;
		if (oldCtx != nullptr)
		{
			ctx = oldCtx;
			ctx->Reset();
		}
		else
		{
			ctx = tactx_Alloc();
			ctx_list.push_back(ctx);
		}
		ctx->Address = addr;
		ctx->lastFrameUsed = FrameCount;

		return ctx;
	}
	return nullptr;
}

TA_context *tactx_Pop(u32 addr)
{
	for (size_t i = 0; i < ctx_list.size(); i++)
	{
		if (ctx_list[i]->Address == addr)
		{
			TA_context *ctx = ctx_list[i];
			
			if (::ta_ctx == ctx)
				SetCurrentTARC(TACTX_NONE);

			ctx_list.erase(ctx_list.begin() + i);

			return ctx;
		}
	}
	return nullptr;
}

void tactx_Term()
{
	if (ta_ctx != nullptr)
		SetCurrentTARC(TACTX_NONE);

	for (TA_context *ctx : ctx_list)
		delete ctx;
	ctx_list.clear();

	Lock _(mtx_pool);
	for (TA_context *ctx : ctx_pool)
		delete ctx;
	ctx_pool.clear();
}

const u32 NULL_CONTEXT = ~0u;

static void serializeContext(Serializer& ser, const TA_context *ctx)
{
	if (ser.dryrun())
	{
		// Maximum size: address, size, data
		ser.skip(4 + 4 + TA_DATA_SIZE);
		return;
	}
	if (ctx == nullptr)
	{
		ser << NULL_CONTEXT;
		return;
	}
	ser << ctx->Address;
	const tad_context& tad = ctx == ::ta_ctx ? ta_tad : ctx->tad;
	const u32 taSize = tad.thd_data - tad.thd_root;
	ser << taSize;
	ser.serialize(tad.thd_root, taSize);
}

static void deserializeContext(Deserializer& deser, TA_context **pctx)
{
	u32 address;
	deser >> address;
	if (address == NULL_CONTEXT)
	{
		*pctx = nullptr;
		return;
	}
	u32 size;
	deser >> size;
	if (size > TA_DATA_SIZE)
		throw Deserializer::Exception("Invalid TA context size");
	*pctx = tactx_Find(address, true);
	tad_context& tad = (*pctx)->tad;
	deser.deserialize(tad.thd_root, size);
	tad.thd_data = tad.thd_root + size;
	if (deser.version() < Deserializer::V26)
	{
		u32 render_pass_count;
		deser >> render_pass_count;
		deser.skip(sizeof(u32) * render_pass_count);
	}
}

void SerializeTAContext(Serializer& ser)
{
	ser << (u32)ctx_list.size();
	int curCtx = -1;
	for (const auto& ctx : ctx_list)
	{
		if (ctx == ::ta_ctx)
			curCtx = (int)(&ctx - &ctx_list[0]);
		serializeContext(ser, ctx);
	}
	ser << curCtx;
}

void DeserializeTAContext(Deserializer& deser)
{
	if (::ta_ctx != nullptr)
		SetCurrentTARC(TACTX_NONE);
	if (deser.version() >= Deserializer::V25)
	{
		u32 listSize;
		deser >> listSize;
		for (const auto& ctx : ctx_list)
			tactx_Recycle(ctx);
		ctx_list.clear();
		for (u32 i = 0; i < listSize; i++)
		{
			TA_context *ctx;
			deserializeContext(deser, &ctx);
		}
		int curCtx;
		deser >> curCtx;
		if (curCtx >= 0 && curCtx < (int)ctx_list.size())
			SetCurrentTARC(ctx_list[curCtx]->Address);
	}
	else
	{
		TA_context *ta_cur_ctx;
		deserializeContext(deser, &ta_cur_ctx);
		if (ta_cur_ctx != nullptr)
			SetCurrentTARC(ta_cur_ctx->Address);
		if (deser.version() >= Deserializer::V20)
			deserializeContext(deser, &ta_cur_ctx);
	}
}
