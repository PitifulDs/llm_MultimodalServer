#include "LLMUnit.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <random>
#include <algorithm>

// 简单 top-k + 温度采样
static llama_token sample_top_k(
    const float *logits,
    int32_t n_vocab,
    int top_k,
    float temperature)
{
    // 1) 复制一份 logits，避免修改原始指针
    std::vector<std::pair<float, int>> logit_id;
    logit_id.reserve(n_vocab);
    for (int i = 0; i < n_vocab; ++i)
    {
        logit_id.push_back({logits[i], i});
    }

    // 2) 按 logit 排序，取前 top_k
    if (top_k > n_vocab)
        top_k = n_vocab;
    std::partial_sort(
        logit_id.begin(),
        logit_id.begin() + top_k,
        logit_id.end(),
        [](const auto &a, const auto &b)
        {
            return a.first > b.first; // 从大到小
        });

    // 3) 对前 top_k 做 softmax( logit / temperature )
    std::vector<float> probs(top_k);
    float sum = 0.0f;
    for (int i = 0; i < top_k; ++i)
    {
        float v = logit_id[i].first / std::max(temperature, 1e-4f);
        float ev = std::exp(v);
        probs[i] = ev;
        sum += ev;
    }
    for (int i = 0; i < top_k; ++i)
    {
        probs[i] /= sum;
    }

    // 4) 按概率随机采样一个
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    float r = uni(rng);
    float cumsum = 0.0f;
    for (int i = 0; i < top_k; ++i)
    {
        cumsum += probs[i];
        if (r <= cumsum)
        {
            return (llama_token)logit_id[i].second;
        }
    }

    // 理论上不会走到这里，兜底用 top-1
    return (llama_token)logit_id[0].second;
}

LLMUnit::LLMUnit(const std::string &model_path,
                 const Config &cfg)
    : cfg_(cfg)
{

    // 初始化后端（多次调用是安全的）
    llama_backend_init();

    // 1. 加载模型
    llama_model_params mparams = llama_model_default_params();
    model_ = llama_load_model_from_file(model_path.c_str(), mparams);
    if (!model_)
    {
        throw std::runtime_error("Failed to load model");
    }

    // 2. 创建上下文（内含 KV cache）
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = cfg_.n_ctx;
    cparams.n_threads =
        cfg_.n_threads > 0 ? cfg_.n_threads
                           : (int)std::thread::hardware_concurrency();

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_)
    {
        llama_model_free(model_);
        model_ = nullptr;
        throw std::runtime_error("Failed to create context");
    }

    // 3. 拿词表
    vocab_ = llama_model_get_vocab(model_);
    if (!vocab_)
    {
        llama_free(ctx_);
        llama_model_free(model_);
        ctx_ = nullptr;
        model_ = nullptr;
        throw std::runtime_error("Failed to get vocab");
    }

    cur_pos_ = 0;
}

LLMUnit::~LLMUnit()
{
    if (ctx_)
    {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_)
    {
        llama_model_free(model_);
        model_ = nullptr;
    }
    // 简单起见，直接释放后端
    llama_backend_free();
}

std::string LLMUnit::BuildPrompt(const std::string &user_prompt) const
{
    // 简单的 Qwen Chat 模板，后面你可以再丰富
    std::string prompt;
    prompt += "<|system|>\n";
    prompt += "你是一个有帮助、准确、简洁的中文智能助手。\n";
    prompt += "<|user|>\n";
    prompt += user_prompt;
    prompt += "\n<|assistant|>\n";
    return prompt;
}

std::vector<llama_token> LLMUnit::Tokenize(const std::string &text) const
{
    // Step 1: probe 需要多少 token
    int n_tokens = llama_tokenize(
        vocab_,
        text.c_str(),
        (int32_t)text.size(),
        nullptr,
        0,
        true, // add_special
        false // parse_special
    );

    if (n_tokens < 0)
    {
        n_tokens = -n_tokens;
    }
    if (n_tokens == 0)
    {
        throw std::runtime_error("Tokenize got 0 tokens");
    }

    // Step 2: 真正 tokenize
    std::vector<llama_token> tokens(n_tokens);

    int n = llama_tokenize(
        vocab_,
        text.c_str(),
        (int32_t)text.size(),
        tokens.data(),
        (int32_t)tokens.size(),
        true,
        false);

    if (n < 0)
    {
        throw std::runtime_error("Failed to tokenize");
    }

    tokens.resize(n);
    return tokens;
}

void LLMUnit::Generate(ServingContext *ctx)
{
    bool reached_limit = false;

    while (!ctx->cancelled && !ctx->finished)
    {
        auto piece = DecodeOneToken();
        if (piece.empty())
            break;

        ctx->EmitDelta(piece);
    }

    if (ctx->finished)
    {
        return;
    }

    if (ctx->cancelled)
    {
        ctx->EmitFinish(FinishReason::cancelled);
    } 
    else if(reached_limit)
    {
        ctx->EmitFinish(FinishReason::length);
    }
    else
    {
        ctx->EmitFinish(FinishReason::stop);
    }
}

