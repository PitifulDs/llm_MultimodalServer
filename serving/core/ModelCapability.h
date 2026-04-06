#pragma once

#include <algorithm>
#include <cctype>
#include <string>

enum class ModelCapability
{
    Chat,
    Embeddings,
    Rerank
};

inline const char *ToString(ModelCapability capability)
{
    switch (capability)
    {
    case ModelCapability::Chat:
        return "chat";
    case ModelCapability::Embeddings:
        return "embeddings";
    case ModelCapability::Rerank:
        return "rerank";
    }
    return "chat";
}

inline bool ParseModelCapability(std::string value, ModelCapability &capability)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });

    if (value == "chat")
    {
        capability = ModelCapability::Chat;
        return true;
    }
    if (value == "embeddings")
    {
        capability = ModelCapability::Embeddings;
        return true;
    }
    if (value == "rerank")
    {
        capability = ModelCapability::Rerank;
        return true;
    }
    return false;
}
