#include "engine/LlamaEngine.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "engine/ModelContext.h"
#include "llama.h"

#include <cassert>
#include <vector>
#include <string>
#include <cstring>

// 把 token decode 到 llama_context（prefill/append 都走它）
static bool decode_tokens(llama_context *lctx,
                          std::vector<llama_token> &toks)
{
    if (!lctx || toks.empty())
        return true;

    llama_batch batch = llama_batch_get_one(
        toks.data(), // 非 const
        (int)toks.size());

    return llama_decode(lctx, batch) == 0;
}

// 将字符串 tokenize 成 llama_token
static bool tokenize_text(const llama_vocab *vocab, const std::string &text, std::vector<llama_token> &out)
{
    if (!vocab)
        return false;
    out.clear();
    out.resize(text.size() + 16);

    int n = llama_tokenize(
        vocab,
        text.c_str(),
        (int)text.size(),
        out.data(),
        (int)out.size(),
        true,
        true);

    if (n < 0)
        return false;
    out.resize(n);
    return true;
}

// 把“本轮增量 messages”转成 prompt，并以 assistant: 结尾
static std::string build_turn_prompt_with_assistant(
    const std::vector<Message> &msgs)
{
    std::string prompt;
    for (const auto &m : msgs)
    {
        prompt += m.role;
        prompt += ": ";
        prompt += m.content;
        prompt += "\n";
    }
    prompt += "assistant:";
    return prompt;
}

// token -> piece（detokenize）
static std::string token_to_piece(const llama_vocab *vocab, llama_token tok)
{
    std::string s;
    s.resize(64);

    int n = llama_token_to_piece(vocab, tok, s.data(), (int)s.size(), 0, false);
    if (n < 0)
        return "";

    if (n > (int)s.size())
    {
        s.resize(n);
        n = llama_token_to_piece(vocab, tok, s.data(), (int)s.size(), 0, false);
        if (n < 0)
            return "";
    }
    s.resize(n);
    return s;
}

// KV 续写 decode：pos 必须从 n_past 开始递增
static bool decode_tokens(llama_context *lctx,
                          const std::vector<llama_token> &toks,
                          int n_past)
{
    if (!lctx || toks.empty())
        return true;

    llama_batch batch = llama_batch_init((int)toks.size(), 0, 1);
    batch.n_tokens = (int)toks.size();

    for (int i = 0; i < (int)toks.size(); ++i)
    {
        batch.token[i] = toks[i];
        batch.pos[i] = n_past + i;

        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;

        // 只需要最后一个 token 的 logits 用于采样
        batch.logits[i] = (i == (int)toks.size() - 1);
    }

    const int rc = llama_decode(lctx, batch);
    llama_batch_free(batch);
    return rc == 0;
}


LlamaEngine::LlamaEngine(const std::string &model_path): model_path_(model_path)
{
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    model_ = llama_model_load_from_file(model_path.c_str(), mparams);
    assert(model_ && "failed to load llama model");
}

LlamaEngine::~LlamaEngine()
{
    if (model_)
        llama_model_free(model_);
    llama_backend_free();
}

std::shared_ptr<ModelContext> LlamaEngine::CreateNewContext()
{
    auto mc = std::make_shared<ModelContext>();

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 4096;
    cparams.n_threads = 4;
    cparams.n_threads_batch = 4;

    // llama_init_from_model
    mc->ctx = llama_init_from_model(model_, cparams);
    if (!mc->ctx)
        return nullptr;

    // sampler：先用 greedy（稳定）
    mc->sampler = llama_sampler_init_greedy();
    mc->n_past = 0;
    mc->initialized = true;
    return mc;
}

std::shared_ptr<ModelContext> LlamaEngine::EnsureContext(const std::shared_ptr<Session> &s)
{
    std::lock_guard<std::mutex> lk(s->mu);

    if (!s->model_ctx)
    {
        s->model_ctx = CreateNewContext();
        return s->model_ctx;
    }

    // 溢出保护：太接近 n_ctx 就重建
    const int n_ctx = llama_n_ctx(s->model_ctx->ctx);
    if (s->model_ctx->n_past > n_ctx - 256)
    {
        s->model_ctx.reset();
        s->model_ctx = CreateNewContext();
    }

    return s->model_ctx;
}

// ============================================================
// Session 级 KV Cache 的 Run（C-4）
// - 多轮：只 prefill 本轮增量 messages + "assistant:"
// - KV 通过 mc->n_past 续写
void LlamaEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx || !ctx->session)
    {
        if (ctx)
        {
            ctx->error_message = "LlamaEngine: ctx/session null";
            ctx->EmitFinish(FinishReason::error);
        }
        return;
    }

    auto mc = EnsureContext(ctx->session);
    if (!mc || !mc->ctx || !mc->sampler)
    {
        ctx->error_message = "LlamaEngine: failed to create session ModelContext";
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    const llama_vocab *vocab = llama_model_get_vocab(model_);
    if (!vocab)
    {
        ctx->error_message = "LlamaEngine: vocab null";
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    // 1) build delta prompt（你已有 auto-diff，ctx->messages 是 delta）
    std::string prompt;
    if (ctx->is_chat)
        prompt = build_turn_prompt_with_assistant(ctx->messages);
    else
        prompt = ctx->prompt;

    // 2) tokenize
    std::vector<llama_token> toks;
    if (!tokenize_text(vocab, prompt, toks))
    {
        ctx->error_message = "LlamaEngine: tokenize failed";
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    // 3) prefill/append -> KV（pos 从 mc->n_past 开始）
    if (!decode_tokens(mc->ctx, toks, mc->n_past))
    {
        ctx->error_message = "LlamaEngine: llama_decode failed (prefill)";
        ctx->EmitFinish(FinishReason::error);
        return;
    }
    mc->n_past += (int)toks.size();

    // 4) generate
    const int max_new_tokens = 512;
    for (int step = 0; step < max_new_tokens; ++step)
    {
        if (ctx->cancelled.load())
        {
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        // sample next token
        llama_token next = llama_sampler_sample(mc->sampler, mc->ctx, -1);

        // ✅ 你的 llama.h：accept 只有 (sampler, token)
        llama_sampler_accept(mc->sampler, next);

        // ✅ 新 API：llama_vocab_is_eog
        if (llama_vocab_is_eog(vocab, next))
        {
            ctx->EmitFinish(FinishReason::stop);
            return;
        }

        // append to KV
        std::vector<llama_token> one{next};
        if (!decode_tokens(mc->ctx, one, mc->n_past))
        {
            ctx->error_message = "LlamaEngine: llama_decode failed (decode)";
            ctx->EmitFinish(FinishReason::error);
            return;
        }
        mc->n_past += 1;

        // output
        std::string piece = token_to_piece(vocab, next);
        if (!piece.empty())
            ctx->EmitDelta(piece);
    }

    ctx->EmitFinish(FinishReason::length);
}
