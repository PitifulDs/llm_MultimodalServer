#include "serving/core/agent/ToolRegistry.h"

void ToolRegistry::Register(const std::string &name, ToolHandler handler)
{
    const bool is_new = handlers_.find(name) == handlers_.end();
    handlers_[name] = std::move(handler);
    if (is_new)
        ordered_names_.push_back(name);
}

bool ToolRegistry::Has(const std::string &name) const
{
    return handlers_.find(name) != handlers_.end();
}

std::string ToolRegistry::Execute(const std::string &name, const nlohmann::json &input) const
{
    const auto it = handlers_.find(name);
    if (it == handlers_.end())
        return "unknown tool: " + name;
    return it->second(input);
}

std::vector<std::string> ToolRegistry::RegisteredToolNames() const
{
    return ordered_names_;
}
