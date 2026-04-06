#include "llm_server.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <glog/logging.h>

using namespace StackFlows;
using json = nlohmann::json;

namespace
{
std::string safe_json_dump(const json &value)
{
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool decode_stream_payload(const std::string &in,
                           std::string &out,
                           std::unordered_map<int, std::string> &stream_buff)
{
    const json body = json::parse(in, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        throw std::runtime_error("invalid stream payload");

    int index = 0;
    if (body.contains("index") && body["index"].is_number_integer())
        index = body["index"].get<int>();

    bool finish = false;
    if (body.contains("finish") && body["finish"].is_boolean())
        finish = body["finish"].get<bool>();

    std::string delta;
    if (body.contains("delta"))
    {
        const auto &raw_delta = body["delta"];
        if (raw_delta.is_string())
            delta = raw_delta.get<std::string>();
        else if (raw_delta.is_null())
            delta.clear();
        else
            delta = safe_json_dump(raw_delta);
    }

    stream_buff[index] = delta;
    if (!finish)
        return true;

    for (size_t i = 0; i < stream_buff.size(); ++i)
    {
        auto it = stream_buff.find(static_cast<int>(i));
        if (it != stream_buff.end())
            out += it->second;
    }
    stream_buff.clear();
    return false;
}
} // namespace

llm_llm::llm_llm() : StackFlow("llm")
{
    task_count_ = 3;
}

void llm_llm::task_output(const std::weak_ptr<llm_task> llm_task_obj_weak,
                          const std::weak_ptr<llm_channel_obj> llm_channel_weak,
                          const std::string &data,
                          bool finish,
                          const std::string &finish_reason)
{
    static std::mutex s_idx_mu;
    static std::unordered_map<std::string, int> s_idx;

    auto llm_task_obj = llm_task_obj_weak.lock();
    auto llm_channel = llm_channel_weak.lock();
    if (!(llm_task_obj && llm_channel))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(llm_task_obj->req_mu_);
        if (!llm_task_obj->current_req_id_.empty())
            llm_channel->request_id_ = llm_task_obj->current_req_id_;
        if (!llm_task_obj->current_work_id_.empty())
            llm_channel->work_id_ = llm_task_obj->current_work_id_;
    }
    if (llm_channel->enstream_)
    {
        json data_body;
        {
            std::lock_guard<std::mutex> lk(s_idx_mu);
            int &count = s_idx[llm_channel->request_id_];
            data_body["index"] = count++;
        }
        data_body["delta"] = data;
        if (!finish)
            data_body["delta"] = data;
        else
            data_body["delta"] = std::string("");
        data_body["finish"] = finish;
        if (finish && !finish_reason.empty())
            data_body["finish_reason"] = finish_reason;
        if (finish)
        {
            std::lock_guard<std::mutex> lk(s_idx_mu);
            s_idx.erase(llm_channel->request_id_);
        }

        llm_channel->send(llm_task_obj->response_format_, data_body, LLM_NO_ERROR);
        if (finish)
        {
            std::lock_guard<std::mutex> lk(llm_task_obj->req_mu_);
            if (!llm_task_obj->current_req_id_.empty())
                llm_channel->clear_request_url(llm_task_obj->current_req_id_);
        }
    }
    else if (finish)
    {
        json data_body;
        data_body["delta"] = data;
        data_body["finish"] = true;
        if (!finish_reason.empty())
            data_body["finish_reason"] = finish_reason;
        llm_channel->send(llm_task_obj->response_format_, data_body, LLM_NO_ERROR);
        std::lock_guard<std::mutex> lk(llm_task_obj->req_mu_);
        if (!llm_task_obj->current_req_id_.empty())
            llm_channel->clear_request_url(llm_task_obj->current_req_id_);
    }
}

void llm_llm::task_user_data(const std::weak_ptr<llm_task> llm_task_obj_weak,
                             const std::weak_ptr<llm_channel_obj> llm_channel_weak,
                             const std::string &object,
                             const std::string &data)
{
    json error_body;
    auto llm_task_obj = llm_task_obj_weak.lock();
    auto llm_channel = llm_channel_weak.lock();
    if (!(llm_task_obj && llm_channel))
    {
        error_body["code"] = -11;
        error_body["message"] = "Model run failed.";
        send("None", "None", error_body, unit_name_);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(llm_task_obj->req_mu_);
        llm_task_obj->current_req_id_ = llm_channel->request_id_;
        llm_task_obj->current_work_id_ = llm_channel->work_id_;
    }
    const bool req_stream = (object.find("stream") != std::string::npos);
    llm_task_obj->enstream_ = req_stream;
    llm_task_obj->response_format_ = object;
    llm_channel->set_stream(req_stream);
    if (data.empty() || (data == "None"))
    {
        error_body["code"] = -24;
        error_body["message"] = "The inference data is empty.";
        send("None", "None", error_body, unit_name_);
        return;
    }
    const std::string *next_data = &data;
    std::string tmp_msg;
    if (object.find("stream") != std::string::npos)
    {
        try
        {
            std::lock_guard<std::mutex> lk(llm_task::s_stream_mu_);
            auto &stream_buff = llm_task::s_stream_buffs_[llm_channel->request_id_];
            if (decode_stream_payload(data, tmp_msg, stream_buff)) {
                return;
            };
            llm_task::s_stream_buffs_.erase(llm_channel->request_id_);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lk(llm_task::s_stream_mu_);
            llm_task::s_stream_buffs_.erase(llm_channel->request_id_);
            error_body["code"] = -25;
            error_body["message"] = "Stream data index error.";
            send("None", "None", error_body, unit_name_);
            return;
        }
        next_data = &tmp_msg;
    }

    if (!llm_task_obj->try_begin_infer())
    {
        LOG(WARNING) << "[llm_task] busy, reject request_id=" << llm_channel->request_id_;
        if (object.find("stream") != std::string::npos)
        {
            std::lock_guard<std::mutex> lk(llm_task::s_stream_mu_);
            llm_task::s_stream_buffs_.erase(llm_channel->request_id_);
        }
        error_body["code"] = -26;
        error_body["message"] = "Unit busy, try later.";
        send("None", "None", error_body, unit_name_);
        return;
    }

    const uint64_t seq = llm_task_obj->req_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    llm_task_obj->inference((*next_data), llm_channel->request_id_, llm_channel->work_id_, seq);
}

int llm_llm::setup(const std::string &work_id, const std::string &object, const std::string &data)
{
    json error_body;
    if ((llm_task_channel_.size() - 1) == task_count_)
    {

        error_body["code"] = -21;
        error_body["message"] = "task full";
        send("None", "None", error_body, unit_name_);
        return -1;
    }
    int work_id_num = sample_get_work_id_num(work_id);
    auto llm_channel = get_channel(work_id);
    auto llm_task_obj = std::make_shared<llm_task>(work_id);
    json config_body;
    config_body = json::parse(data, nullptr, false);
    if (config_body.is_discarded() || !config_body.is_object())
    {
        error_body["code"] = -2;
        error_body["message"] = "json format error.";
        send("None", "None", error_body, unit_name_);
        return -2;
    }
    int ret = llm_task_obj->load_model(config_body);
    if (ret == 0)
    {
        llm_channel->set_output(true);
        llm_channel->set_stream(llm_task_obj->enstream_);
        llm_task_obj->set_output(std::bind(&llm_llm::task_output, this, std::weak_ptr<llm_task>(llm_task_obj),
                                           std::weak_ptr<llm_channel_obj>(llm_channel), std::placeholders::_1,
                                           std::placeholders::_2, std::placeholders::_3));
        llm_channel->subscriber_work_id(
            "",
            std::bind(&llm_llm::task_user_data, this, std::weak_ptr<llm_task>(llm_task_obj),
                      std::weak_ptr<llm_channel_obj>(llm_channel), std::placeholders::_1, std::placeholders::_2));
        llm_task_[work_id_num] = llm_task_obj;
        send("None", "None", LLM_NO_ERROR, work_id);

        return 0;
    }
    else
    {
        error_body["code"] = -5;
        error_body["message"] = "Model loading failed.";
        send("None", "None", error_body, unit_name_);
        return -1;
    }
}

void llm_llm::taskinfo(const std::string &work_id, const std::string &object, const std::string &data)
{
    json req_body;
    int work_id_num = sample_get_work_id_num(work_id);
    if (WORK_ID_NONE == work_id_num)
    {
        std::vector<std::string> task_list;
        std::transform(llm_task_channel_.begin(), llm_task_channel_.end(), std::back_inserter(task_list),
                       [](const auto task_channel)
                       { return task_channel.second->work_id_; });
        req_body = task_list;
        send("llm.tasklist", req_body, LLM_NO_ERROR, work_id);
    }
    else
    {
        if (llm_task_.find(work_id_num) == llm_task_.end())
        {
            req_body["code"] = -6;
            req_body["message"] = "Unit Does Not Exist";
            send("None", "None", req_body, work_id);
            return;
        }
        auto llm_task_obj = llm_task_[work_id_num];
        req_body["model"] = llm_task_obj->model_;
        req_body["response_format"] = llm_task_obj->response_format_;
        req_body["enoutput"] = llm_task_obj->enoutput_;
        req_body["inputs"] = llm_task_obj->inputs_;
        send("llm.taskinfo", req_body, LLM_NO_ERROR, work_id);
    }
}

int llm_llm::exit(const std::string &work_id, const std::string &object, const std::string &data)
{
    json error_body;
    int work_id_num = sample_get_work_id_num(work_id);
    if (llm_task_.find(work_id_num) == llm_task_.end())
    {
        error_body["code"] = -6;
        error_body["message"] = "Unit Does Not Exist";
        send("None", "None", error_body, work_id);
        return -1;
    }
    llm_task_[work_id_num]->stop();
    auto llm_channel = get_channel(work_id_num);
    llm_channel->stop_subscriber("");
    llm_task_.erase(work_id_num);
    send("None", "None", LLM_NO_ERROR, work_id);
    return 0;
}

llm_llm::~llm_llm()
{
    while (1)
    {
        auto iteam = llm_task_.begin();
        if (iteam == llm_task_.end())
        {
            break;
        }
        iteam->second->stop();
        get_channel(iteam->first)->stop_subscriber("");
        iteam->second.reset();
        llm_task_.erase(iteam->first);
    }
}
