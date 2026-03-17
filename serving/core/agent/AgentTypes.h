#pragma once

#include <string>

#include "utils/json.hpp"

struct AgentAction
{
    enum class Type
    {
        final_answer,
        tool_call
    };

    Type type = Type::final_answer;
    std::string answer;
    std::string tool_name;
    nlohmann::json tool_input = nlohmann::json::object();
};
