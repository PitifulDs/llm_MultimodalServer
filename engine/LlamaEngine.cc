#include "engine/LlamaEngine.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "engine/ModelContext.h"
#include "llama.h"

#include <cassert>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>

namespace
{
int get_env_int(const char *name, int def)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return def;
    try
    {
        int n = std::stoi(v);
        return n > 0 ? n : def;
    }
    catch (...)
    {
        return def;
    }
}
} // namespace

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
static bool tokenize_text(const llama_vocab *vocab, const std::string &text, std::vector<llama_token> &out, bool add_special)
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
        add_special,
        true);

    if (n < 0)
        return false;
    out.resize(n);
    return true;
}

// 使用 llama_chat_apply_template 生成“本轮增量 prompt”
// 逻辑参考 llama.cpp/examples/simple-chat：用历史长度裁剪出 delta
static bool build_chat_delta_prompt(
    const llama_model *model,
    const std::vector<Message> &history,
    const std::vector<Message> &incoming,
    std::string &out_prompt,
    std::string &err)
{
    const char *tmpl = llama_model_chat_template(model, nullptr);
    if (!tmpl)
        tmpl = "chatml";

    std::vector<llama_chat_message> full_msgs;
    full_msgs.reserve(history.size() + incoming.size());
    for (const auto &m : history)
    {
        full_msgs.push_back({m.role.c_str(), m.content.c_str()});
    }
    for (const auto &m : incoming)
    {
        full_msgs.push_back({m.role.c_str(), m.content.c_str()});
    }

    int32_t prev_len = 0;
    if (!history.empty())
    {
        std::vector<llama_chat_message> prev_msgs;
        prev_msgs.reserve(history.size());
        for (const auto &m : history)
        {
            prev_msgs.push_back({m.role.c_str(), m.content.c_str()});
        }
        prev_len = llama_chat_apply_template(tmpl, prev_msgs.data(), prev_msgs.size(), false, nullptr, 0);
        if (prev_len < 0)
        {
            err = "chat template apply failed (prev)";
            return false;
        }
    }

    int32_t new_len = llama_chat_apply_template(tmpl, full_msgs.data(), full_msgs.size(), true, nullptr, 0);
    if (new_len < 0)
    {
        err = "chat template apply failed (full)";
        return false;
    }

    std::string formatted;
    formatted.resize(new_len);
    int32_t res = llama_chat_apply_template(tmpl, full_msgs.data(), full_msgs.size(), true, formatted.data(), formatted.size());
    if (res < 0)
    {
        err = "chat template apply failed (format)";
        return false;
    }

    if (prev_len < 0)
        prev_len = 0;
    if (prev_len > res)
        prev_len = 0;

    out_prompt.assign(formatted.data() + prev_len, formatted.data() + res);
    return true;
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
    cparams.n_ctx = get_env_int("LLAMA_N_CTX", 4096);
    cparams.n_threads = get_env_int("LLAMA_N_THREADS", 4);
    cparams.n_threads_batch = get_env_int("LLAMA_N_THREADS_BATCH", 4);

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
    const int margin = get_env_int("KV_RESET_MARGIN", 256);
    if (s->model_ctx->n_past > n_ctx - margin)
    {
        s->model_ctx.reset();
        s->model_ctx = CreateNewContext();
    }

    return s->model_ctx;
}

