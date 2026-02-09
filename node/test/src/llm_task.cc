#include "llm_task.h"

#include "engine/LlamaEngine.h"
#include "serving/core/ServingContext.h"
#include "serving/core/Session.h"
#include "glog/logging.h"

#include <cstdlib>
#include <cstring>

using json = nlohmann::json;

static std::string get_env_string(const char *name, const char *def_val = "")
{
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(def_val);
}

static int get_env_int(const char *name, int def_val)
{
    const char *v = std::getenv(name);
    if (!v || !*v)
        return def_val;
    try
    {
        int n = std::stoi(v);
        return n > 0 ? n : def_val;
    }
    catch (...)
    {
        return def_val;
    }
}

static const char *kDefaultModelPath =
    "models/"
    "qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_0.gguf";

static std::string gen_request_id()
{
    static std::atomic<uint64_t> seq{0};
    return "req-" + std::to_string(++seq);
}

static size_t utf8_valid_prefix_len(const std::string &s)
{
    size_t i = 0;
    while (i < s.size())
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c <= 0x7F)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            break;

        if (i + len > s.size())
            break;

        for (size_t j = 1; j < len; ++j)
        {
            unsigned char cc = static_cast<unsigned char>(s[i + j]);
            if ((cc & 0xC0) != 0x80)
                return i;
        }
        i += len;
    }
    return i;
}

static std::string utf8_sanitize(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c <= 0x7F)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
        {
            out.push_back('?');
            ++i;
            continue;
        }

        if (i + len > s.size())
            break;

        bool ok = true;
        for (size_t j = 1; j < len; ++j)
        {
            unsigned char cc = static_cast<unsigned char>(s[i + j]);
            if ((cc & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
        }
        if (!ok)
        {
            out.push_back('?');
            ++i;
            continue;
        }
        out.append(s, i, len);
        i += len;
    }
    return out;
}

llm_task::llm_task(const std::string &workid)
{
    work_id_ = workid;
}

llm_task::~llm_task()
{
    stop();
}

void llm_task::set_output(task_callback_t out_callback)
{
    out_callback_ = out_callback;
}

bool llm_task::try_begin_infer()
{
    bool expected = false;
    return running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}

void llm_task::end_infer()
{
    running_.store(false, std::memory_order_release);
}

bool llm_task::parse_config(const nlohmann::json &config_body)
{
    try
    {
        system_prompt_.clear();
        model_ = config_body.at("model");
        response_format_ = config_body.at("response_format");
        enoutput_ = config_body.at("enoutput");
        if (config_body.contains("prompt") && config_body["prompt"].is_string())
        {
            system_prompt_ = config_body["prompt"].get<std::string>();
        }
        if (config_body.contains("input"))
        {
            if (config_body["input"].is_string())
            {
                inputs_.push_back(config_body["input"].get<std::string>());
            }
            else if (config_body["input"].is_array())
            {
                for (auto _in : config_body["input"])
                {
                    inputs_.push_back(_in.get<std::string>());
                }
            }
        }
        if (config_body.contains("temperature") && config_body["temperature"].is_number())
            temperature_ = config_body["temperature"].get<float>();
        if (config_body.contains("top_p") && config_body["top_p"].is_number())
            top_p_ = config_body["top_p"].get<float>();
        if (config_body.contains("top_k") && config_body["top_k"].is_number_integer())
            top_k_ = config_body["top_k"].get<int>();
        if (config_body.contains("repeat_penalty") && config_body["repeat_penalty"].is_number())
            repeat_penalty_ = config_body["repeat_penalty"].get<float>();
        if (config_body.contains("repetition_penalty") && config_body["repetition_penalty"].is_number())
            repeat_penalty_ = config_body["repetition_penalty"].get<float>();
        if (config_body.contains("presence_penalty") && config_body["presence_penalty"].is_number())
            presence_penalty_ = config_body["presence_penalty"].get<float>();
        if (config_body.contains("frequency_penalty") && config_body["frequency_penalty"].is_number())
            frequency_penalty_ = config_body["frequency_penalty"].get<float>();
        if (config_body.contains("repeat_last_n") && config_body["repeat_last_n"].is_number_integer())
            repeat_last_n_ = config_body["repeat_last_n"].get<int>();
        if (config_body.contains("seed") && config_body["seed"].is_number_integer())
        {
            seed_ = static_cast<uint32_t>(config_body["seed"].get<int>());
            has_seed_ = true;
        }
    }
    catch (...)
    {
        return true;
    }
    enstream_ = (response_format_.find("stream") != std::string::npos);
    return false;
}

