#pragma once

#include <memory>

#include "serving/service/ChatTypes.h"

class ModelEngine;

class LegacyChatEngineFacade
{
public:
    explicit LegacyChatEngineFacade(std::shared_ptr<ModelEngine> engine);

    bool Supports(ModelCapability capability) const;
    void RunChat(const ChatRequest &request, ChatResponse &response) const;

private:
    std::shared_ptr<ModelEngine> engine_;
};
