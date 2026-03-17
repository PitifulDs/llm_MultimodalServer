#pragma once

#include <string>

#include "serving/core/agent/AgentTypes.h"

AgentAction ParseAgentAction(const std::string &raw_output);
