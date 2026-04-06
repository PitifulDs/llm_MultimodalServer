#include "serving/backend/LegacyChatEngineFacade.h"

#include "serving/core/ModelEngine.h"
#include "serving/core/ServingContext.h"

LegacyChatEngineFacade::LegacyChatEngineFacade(std::shared_ptr<ModelEngine> engine)
    : engine_(std::move(engine))
{
}

bool LegacyChatEngineFacade::Supports(ModelCapability capability) const
{
    return engine_ && capability == ModelCapability::Chat;
}

void LegacyChatEngineFacade::RunChat(const ChatRequest &request, ChatResponse &response) const
{
    response.model = request.model;

    if (!engine_)
    {
        response.error_message = "legacy chat engine unavailable";
        response.finish_reason = FinishReason::error;
        return;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = request.request_id;
    ctx->model = request.model;
    ctx->inference_backend = request.inference_backend;
    ctx->capability = request.capability;
    ctx->is_chat = true;
    ctx->stream = request.stream;
    ctx->messages = request.messages;
    ctx->params = request.params;
    ctx->on_chunk = request.on_chunk;
    ctx->on_finish = request.on_finish;

    engine_->Run(ctx);

    if (!ctx->finished.load(std::memory_order_acquire))
        ctx->EmitFinish(FinishReason::stop);

    response.output_text = ctx->final_text;
    response.finish_reason = ctx->finish_reason;
    response.error_message = ctx->error_message;
    response.usage = ctx->usage;
}
