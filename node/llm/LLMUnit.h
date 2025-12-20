#pragma once

#include <string>
#include <vector>
#include <functional>

#include "llama.h"

// 一个简单的 C++ LLM 单元：负责
//  - 加载 GGUF 模型
//  - 维护 llama_context（带 KV cache）
//  - 提供 Generate() 做多轮对话
class LLMUnit
{
public:
    struct Config
    {
        int n_ctx;          // 最大上下文长度
        int n_threads;      // 推理线程数
        int max_new_tokens; // 每次最多生成的 token 数
        bool verbose;       // 是否打印调试信息

        Config()
            : n_ctx(2048),
              n_threads(8),
              max_new_tokens(128),
              verbose(false) {}
    };

    using ChunkCallback = std::function<void(const std::string &)>;

    // 注意：这里不再带默认参数 = Config()
    explicit LLMUnit(const std::string &model_path,
                     const Config &cfg);

    ~LLMUnit();

    // 不允许拷贝
    LLMUnit(const LLMUnit &) = delete;
    LLMUnit &operator=(const LLMUnit &) = delete;

    // 简单单轮/多轮对话接口：
    //  - 你每次传入一个 user_prompt
    //  - 它会在当前上下文基础上继续生成
    std::string Generate(const std::string &user_prompt);

    // 流式生成，每生成一小段就回调给上层
    std::string GenerateStream(const std::string &user_prompt,
                               const ChunkCallback &on_chunk);

    // 可选：重置会话（清空 KV cache，相当于重新开始聊天）
    void Reset();

    int CountTokens(const std::string &text) const;

private:
    // 构建 Qwen 风格 Chat Prompt
    std::string BuildPrompt(const std::string &user_prompt) const;

    // 两步 tokenize，返回 llama_token 序列
    std::vector<llama_token> Tokenize(const std::string &text) const;

private:
    llama_model *model_ = nullptr;
    llama_context *ctx_ = nullptr;
    const llama_vocab *vocab_ = nullptr;

    Config cfg_;

    // 当前上下文中已经使用到的 position（下一次写入的位置）
    // 保证所有 pos 连续：Y = X + 1
    int cur_pos_ = 0;
};
