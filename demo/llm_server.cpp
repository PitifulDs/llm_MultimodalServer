#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>

#include "../node/llm/LLMUnit.h"
#include "../utils/json.hpp" // nlohmann::json

// 在另一个窗口使用 nc 127.0.0.1 9000
static const int kListenPort = 9000;

// 最大 token 上限
constexpr int MAX_CONTEXT_TOKENS = 1800; // < n_ctx

// 只保留最近 N 轮对话,每一轮 = user + assistant
constexpr size_t MAX_TURNS = 8;
constexpr size_t MAX_MESSAGES = MAX_TURNS * 2;

// System prompt（你可以改成更强的指令）
static const std::string kSystemPrompt =
    "<|system|>\n"
    "你是一个有帮助、准确、简洁的中文智能助手。\n";

struct Message
{
    std::string role; // "user" | "assistant"
    std::string content;
};

struct Session
{
    std::unique_ptr<LLMUnit> llm;
    std::vector<Message> history;
};


std::unordered_map<std::string, Session> g_sessions; // session_id -> Session(会话状态)
std::mutex g_sessions_mtx;

using json = nlohmann::json;

namespace
{

    constexpr size_t STREAM_FLUSH_CHARS = 16;

    // 判断是否需要 flush buffer
    bool should_flush(const std::string &buf)
    {
        if (buf.size() >= STREAM_FLUSH_CHARS)
            return true;

        if (!buf.empty())
        {
            char c = buf.back();
            if (c == '\n' || c == '.' || c == '!' || c == '?' ||
                c == '。' || c == '！' || c == '？')
            {
                return true;
            }
        }
        return false;
    }

} // anonymous namespace

int count_prompt_tokens(LLMUnit &llm, const std::string &prompt)
{
    return llm.CountTokens(prompt);
}

void trim_history_by_tokens(
    Session &session,
    LLMUnit &llm,
    const std::string &system_prompt)
{
    while (true)
    {
        // 1. 拼完整 prompt
        std::string full_prompt = system_prompt;

        for (const auto &msg : session.history)
        {
            if (msg.role == "user")
            {
                full_prompt += "<|user|>\n" + msg.content + "\n";
            }
            else if (msg.role == "assistant")
            {
                full_prompt += "<|assistant|>\n" + msg.content + "\n";
            }
        }
        full_prompt += "<|assistant|>\n";

        // 2. 计算 token 数
        int tokens = llm.CountTokens(full_prompt);

        // 3. 如果安全，结束
        if (tokens <= MAX_CONTEXT_TOKENS)
        {
            break;
        }

        // 4. 否则：删除最老的一轮（user + assistant）
        if (session.history.size() >= 2)
        {
            session.history.erase(session.history.begin(),
                                  session.history.begin() + 2);
        }
        else
        {
            // 极端情况，防死循环
            session.history.clear();
            break;
        }
    }
}


Session &get_or_create_session(const std::string& session_id , const std::string& model_path, const LLMUnit::Config &cfg)
{
    std::lock_guard<std::mutex> lk(g_sessions_mtx);

    auto it = g_sessions.find(session_id);
    if(it != g_sessions.end()){
        return it->second;
    }

    Session s;
    s.llm = std::make_unique<LLMUnit>(model_path, cfg);

    auto res = g_sessions.emplace(session_id, std::move(s));
    return res.first->second;
}




// 发送完整字符串（不加换行）
bool send_all(int fd, const std::string &data)
{
    const char *buf = data.data();
    size_t len = data.size();
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = ::send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// 发送一行 JSON（自动加 '\n'）
bool send_json_line(int fd, const json &j)
{
    std::string s = j.dump(
        -1,                                      // indent
        ' ',                                     // indent char
        false,                                   // ensure_ascii
        nlohmann::json::error_handler_t::replace // 关键
    );
    s.push_back('\n');
    return send_all(fd, s);
}

// 从 socket 读一行（以 '\n' 结束）
bool recv_line(int fd, std::string &out)
{
    out.clear();
    char ch;
    while (true)
    {
        ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0)
        {
            return false; // 断开或出错
        }
        if (ch == '\n')
        {
            break;
        }
        out.push_back(ch);
    }
    // 去掉可能的 '\r'
    if (!out.empty() && out.back() == '\r')
    {
        out.pop_back();
    }
    return true;
}

