#pragma once

#include <string>
#include <vector>

std::string BuildToolPrompt(const std::string &agent_mode,
                            const std::vector<std::string> &allowed_tools);
std::string BuildToolResultMessage(const std::string &tool_name, const std::string &tool_output);
std::string BuildWebResearchDecompositionPrompt();
std::string BuildWebResearchSynthesisPrompt();
