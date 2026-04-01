#pragma once

#include <cstddef>
#include <string>
#include <vector>

class EmbeddingProvider
{
public:
    struct Options
    {
        size_t dimension = 256;
    };

    EmbeddingProvider();
    explicit EmbeddingProvider(Options options);

    size_t dimension() const { return options_.dimension; }
    std::vector<float> Embed(const std::string &text) const;

private:
    Options options_;
};
