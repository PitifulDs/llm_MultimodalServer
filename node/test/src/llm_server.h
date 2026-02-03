#pragma once

#include <memory>
#include <unordered_map>

#include "StackFlow.h"
#include "llm_task.h"

class llm_llm : public StackFlows::StackFlow
{
public:
    llm_llm();
    ~llm_llm();

    void task_output(const std::weak_ptr<llm_task> llm_task_obj_weak,
                     const std::weak_ptr<StackFlows::llm_channel_obj> llm_channel_weak,
                     const std::string &data,
                     bool finish);
    void task_user_data(const std::weak_ptr<llm_task> llm_task_obj_weak,
                        const std::weak_ptr<StackFlows::llm_channel_obj> llm_channel_weak,
                        const std::string &object,
                        const std::string &data);

    int setup(const std::string &work_id, const std::string &object, const std::string &data) override;
    void taskinfo(const std::string &work_id, const std::string &object, const std::string &data) override;
    int exit(const std::string &work_id, const std::string &object, const std::string &data) override;

private:
    int task_count_;
    std::unordered_map<int, std::shared_ptr<llm_task>> llm_task_;
};