// std::string LLMUnit::Generate(const std::string &user_prompt)
// {
//     // 1. 构建真正喂给模型的 prompt（带 system/user/assistant）
//     std::string prompt = BuildPrompt(user_prompt);

//     // 2. 分词
//     std::vector<llama_token> tokens = Tokenize(prompt);
//     if (tokens.empty())
//     {
//         throw std::runtime_error("Tokenize returned empty token list");
//     }

//     // 3. 先把整个 prompt decode 一遍，构建 KV cache
//     {
//         llama_batch batch = llama_batch_init((int32_t)tokens.size(), 0, 1);

//         for (int i = 0; i < (int)tokens.size(); ++i)
//         {
//             batch.token[i] = tokens[i];  // token id
//             batch.pos[i] = cur_pos_ + i; // 从全局 cur_pos_ 开始排
//             batch.seq_id[i][0] = 0;      // 单条对话：seq 0
//             batch.n_seq_id[i] = 1;
//             batch.logits[i] = (i == (int)tokens.size() - 1); // 只最后一个要 logits
//         }
//         batch.n_tokens = (int32_t)tokens.size();

//         if (llama_decode(ctx_, batch) != 0)
//         {
//             llama_batch_free(batch);
//             throw std::runtime_error("Prompt decode failed");
//         }

//         llama_batch_free(batch);

//         // prompt 用完，更新全局 position
//         cur_pos_ += (int)tokens.size();
//     }

//     std::string output;

//     // 4. 多轮自回归生成：每次从 logits 中选一个 token（手写 greedy）
//     for (int step = 0; step < cfg_.max_new_tokens; ++step)
//     {
//         // 4.1 拿到最后一个 token 的 logits
//         float *logits = llama_get_logits_ith(ctx_, -1);
//         if (!logits)
//         {
//             std::cerr << "llama_get_logits_ith returned nullptr\n";
//             break;
//         }

//         int32_t n_vocab = llama_vocab_n_tokens(vocab_);
//         if (n_vocab <= 0)
//         {
//             std::cerr << "Invalid vocab size: " << n_vocab << "\n";
//             break;
//         }

//         // 4.2 使用 top-k + 温度采样
//         int top_k = 20;           // 你可以调，比如 20、40
//         float temperature = 0.8f; // 稍微有点随机，但不会太跑偏

//         llama_token best_token = sample_top_k(
//             logits,
//             n_vocab,
//             top_k,
//             temperature);

//         // 遇到 EOS，停止生成
//         if (best_token == llama_vocab_eos(vocab_))
//         {
//             break;
//         }

//         // 4.3 token → 文本，追加到输出
//         {
//             char buf[4096];
//             int n = llama_token_to_piece(
//                 vocab_,
//                 best_token,
//                 buf,
//                 (int32_t)sizeof(buf),
//                 0,   // flags
//                 true // special tokens 也转成文本
//             );
//             if (n > 0)
//             {
//                 output.append(buf, n);
//             }
//         }

//         // 4.4 把刚才生成的 token 再喂回模型，更新 KV cache，产生新的 logits
//         {
//             llama_batch batch = llama_batch_init(1, 0, 1);

//             batch.token[0] = best_token;
//             batch.pos[0] = cur_pos_++; // 使用并递增全局 cur_pos_
//             batch.seq_id[0][0] = 0;
//             batch.n_seq_id[0] = 1;
//             batch.logits[0] = true; // 这个 token 需要输出 logits
//             batch.n_tokens = 1;

//             if (llama_decode(ctx_, batch) != 0)
//             {
//                 llama_batch_free(batch);
//                 std::cerr << "Decode failed at step " << step << "\n";
//                 break;
//             }

//             llama_batch_free(batch);
//         }
//     }

//     // 5. 收尾清洗：把模板标记 / Human 之类裁掉
//     {
//         size_t cut_pos = output.size();

//         auto cut_if_found = [&](const std::string &marker)
//         {
//             size_t p = output.find(marker);
//             if (p != std::string::npos && p < cut_pos)
//             {
//                 cut_pos = p;
//             }
//         };

//         cut_if_found("<|system|>");
//         cut_if_found("<|user|>");
//         cut_if_found("<|assistant|>");
//         cut_if_found("<|endoftext|>");
//         cut_if_found("Human:");

//         if (cut_pos < output.size())
//         {
//             output.resize(cut_pos);
//         }
//     }

//     return output;
// }

// std::string LLMUnit::GenerateStream(const std::string &user_prompt,
//                                     const ChunkCallback &on_chunk)
// {
//     // 1. 构建真正喂给模型的 prompt（带 system/user/assistant）
//     std::string prompt = BuildPrompt(user_prompt);