int llm_task::load_model(const nlohmann::json &config_body)
{
    if (parse_config(config_body))
    {
        return -1;
    }
    if (config_body.contains("max_token_len") && config_body["max_token_len"].is_number_integer())
    {
        const int v = config_body["max_token_len"].get<int>();
        if (v > 0)
            max_token_len_ = v;
    }

    if (config_body.contains("model_path") && config_body["model_path"].is_string())
    {
        model_path_ = config_body["model_path"].get<std::string>();
    }
    if (model_path_.empty())
    {
        model_path_ = get_env_string("STACKFLOW_MODEL_PATH");
    }
    if (model_path_.empty())
    {
        model_path_ = get_env_string("LLM_MODEL_PATH");
    }
    if (model_path_.empty())
    {
        model_path_ = get_env_string("MODEL_PATH");
    }
    if (model_path_.empty())
    {
        model_path_ = kDefaultModelPath;
    }
    if (model_path_.empty())
    {
        LOG(ERROR) << "[llm_task] model_path missing";
        return -1;
    }

    {
        std::lock_guard<std::mutex> lk(s_engine_mu_);
        auto cached = s_engine_.lock();
        bool created = false;
        if (!cached || s_model_path_ != model_path_)
        {
            LOG(INFO) << "[llm_task] loading model path=" << model_path_;
            cached = std::make_shared<LlamaEngine>(model_path_);
            s_engine_ = cached;
            s_model_path_ = model_path_;
            created = true;
        }
        LOG(INFO) << "[llm_task] setup done cached=" << (created ? "new" : "reuse");
        if (created)
        {
            LOG(INFO) << "[llm_task] model ready";
        }
        {
            std::lock_guard<std::mutex> lk_local(engine_mu_);
            engine_ = cached;
        }
    }
    return 0;
}

