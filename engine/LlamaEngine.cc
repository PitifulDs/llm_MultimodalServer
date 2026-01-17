#include "engine/LlamaEngine.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
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

// ============================================================
// Session 级 KV Cache 的 Run
// - 每个 session 第一次：创建 session->model_ctx（llama_context + sampler）并 prefill
// - 多轮：仅 prefill 本轮新增 messages + "assistant:"
// - decode 输出 token -> EmitDelta，并把最终 assistant 写入 session->history
void LlamaEngine::Run(std::shared_ptr<ServingContext> ctx)
{
    static std::atomic<int> in_flight{0};
    int v = ++in_flight;
    LOG(INFO) << "[LlamaEngine::Run] in_flight=" << v << " req=" << ctx->request_id;
    --in_flight;

    if (!ctx)
    {
        return;
    }

    if (!ctx->session)
    {
        ctx->error_message = "missing session in ServingContext";
        ctx->EmitFinish(FinishReason::error);
        ctx->finished.store(true);
        return;
    }

    auto session = ctx->session;
    session->touch();

    // ===== vocab =====
    const llama_vocab *vocab = llama_model_get_vocab(model_);
    if (!vocab)
    {
        ctx->error_message = "failed to get llama vocab";
        ctx->EmitFinish(FinishReason::error);
        ctx->finished.store(true);
        return;
    }

    // 确保 session->model_ctx 存在（KV Cache 容器）
    if (!session->model_ctx)
    {
        session->model_ctx = std::make_shared<ModelContext>();
    }

    auto mctx = session->model_ctx;

    // 首次为该 session 创建 llama_context / sampler（KV Cache 独享）
    if (!mctx->ctx)
    {
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 2048; // 你原来的值
        mctx->ctx = llama_init_from_model(model_, cparams);
        if (!mctx->ctx)
        {
            ctx->error_message = "failed to create llama context (session)";
            ctx->EmitFinish(FinishReason::error);
            ctx->finished.store(true);
            return;
        }
    }
    if (!mctx->sampler)
    {
        llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
        mctx->sampler = llama_sampler_chain_init(sparams);
        llama_sampler_chain_add(mctx->sampler, llama_sampler_init_greedy());
    }

    // 决定本轮 prompt（首轮/多轮不同）
    // 首轮：prefill 本轮 messages（通常包含 user/system）
    // 多轮：只 prefill 本轮新增 messages（不再重复历史）
    const bool first_turn = !mctx->initialized;
    ctx->is_first_turn = first_turn;

    // ⚠️ 这里假设 HttpGateway 传进来的 ctx->messages 是“本轮新增”
    // 如果你目前还传全历史，那 Step 4.6-B 的收益会被抵消（但语义仍正确）
    std::string turn_prompt = build_turn_prompt_with_assistant(ctx->messages);

    // ===== tokenize turn prompt =====
    std::vector<llama_token> turn_tokens;
    if (!tokenize_text(vocab, turn_prompt, turn_tokens))
    {
        ctx->error_message = "llama_tokenize failed";
        ctx->EmitFinish(FinishReason::error);
        ctx->finished.store(true);
        return;
    }

    // prefill/append 本轮 tokens 到 session KV Cache
    // 首轮：这一步等价于传统 prefill
    // 多轮：这一步等价于“append user turn + assistant: 提示”
    if (!decode_tokens(mctx->ctx, turn_tokens))
    {
        ctx->error_message = "llama_decode(turn) failed";
        ctx->EmitFinish(FinishReason::error);
        ctx->finished.store(true);
        return;
    }

    mctx->initialized = true;

    // ===== generation loop =====
    std::string generated; // 用于写回 session->history

    while (!ctx->cancelled.load())
    {
        llama_token tok = llama_sampler_sample(mctx->sampler, mctx->ctx, -1);

        if (tok == llama_vocab_eos(vocab))
        {
            break;
        }

        llama_batch next = llama_batch_get_one(&tok, 1);
        if (llama_decode(mctx->ctx, next) != 0)
        {
            ctx->error_message = "llama_decode(next) failed";
            ctx->EmitFinish(FinishReason::error);
            ctx->finished.store(true);
            return;
        }

        // token → text
        char buf[8 * 1024];
        int len = llama_token_to_piece(vocab, tok, buf, sizeof(buf), 0, true);
        if (len > 0)
        {
            std::string piece(buf, len);
            generated += piece;
            ctx->EmitDelta(piece);
        }
    }

    if (ctx->cancelled.load())
    {
        ctx->EmitFinish(FinishReason::cancelled);
        ctx->finished.store(true);
        return;
    }

    ctx->EmitFinish(FinishReason::stop);
    ctx->finished.store(true);

    // 写回 session->history（实现“语义多轮”+ 为后续对齐做准备）
    // 注意：这里把本轮输入 + 输出都追加进 history
    for (const auto &m : ctx->messages)
    {
        session->history.push_back(m);
    }
    session->history.push_back(Message{"assistant", generated});
}