#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "json.hpp"

class LlamaEngine;

using task_callback_t = std::function<void(const std::string &data, bool finish)>;

class llm_task
{
public:
    std::string model_;
    std::string response_format_;
    std::string model_path_;
    std::string system_prompt_;
    std::vector<std::string> inputs_;
    task_callback_t out_callback_;
    bool enoutput_ = false;
    bool enstream_ = false;
    int max_token_len_ = 512;
    float temperature_ = 0.0f;
    float top_p_ = 1.0f;
    int top_k_ = 0;
    float repeat_penalty_ = 1.0f;
    float presence_penalty_ = 0.0f;
    float frequency_penalty_ = 0.0f;
    int repeat_last_n_ = 0;
    bool has_seed_ = false;
    uint32_t seed_ = 0;

    std::string work_id_;
    std::shared_ptr<LlamaEngine> engine_;
    std::mutex engine_mu_;
    std::string utf8_pending_;
    std::mutex req_mu_;
    std::string current_req_id_;
    std::string current_work_id_;
    std::atomic<uint64_t> req_seq_{0};
    std::atomic<bool> running_{false};

    static std::mutex s_engine_mu_;
    static std::mutex s_infer_mu_;
    static std::condition_variable s_infer_cv_;
    static int s_infer_active_;
    static std::weak_ptr<LlamaEngine> s_engine_;
    static std::string s_model_path_;
    static std::mutex s_stream_mu_;
    static std::unordered_map<std::string, std::unordered_map<int, std::string>> s_stream_buffs_;

    explicit llm_task(const std::string &workid);
    ~llm_task();

    void set_output(task_callback_t out_callback);
    int load_model(const nlohmann::json &config_body);
    void inference(const std::string &msg, const std::string &req_id, const std::string &work_id, uint64_t seq);
    void start();
    void stop();
    bool try_begin_infer();
    void end_infer();

private:
    bool parse_config(const nlohmann::json &config_body);
};