void llm_task::inference(const std::string &msg, const std::string &req_id, const std::string &work_id, uint64_t seq)
{
    if (!out_callback_) {
        return;
    }

    struct LocalEndGuard
    {
        llm_task *self;
        ~LocalEndGuard()
        {
            if (self)
                self->end_infer();
        }
    } end_guard{this};

    std::shared_ptr<LlamaEngine> engine;
    {
        std::lock_guard<std::mutex> lk(engine_mu_);
        engine = engine_;
    }
    if (!engine)
    {
        out_callback_(std::string(""), true);
        return;
    }

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = gen_request_id();
    ctx->session_id = ctx->request_id;
    ctx->model = model_;
    ctx->stream = enstream_;
    ctx->is_chat = true;
    ctx->messages.clear();
    bool used_payload_messages = false;
    if (!msg.empty() && msg.front() == '{')
    {
        try
        {
            auto j = nlohmann::json::parse(msg);
            if (j.contains("messages") && j["messages"].is_array())
            {
                bool has_system = false;
                for (const auto &m : j["messages"])
                {
                    if (!m.is_object())
                        continue;
                    const std::string role = m.value("role", "");
                    const std::string content = m.value("content", "");
                    if (role.empty())
                        continue;
                    if (role == "system" && !content.empty())
                        has_system = true;
                    ctx->messages.push_back({role, content});
                }
                if (!ctx->messages.empty())
                {
                    used_payload_messages = true;
                    if (!has_system && !system_prompt_.empty())
                    {
                        ctx->messages.insert(ctx->messages.begin(), {"system", system_prompt_});
                    }
                }
            }
        }
        catch (...)
        {
        }
    }
    if (!used_payload_messages)
    {
        if (!system_prompt_.empty())
        {
            ctx->messages.push_back({"system", system_prompt_});
        }
        ctx->messages.push_back({"user", msg});
    }
    ctx->params["max_tokens"] = std::to_string(max_token_len_);
    ctx->params["temperature"] = std::to_string(temperature_);
    ctx->params["top_p"] = std::to_string(top_p_);
    ctx->params["top_k"] = std::to_string(top_k_);
    ctx->params["repeat_penalty"] = std::to_string(repeat_penalty_);
    ctx->params["presence_penalty"] = std::to_string(presence_penalty_);
    ctx->params["frequency_penalty"] = std::to_string(frequency_penalty_);
    if (repeat_last_n_ != 0)
        ctx->params["repeat_last_n"] = std::to_string(repeat_last_n_);
    if (has_seed_)
        ctx->params["seed"] = std::to_string(seed_);
    ctx->session = std::make_shared<Session>(ctx->session_id, ctx->model);
    LOG(INFO) << "[llm_task] inference start stream=" << (enstream_ ? 1 : 0);

    {
        std::lock_guard<std::mutex> lk(req_mu_);
        current_req_id_ = req_id;
        current_work_id_ = work_id;
        utf8_pending_.clear();
    }

    struct GlobalInferGuard
    {
        bool active{false};
        GlobalInferGuard()
        {
            std::unique_lock<std::mutex> lk(s_infer_mu_);
            const int limit = get_env_int("STACKFLOW_MAX_CONCURRENCY", 2);
            s_infer_cv_.wait(lk, [&] { return s_infer_active_ < limit; });
            ++s_infer_active_;
            active = true;
        }
        ~GlobalInferGuard()
        {
            if (!active)
                return;
            std::lock_guard<std::mutex> lk(s_infer_mu_);
            if (s_infer_active_ > 0)
                --s_infer_active_;
            s_infer_cv_.notify_all();
        }
    } global_guard;

    if (enstream_)
    {
        ctx->on_chunk = [this, seq](const StreamChunk &c)
        {
            if (!out_callback_)
                return;
            if (seq != req_seq_.load(std::memory_order_acquire))
                return;
            if (c.is_finished)
            {
                if (!utf8_pending_.empty())
                {
                    std::string flush = utf8_sanitize(utf8_pending_);
                    utf8_pending_.clear();
                    if (!flush.empty())
                        out_callback_(flush, false);
                }
                out_callback_(std::string(""), true);
            }
            else if (!c.delta.empty())
            {
                utf8_pending_.append(c.delta);
                const size_t n = utf8_valid_prefix_len(utf8_pending_);
                if (n > 0)
                {
                    out_callback_(utf8_pending_.substr(0, n), false);
                    utf8_pending_.erase(0, n);
                }
            }
        };
        engine->Run(ctx);
    }
    else
    {
        engine->Run(ctx);
        if (seq != req_seq_.load(std::memory_order_acquire))
        {
            return;
        }
        if (!ctx->error_message.empty())
        {
            out_callback_(std::string(""), true);
            return;
        }
        out_callback_(utf8_sanitize(ctx->final_text), true);
        return;
    }
}

void llm_task::start()
{
}

void llm_task::stop()
{
}

std::mutex llm_task::s_engine_mu_;
std::mutex llm_task::s_infer_mu_;
std::condition_variable llm_task::s_infer_cv_;
int llm_task::s_infer_active_ = 0;
std::weak_ptr<LlamaEngine> llm_task::s_engine_;
std::string llm_task::s_model_path_;
std::mutex llm_task::s_stream_mu_;
std::unordered_map<std::string, std::unordered_map<int, std::string>> llm_task::s_stream_buffs_;
