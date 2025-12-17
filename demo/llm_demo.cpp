#include <iostream>
#include "../node/llm/LLMUnit.h"

int main()
{
    // 1. 配置参数（用默认构造，然后按需修改）
    LLMUnit::Config cfg;
    cfg.n_ctx = 2048;         // 不改也行，默认就是 2048
    cfg.n_threads = 8;        // 不改也行
    cfg.max_new_tokens = 128; // 每次最多生成 128 个 token
    cfg.verbose = false;

    // 2. 模型路径（按你的实际路径改）
    const std::string model_path =
        "/home/dongsong/workspace/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf";

    // 3. 创建 LLMUnit（注意：这里要传 2 个参数）
    LLMUnit llm(model_path, cfg);

    std::cout << "Qwen C++ Chat, 输入内容，输入 exit / reset\n";

    while (true)
    {
        std::cout << "\nYou: ";
        std::string line;
        if (!std::getline(std::cin, line))
        {
            break;
        }

        if (line == "exit")
        {
            break;
        }
        if (line == "reset")
        {
            llm.Reset();
            std::cout << "[Session reset]\n";
            continue;
        }

        std::cout << "Qwen: ";
        std::cout.flush();
        try
        {
            // ⭐ 流式输出：在回调里边生成边打印
            std::string full_reply = llm.GenerateStream(
                line,
                [](const std::string &chunk)
                {
                    std::cout << chunk;
                    std::cout.flush();
                });

            // 最后打一行换行，让下一轮输入好看一点
            std::cout << std::endl;

            // 你也可以把 full_reply 写日志 / 存档：
            // std::cerr << "[FULL REPLY] " << full_reply << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "\nLLM error: " << e.what() << std::endl;
        }
        
        // std::string reply;
        // try
        // {
        //     reply = llm.Generate(line);
        // }
        // catch (const std::exception &e)
        // {
        //     std::cerr << "LLM error: " << e.what() << std::endl;
        //     continue;
        // }

        // std::cout << "Qwen: " << reply << std::endl;
    }

    return 0;
}
