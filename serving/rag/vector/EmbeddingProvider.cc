#include "serving/rag/vector/EmbeddingProvider.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> tokenize(std::string text)
{
    for (char &ch : text)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) == 0 && ch != '_' && ch != '/' && ch != ':' && ch != '.')
            ch = ' ';
        else
            ch = static_cast<char>(std::tolower(uch));
    }

    std::vector<std::string> tokens;
    std::string current;
    for (char ch : text)
    {
        if (ch == ' ')
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty())
        tokens.push_back(current);
    return tokens;
}

uint64_t fnv1a_64(const std::string &value)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : value)
    {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace

EmbeddingProvider::EmbeddingProvider()
    : EmbeddingProvider(Options{})
{
}

EmbeddingProvider::EmbeddingProvider(Options options)
    : options_(std::move(options))
{
    if (options_.dimension == 0)
        options_.dimension = 256;
}

std::vector<float> EmbeddingProvider::Embed(const std::string &text) const
{
    std::vector<float> embedding(options_.dimension, 0.0F);
    const auto tokens = tokenize(text);
    if (tokens.empty())
        return embedding;

    for (const auto &token : tokens)
    {
        const uint64_t hash = fnv1a_64(token);
        const size_t index = hash % options_.dimension;
        embedding[index] += 1.0F;

        const size_t index2 = ((hash / static_cast<uint64_t>(options_.dimension)) + token.size()) % options_.dimension;
        embedding[index2] += 0.5F;
    }

    float norm = 0.0F;
    for (float value : embedding)
        norm += value * value;
    norm = std::sqrt(norm);
    if (norm <= 0.0F)
        return embedding;

    for (float &value : embedding)
        value /= norm;
    return embedding;
}
