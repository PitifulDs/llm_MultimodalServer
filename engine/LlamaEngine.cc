#include "engine/LlamaEngine.h"
#include "serving/core/ServingContext.h"

#include "llama.h"

#include <cassert>
#include <vector>
#include <string>
#include <cstring>

static std::string build_prompt(const ServingContext &ctx)
{
    std::string prompt;
    for (const auto &m : ctx.messages)
    {
        prompt += m.role + ": " + m.content + "\n";
    }
    prompt += "assistant:";
    return prompt;
}

LlamaEngine::LlamaEngine(const std::string &model_path)
    : model_path_(model_path)
{
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    model_ = llama_model_load_from_file(model_path.c_str(), mparams);

    assert(model_ && "failed to load llama model");

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    ctx_ = llama_init_from_model(model_, cparams);
    assert(ctx_ && "failed to create llama context");

    // sampler
    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    sampler_ = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());
}

LlamaEngine::~LlamaEngine()
{
    if (sampler_)
        llama_sampler_free(sampler_);
    if (ctx_)
        llama_free(ctx_);
    if (model_)
        llama_model_free(model_);
    llama_backend_free();
}

void LlamaEngine::Generate(ServingContext &ctx)
{
    GenerateStream(ctx);
}

void LlamaEngine::GenerateStream(ServingContext &sctx)
{
    const std::string prompt = build_prompt(sctx);

    // ===== vocab (NEW API 核心点) =====
    const llama_vocab *vocab = llama_model_get_vocab(model_);
    assert(vocab && "failed to get llama vocab");

    // ===== tokenize =====
    std::vector<llama_token> tokens(prompt.size() + 16);
    int n = llama_tokenize(
        vocab,
        prompt.c_str(),
        prompt.size(),
        tokens.data(),
        tokens.size(),
        true,
        true);

    assert(n >= 0);
    tokens.resize(n);

    // ===== prefill =====
    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
    llama_decode(ctx_, batch);

    // ===== generation loop =====
    while (!sctx.cancelled)
    {
        llama_token tok = llama_sampler_sample(sampler_, ctx_, -1);

        if (tok == llama_vocab_eos(vocab))
        {
            break;
        }

        llama_batch next = llama_batch_get_one(&tok, 1);
        llama_decode(ctx_, next);

        // token → text (NEW API)
        char buf[8 * 1024];
        int len = llama_token_to_piece(
            vocab,
            tok,
            buf,
            sizeof(buf),
            0,
            true);

        if (len > 0)
        {
            StreamChunk chunk;
            chunk.delta.assign(buf, len);
            chunk.is_finished = false;

            if (sctx.on_chunk)
            {
                sctx.on_chunk(chunk);
            }
        }

        tokens.push_back(tok);
    }

    // ===== finish chunk (Step 4.2 规范) =====
    StreamChunk end;
    end.delta = "";
    end.is_finished = true;
    end.finish_reason = FinishReason::stop;

    if (sctx.on_chunk)
    {
        sctx.on_chunk(end);
    }
}