void handle_client(int client_fd, const std::string &model_path, const LLMUnit::Config &cfg)
{
    // 欢迎信息，用纯文本提示一下协议
    {
        std::string welcome =
            "Qwen C++ TCP JSON Chat.\n"
            "每一行发送一个 JSON 请求，例如：\n"
            R"({"type":"chat","prompt":"介绍一下C++","stream":true})"
            "\n"
            R"({"type":"reset"})"
            "\n"
            R"({"type":"exit"})"
            "\n\n";
        send_all(client_fd, welcome);
    }

    std::string line;

    while (true)
    {
        if (!recv_line(client_fd, line))
        {
            break; // 客户端断开
        }

        // 跳过空行
        if (line.empty())
        {
            continue;
        }

        json req;
        try
        {
            req = json::parse(line);
        }
        catch (const std::exception &e)
        {
            json err = {
                {"type", "error"},
                {"message", std::string("invalid json: ") + e.what()},
                {"raw", line},
            };
            send_json_line(client_fd, err);
            continue;
        }

        std::string type = req.value("type", std::string("chat"));
        std::string session_id = req.value("session_id", std::string("default"));

        // 支持两种退出方式：
        // 1) {"type":"exit"}
        // 2) 直接断开连接
        if (type == "exit")
        {
            json bye = {
                {"type", "bye"},
                {"session_id", session_id},
            };
            send_json_line(client_fd, bye);
            break;
        }

        // 重置会话：
        // {"type":"reset"} 或 {"reset":true}
        bool reset_flag = req.value("reset", false);
        if (type == "reset" || reset_flag)
        {
            // llm.Reset();
            // 只重置当前 session
            {
                std::lock_guard<std::mutex> lock(g_sessions_mtx);
                g_sessions.erase(session_id);
            }

            json resp = {
                {"type", "reset"},
                {"session_id", session_id},
                {"message", "session reset"},
            };
            send_json_line(client_fd, resp);
            continue;
        }

        // 聊天请求：{"type":"chat","prompt":"...","stream":true/false}
        if (type == "chat")
        {
            // 按 session_id 获取或创建 session
            Session &session = get_or_create_session(session_id, model_path, cfg);

            // 当前请求使用该 session 的 llm
            LLMUnit &llm = *session.llm;

            if (!req.contains("prompt"))
            {
                json err = {
                    {"type", "error"},
                    {"message", "missing 'prompt' field"},
                };
                send_json_line(client_fd, err);
                continue;
            }

            std::string prompt = req["prompt"].get<std::string>();
            bool stream = req.value("stream", false);

            // 记录用户输入
            session.history.push_back({"user", prompt});
            std::cerr << "[HISTORY SIZE][" << session_id << "] "
                      << session.history.size() << std::endl;

            trim_history_by_tokens(session, llm, kSystemPrompt);
            std::string full_prompt = kSystemPrompt;

            for (const auto &msg : session.history)
            {
                if (msg.role == "user")
                {
                    full_prompt += "<|user|>\n" + msg.content + "\n";
                }
                else if (msg.role == "assistant")
                {
                    full_prompt += "<|assistant|>\n" + msg.content + "\n";
                }
            }

            // 最后补 assistant，让模型继续生成
            full_prompt += "<|assistant|>\n";


            // 目前 top_k / temperature 是在 LLMUnit 内部写死的
            // 这里预留字段，后续可以接 Config 或 per-request 参数
            // int   top_k       = req.value("top_k", 20);
            // float temperature = req.value("temperature", 0.8f);

            if (!stream)
            {
                // 非流式：直接返回完整 reply
                try
                {
                    std::string reply = llm.Generate(full_prompt);
                    // 把 assistant 回复加入 history
                    session.history.push_back({"assistant", reply});
                    // 如果 history 太长，裁剪最前面的
                    if (session.history.size() > MAX_MESSAGES)
                    {
                        session.history.erase(session.history.begin(), session.history.begin() + (session.history.size() - MAX_MESSAGES));
                    }

                    json resp = {
                        {"type", "response"},
                        {"session_id", session_id},
                        {"reply", reply},
                        {"finish_reason", "stop"},
                    };
                    send_json_line(client_fd, resp);
                }
                catch (const std::exception &e)
                {
                    json err = {
                        {"type", "error"},
                        {"message", std::string("llm error: ") + e.what()},
                    };
                    send_json_line(client_fd, err);
                }
            }
            else
            {
                // 流式：先发一个 start
                {
                    json start = {
                        {"type", "start"},
                        {"session_id", session_id},
                    };
                    send_json_line(client_fd, start);
                }

                try
                {
                    std::string stream_buffer;
                    // 流式生成：每个 chunk 发一条 JSON
                    std::string full_reply = llm.GenerateStream(
                        full_prompt,
                        [&](const std::string &chunk)
                        {
                            if (chunk.empty())
                                return;

                            // 1. 先累积到 buffer
                            stream_buffer += chunk;

                            // 2. 判断是否需要 flush
                            if (should_flush(stream_buffer))
                            {
                                json jchunk = {
                                    {"type", "chunk"},
                                    {"session_id", session_id},
                                    {"delta", stream_buffer},
                                };
                                send_json_line(client_fd, jchunk);
                                stream_buffer.clear();
                            }
                        });

                    // 流式生成结束后，把 assistant 回复写回 history
                    session.history.push_back({"assistant", full_reply});

                    // message 数兜底裁剪（可保留）
                    if (session.history.size() > MAX_MESSAGES)
                    {
                        session.history.erase(
                            session.history.begin(),
                            session.history.begin() + (session.history.size() - MAX_MESSAGES));
                    }
                    
                    // flush 剩余 buffer
                    if (!stream_buffer.empty())
                    {
                        json jchunk = {
                            {"type", "chunk"},
                            {"session_id", session_id},
                            {"delta", stream_buffer},
                        };
                        send_json_line(client_fd, jchunk);
                        stream_buffer.clear();
                    }

                    // 结束标记
                    json end = {
                        {"type", "end"},
                        {"session_id", session_id},
                        {"finish_reason", "stop"},
                    };
                    send_json_line(client_fd, end);

                    // 你也可以在这里把 full_reply 打日志
                    std::cerr << "[FULL REPLY][" << session_id << "] "
                              << full_reply << std::endl;
                }
                catch (const std::exception &e)
                {
                    json err = {
                        {"type", "error"},
                        {"message", std::string("llm error: ") + e.what()},
                    };
                    send_json_line(client_fd, err);
                }
            }

            continue;
        }

        // 未知 type
        json err = {
            {"type", "error"},
            {"message", std::string("unknown request type: ") + type},
        };
        send_json_line(client_fd, err);
    }

    ::close(client_fd);
    std::cerr << "Client disconnected\n";
}

