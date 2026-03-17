#pragma once

#include <string>
#include <vector>

std::string BuildToolPrompt(const std::vector<std::string> &allowed_tools);
std::string BuildToolResultMessage(const std::string &tool_name, const std::string &tool_output);
