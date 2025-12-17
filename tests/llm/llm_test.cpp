#include <iostream>
#include <vector>
#include <string>
#include <thread>

#include "llama.h"

int main()
{
    const std::string model_path =
        "/home/dongsong/workspace/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf";

    // ===== Init backend =====
    llama_backend_init();

    // ===== Load model =====
    llama_model_params mparams = llama_model_default_params();
    llama_model *model = llama_load_model_from_file(model_path.c_str(), mparams);
    if (!model)
    {
        std::cerr << "Failed to load model\n";
        return 1;
    }

    // ===== Create context =====
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_threads = std::thread::hardware_concurrency();

    llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx)
    {
        std::cerr << "Failed to create context\n";
        return 1;
    }

    const llama_vocab *vocab = llama_model_get_vocab(model);

    // ===== Prompt =====
    std::string prompt = "你是一个中文智能助手，请用一句话介绍你自己。";

    // ===== Tokenize（单次，大 buffer）=====
    // 这里直接给一个比较大的上限，防止溢出
    std::vector<llama_token> prompt_tokens(1024);

    int n_prompt_tokens = llama_tokenize(
        vocab,
        prompt.c_str(),
        (int32_t)prompt.size(),
        prompt_tokens.data(),
        (int32_t)prompt_tokens.size(),
        true, // add_special
        false // parse_special
    );

    if (n_prompt_tokens < 0)
    {
        std::cerr << "Tokenize failed, code = " << n_prompt_tokens << "\n";
        return 1;
    }
    if (n_prompt_tokens == 0)
    {
        std::cerr << "Tokenize got 0 tokens, something is wrong.\n";
        return 1;
    }

    prompt_tokens.resize(n_prompt_tokens);

    // ===== Decode prompt（手动构造 batch）=====
    {
        llama_batch batch = llama_batch_init(n_prompt_tokens, 0, 1);

        for (int i = 0; i < n_prompt_tokens; ++i)
        {
            batch.token[i] = prompt_tokens[i];
            batch.pos[i] = i;
            batch.seq_id[i][0] = 0;
            batch.n_seq_id[i] = 1;
            // 只让最后一个 token 产生 logits
            batch.logits[i] = (i == n_prompt_tokens - 1);
        }
        batch.n_tokens = n_prompt_tokens;

        if (llama_decode(ctx, batch) != 0)
        {
            std::cerr << "Prompt decode failed\n";
            llama_batch_free(batch);
            return 1;
        }

        llama_batch_free(batch);
    }

    // ===== Sampler =====
    llama_sampler *sampler = llama_sampler_init_greedy();

    std::cout << "Qwen 输出: ";

    int cur_pos = n_prompt_tokens;

    // ===== 生成循环 =====
    for (int i = 0; i < 200; ++i)
    {
        // 从最新 logits 中采样下一个 token
        llama_token new_token = llama_sampler_sample(sampler, ctx, -1);

        if (new_token == llama_vocab_eos(vocab))
        {
            break;
        }

        // 转回字符串输出
        char buf[4096];
        int n = llama_token_to_piece(
            vocab,
            new_token,
            buf,
            (int32_t)sizeof(buf),
            0,
            true);
        if (n > 0)
        {
            std::cout.write(buf, n);
            std::cout.flush();
        }

        // ===== 手动构造只包含一个 token 的 batch =====
        llama_batch batch = llama_batch_init(1, 0, 1);

        batch.token[0] = new_token;
        batch.pos[0] = cur_pos;
        batch.seq_id[0][0] = 0;
        batch.n_seq_id[0] = 1;
        batch.logits[0] = true; // 这个 token 需要输出 logits
        batch.n_tokens = 1;

        if (llama_decode(ctx, batch) != 0)
        {
            std::cerr << "\nDecode failed at step " << i << "\n";
            llama_batch_free(batch);
            break;
        }

        llama_batch_free(batch);
        cur_pos++;
    }

    std::cout << std::endl;

    // ===== Cleanup =====
    llama_sampler_free(sampler);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();

    return 0;
}