int main()
{
    // 1. 初始化 LLMUnit
    LLMUnit::Config cfg;
    cfg.n_ctx = 2048;
    cfg.n_threads = 8;
    cfg.max_new_tokens = 128;
    cfg.verbose = false;

    const std::string model_path =
        "/home/dongsong/workspace/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf";

    LLMUnit llm(model_path, cfg);

    // 2. 创建监听 socket
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(kListenPort);

    if (::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        ::close(listen_fd);
        return 1;
    }

    if (::listen(listen_fd, 16) < 0)
    {
        perror("listen");
        ::close(listen_fd);
        return 1;
    }

    std::cout << "LLM TCP JSON server listening on port " << kListenPort << " ...\n";

    // 3. 接收客户端连接
    while (true)
    {
        sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = ::accept(listen_fd, (sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        std::cout << "New client connected\n";

        // 简单起见：每个连接一个线程（后续可以改线程池 / Reactor）
        std::thread t(handle_client, client_fd, std::cref(model_path), std::cref(cfg));
        t.detach();
    }

    ::close(listen_fd);
    return 0;
}





// #include <arpa/inet.h>
// #include <netinet/in.h>
// #include <sys/socket.h>
// #include <unistd.h>

// #include <cstring>
// #include <iostream>
// #include <string>
// #include <thread>

// #include "../node/llm/LLMUnit.h"

// static const int kListenPort = 9000;

// // 简单工具：从 socket 读一行（以 '\n' 结束）
// bool recv_line(int fd, std::string &out)
// {
//     out.clear();
//     char ch;
//     while (true)
//     {
//         ssize_t n = ::recv(fd, &ch, 1, 0);
//         if (n <= 0)
//         {
//             return false; // 断开或出错
//         }
//         if (ch == '\n')
//         {
//             break;
//         }
//         out.push_back(ch);
//     }
//     // 去掉可能的 '\r'
//     if (!out.empty() && out.back() == '\r')
//     {
//         out.pop_back();
//     }
//     return true;
// }

// void handle_client(int client_fd, LLMUnit &llm)
// {
//     std::string line;

//     const char *welcome =
//         "Qwen C++ TCP Chat, 发送一行文本，发送 exit 关闭连接，发送 reset 重置会话。\n";
//     ::send(client_fd, welcome, std::strlen(welcome), 0);

//     while (true)
//     {
//         const char *prompt = "\nYou: ";
//         ::send(client_fd, prompt, std::strlen(prompt), 0);

//         if (!recv_line(client_fd, line))
//         {
//             break; // 客户端断开
//         }

//         if (line == "exit")
//         {
//             break;
//         }
//         if (line == "reset")
//         {
//             llm.Reset();
//             const char *msg = "[Session reset]\n";
//             ::send(client_fd, msg, std::strlen(msg), 0);
//             continue;
//         }

//         const char *prefix = "\nQwen: ";
//         ::send(client_fd, prefix, std::strlen(prefix), 0);

//         try
//         {
//             // 流式生成：每生成一小段就写到 socket
//             std::string full_reply = llm.GenerateStream(
//                 line,
//                 [&](const std::string &chunk)
//                 {
//                     ::send(client_fd, chunk.data(), chunk.size(), 0);
//                 });

//             const char *end_flag = "\n<<END>>\n";
//             ::send(client_fd, end_flag, std::strlen(end_flag), 0);

//             // 你也可以把 full_reply 打日志
//             std::cerr << "[FULL REPLY] " << full_reply << std::endl;
//         }
//         catch (const std::exception &e)
//         {
//             std::string err = std::string("\n[LLM ERROR] ") + e.what() + "\n";
//             ::send(client_fd, err.data(), err.size(), 0);
//         }
//     }

//     ::close(client_fd);
//     std::cerr << "Client disconnected\n";
// }

// int main()
// {
//     // 1. 初始化 LLMUnit（和 demo 里的几乎一样）
//     LLMUnit::Config cfg;
//     cfg.n_ctx = 2048;
//     cfg.n_threads = 8;
//     cfg.max_new_tokens = 128;
//     cfg.verbose = false;

//     const std::string model_path =
//         "/home/dongsong/workspace/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf";

//     LLMUnit llm(model_path, cfg);

//     // 2. 创建监听得socket
//     int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
//     if(listen_fd < 0){
//         perror("socket");
//         return 1;
//     }

//     int opt = 1;
//     ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
//     sockaddr_in addr{};
//     addr.sin_family = AF_INET;
//     addr.sin_addr.s_addr = INADDR_ANY;
//     addr.sin_port = htons(kListenPort);

//     if (::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
//     {
//         perror("bind");
//         ::close(listen_fd);
//         return 1;
//     }

//     if (::listen(listen_fd, 16) < 0)
//     {
//         perror("listen");
//         ::close(listen_fd);
//         return 1;
//     }

//     std::cout << "LLM TCP server listening on port " << kListenPort << " ...\n";
 
//     // 3. 主循环：接受客户端连接，每个连接用一个线程处理
//     while(true){
//         sockaddr_in client_addr{};
//         socklen_t client_addr_len = sizeof(client_addr);
//         int client_fd = ::accept(listen_fd, (sockaddr *)&client_addr, &client_addr_len);
//         if(client_fd < 0){
//             perror("accept");
//             continue;
//         }
        
//         std::cout << "New client connected from " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << "\n";

//         // 简单起见：每个连接开一个线程
//         std::thread t(handle_client, client_fd, std::ref(llm));
//         t.detach();
//     }

//     ::close(listen_fd);
//     return 0;
// }







