#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "utils/json.hpp"

class ToolRegistry
{
public:
    using ToolHandler = std::function<std::string(const nlohmann::json &)>;

    void Register(const std::string &name, ToolHandler handler);
    bool Has(const std::string &name) const;
    std::string Execute(const std::string &name, const nlohmann::json &input) const;
    std::vector<std::string> RegisteredToolNames() const;

private:
    std::unordered_map<std::string, ToolHandler> handlers_;
    std::vector<std::string> ordered_names_;
};