//     // 2. 分词
//     std::vector<llama_token> tokens = Tokenize(prompt);
//     if (tokens.empty())
//     {
//         throw std::runtime_error("Tokenize returned empty token list");
//     }

//     // 3. 先把整个 prompt decode 一遍，构建 KV cache
//     {
//         llama_batch batch = llama_batch_init((int32_t)tokens.size(), 0, 1);

//         for (int i = 0; i < (int)tokens.size(); ++i)
//         {
//             batch.token[i] = tokens[i];
//             batch.pos[i] = cur_pos_ + i; // 从全局 cur_pos_ 开始排
//             batch.seq_id[i][0] = 0;      // 单条对话：seq 0
//             batch.n_seq_id[i] = 1;
//             batch.logits[i] = (i == (int)tokens.size() - 1); // 只最后一个要 logits
//         }
//         batch.n_tokens = (int32_t)tokens.size();

//         if (llama_decode(ctx_, batch) != 0)
//         {
//             llama_batch_free(batch);
//             throw std::runtime_error("Prompt decode failed");
//         }

//         llama_batch_free(batch);

//         // prompt 用完，更新全局 position
//         cur_pos_ += (int)tokens.size();
//     }

//     std::string output;

//     // 4. 多轮自回归生成：每次从 logits 中选一个 token（top-k 采样）
//     for (int step = 0; step < cfg_.max_new_tokens; ++step)
//     {
//         float *logits = llama_get_logits_ith(ctx_, -1);
//         if (!logits)
//         {
//             std::cerr << "llama_get_logits_ith returned nullptr\n";
//             break;
//         }

//         int32_t n_vocab = llama_vocab_n_tokens(vocab_);
//         if (n_vocab <= 0)
//         {
//             std::cerr << "Invalid vocab size: " << n_vocab << "\n";
//             break;
//         }

//         int top_k = 20;
//         float temperature = 0.8f;

//         llama_token token = sample_top_k(
//             logits,
//             n_vocab,
//             top_k,
//             temperature);

//         if (token == llama_vocab_eos(vocab_))
//         {
//             break;
//         }

//         // token → 文本
//         char buf[4096];
//         int n = llama_token_to_piece(
//             vocab_,
//             token,
//             buf,
//             (int32_t)sizeof(buf),
//             0,
//             true);

//         std::string piece;
//         if (n > 0)
//         {
//             piece.assign(buf, n);
//             output.append(piece);
//         }

//         // ⭐ 立刻把这一小段通过回调抛给上层（流式）
//         if (!piece.empty() && on_chunk)
//         {
//             on_chunk(piece);
//         }

//         // 把刚才生成的 token 再喂回模型
//         {
//             llama_batch batch = llama_batch_init(1, 0, 1);

//             batch.token[0] = token;
//             batch.pos[0] = cur_pos_++;
//             batch.seq_id[0][0] = 0;
//             batch.n_seq_id[0] = 1;
//             batch.logits[0] = true;
//             batch.n_tokens = 1;

//             if (llama_decode(ctx_, batch) != 0)
//             {
//                 llama_batch_free(batch);
//                 std::cerr << "Decode failed at step " << step << "\n";
//                 break;
//             }

//             llama_batch_free(batch);
//         }

//         // 提前发现模板标记可以提前停（避免刷太多垃圾）
//         if (output.find("<|system|>") != std::string::npos ||
//             output.find("<|user|>") != std::string::npos)
//         {
//             break;
//         }
//     }

//     // 5. 收尾清洗：把模板标记 / Human 之类裁掉（返回值用）
//     {
//         size_t cut_pos = output.size();

//         auto cut_if_found = [&](const std::string &marker)
//         {
//             size_t p = output.find(marker);
//             if (p != std::string::npos && p < cut_pos)
//             {
//                 cut_pos = p;
//             }
//         };

//         cut_if_found("<|system|>");
//         cut_if_found("<|user|>");
//         cut_if_found("<|assistant|>");
//         cut_if_found("<|endoftext|>");
//         cut_if_found("Human:");

//         if (cut_pos < output.size())
//         {
//             output.resize(cut_pos);
//         }
//     }

//     return output;
// }


void LLMUnit::Reset()
{
    if (ctx_)
    {
        llama_free(ctx_);
        ctx_ = nullptr;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = cfg_.n_ctx;
    cparams.n_threads =
        cfg_.n_threads > 0 ? cfg_.n_threads
                           : (int)std::thread::hardware_concurrency();

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_)
    {
        throw std::runtime_error("Failed to reset context");
    }

    cur_pos_ = 0;
}

int LLMUnit::CountTokens(const std::string &text) const
{
    int n = llama_tokenize(
        vocab_,
        text.c_str(),
        text.size(),
        nullptr,
        0,
        true,
        false);

    if (n < 0)
    {
        n = -n;
    }
    return n;
}

