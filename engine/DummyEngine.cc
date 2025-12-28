#include "engine/DummyEngine.h"
#include "serving/ServingContext.h"

#include <sstream>
#include <thread>
#include <chrono>

void DummyEngine::run(std::shared_ptr<ServingContext> ctx)
{
    if (ctx->stream)
    {
        StreamChunk c1;
        c1.delta = "Hello";
        ctx->on_chunk(c1);

        StreamChunk c2;
        c2.is_finished = true;
        c2.finish_reason = FinishReason::stop;
        ctx->on_chunk(c2);
    }
    else
    {
        ctx->final_text = "Hello";
        ctx->finish_reason = FinishReason::stop;
    }
}

// void DummyEngine::RunStream(const ServingContext &ctx,
//                             const std::function<void(const std::string &)> &on_delta,
//                             const std::function<void()> &on_done)
// {
//     const std::string text = Run(ctx);

//     // 简单按空格拆分 token 模拟流式输出
//     std::istringstream iss(text);
//     std::string token;
//     bool first = true;
//     while (iss >> token)
//     {
//         if (!first)
//         {
//             on_delta(" ");
//         }
//         on_delta(token);
//         first = false;
//         std::this_thread::sleep_for(std::chrono::milliseconds(100));
//     }

//     on_done();
// }
