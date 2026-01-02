#include "engine/DummyEngine.h"
#include "serving/core/ServingContext.h"


void DummyEngine::Generate(ServingContext& ctx)
{
    // 非流式，直接给出结果
    ctx.final_text = text_;
    ctx.finish_reason = FinishReason::stop;
}

void DummyEngine::GenerateStream(ServingContext& ctx)
{
    // 流式
    // 内容帧
    StreamChunk chunk;
    chunk.delta = text_;
    chunk.is_finished = false;

    if(ctx.on_chunk)
    {
        ctx.on_chunk(chunk);
    }

    // 结束帧
    StreamChunk end;
    end.delta = "";
    end.is_finished = true;
    end.finish_reason = FinishReason::stop;

    if(ctx.on_chunk)
    {
        ctx.on_chunk(end);
    }
}