// ============================================================
// Session 级 KV Cache 的 Run（C-4）
// - 多轮：只 prefill 本轮增量 messages + "assistant:"
// - KV 通过 mc->n_past 续写void LlamaEngine::Run(std::shared_ptr<ServingContext> ctx)
void LlamaEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    if (!ctx || !ctx->session)
    {
        if (ctx)
        {
            ctx->error_message = "LlamaEngine: ctx/session null";
            ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;
            ctx->EmitFinish(FinishReason::error);
        }
        return;
    }

    auto finalize_usage = [&]
    {
        ctx->usage.total_tokens = ctx->usage.prompt_tokens + ctx->usage.completion_tokens;
    };

    // 取消点：尽早退出
    if (ctx->cancelled.load(std::memory_order_acquire))
    {
        finalize_usage();
        ctx->EmitFinish(FinishReason::cancelled);
        return;
    }

    auto mc = EnsureContext(ctx->session);
    if (!mc || !mc->ctx || !mc->sampler)
    {
        ctx->error_message = "LlamaEngine: failed to create session ModelContext";
        finalize_usage();
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    const llama_vocab *vocab = llama_model_get_vocab(model_);
    if (!vocab)
    {
        ctx->error_message = "LlamaEngine: vocab null";
        finalize_usage();
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    // 1) build delta prompt
    std::string prompt;
    if (ctx->is_chat)
    {
        std::vector<Message> history_copy;
        {
            std::lock_guard<std::mutex> lk(ctx->session->mu);
            history_copy = ctx->session->history;
        }

        std::string err;
        if (!build_chat_delta_prompt(model_, history_copy, ctx->messages, prompt, err))
        {
            ctx->error_message = "LlamaEngine: " + err;
            finalize_usage();
            ctx->EmitFinish(FinishReason::error);
            return;
        }
    }
    else
    {
        prompt = ctx->prompt;
    }

    // 取消点：tokenize 之前
    if (ctx->cancelled.load(std::memory_order_acquire))
    {
        finalize_usage();
        ctx->EmitFinish(FinishReason::cancelled);
        return;
    }

    // 2) tokenize
    std::vector<llama_token> toks;
    const bool add_special = (mc->n_past == 0);
    if (!tokenize_text(vocab, prompt, toks, add_special))
    {
        ctx->error_message = "LlamaEngine: tokenize failed";
        finalize_usage();
        ctx->EmitFinish(FinishReason::error);
        return;
    }

    // prompt tokens（本次 delta prompt）
    ctx->usage.prompt_tokens += static_cast<int>(toks.size());

    // 取消点：prefill 之前
    if (ctx->cancelled.load(std::memory_order_acquire))
    {
        finalize_usage();
        ctx->EmitFinish(FinishReason::cancelled);
        return;
    }

    // 3) prefill/append -> KV
    if (!decode_tokens(mc->ctx, toks, mc->n_past))
    {
        ctx->error_message = "LlamaEngine: llama_decode failed (prefill)";
        finalize_usage();
        ctx->EmitFinish(FinishReason::error);
        return;
    }
    mc->n_past += (int)toks.size();

    // 取消点：prefill 之后
    if (ctx->cancelled.load(std::memory_order_acquire))
    {
        finalize_usage();
        ctx->EmitFinish(FinishReason::cancelled);
        return;
    }

    // 4) generate
    int max_new_tokens = 512;
    auto it = ctx->params.find("max_tokens");
    if (it != ctx->params.end())
    {
        try
        {
            max_new_tokens = std::stoi(it->second);
        }
        catch (...)
        {
            max_new_tokens = 512;
        }
    }
    else if (const char *env = std::getenv("DEFAULT_MAX_TOKENS"))
    {
        try
        {
            max_new_tokens = std::stoi(env);
        }
        catch (...)
        {
            max_new_tokens = 512;
        }
    }
    if (max_new_tokens <= 0)
        max_new_tokens = 1;

    LOG(INFO) << "[llama] req=" << ctx->request_id
              << " max_new_tokens=" << max_new_tokens;
    for (int step = 0; step < max_new_tokens; ++step)
    {
        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            finalize_usage();
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        llama_token next = llama_sampler_sample(mc->sampler, mc->ctx, -1);
        llama_sampler_accept(mc->sampler, next);

        if (llama_vocab_is_eog(vocab, next))
        {
            finalize_usage();
            ctx->EmitFinish(FinishReason::stop);
            return;
        }

        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            finalize_usage();
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        std::vector<llama_token> one{next};
        if (!decode_tokens(mc->ctx, one, mc->n_past))
        {
            ctx->error_message = "LlamaEngine: llama_decode failed (decode)";
            finalize_usage();
            ctx->EmitFinish(FinishReason::error);
            return;
        }
        mc->n_past += 1;

        // completion tokens（成功 decode 的 token）
        ctx->usage.completion_tokens += 1;

        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            finalize_usage();
            ctx->EmitFinish(FinishReason::cancelled);
            return;
        }

        std::string piece = token_to_piece(vocab, next);
        if (!piece.empty() && !ctx->cancelled.load(std::memory_order_acquire))
            ctx->EmitDelta(piece);
    }

    finalize_usage();
    ctx->EmitFinish(FinishReason::length);
}
