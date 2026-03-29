#pragma once

#include <string>
#include <vector>

#include "serving/rag/Chunk.h"

class SqliteIndexStore
{
public:
    explicit SqliteIndexStore(std::string index_path);

    const std::string &index_path() const { return index_path_; }

    bool Search(const std::string &kb,
                const std::string &query,
                int top_k,
                std::vector<RetrievalHit> &hits_out,
                std::string &error_out) const;

private:
    std::string index_path_;
};
