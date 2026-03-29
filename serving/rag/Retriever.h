#pragma once

#include <string>
#include <vector>

#include "serving/rag/Chunk.h"
#include "serving/rag/SqliteIndexStore.h"

class Retriever
{
public:
    explicit Retriever(const SqliteIndexStore &store);

    bool Retrieve(const std::string &kb,
                  const std::string &query,
                  int top_k,
                  const std::string &mode,
                  std::vector<RetrievalHit> &hits_out,
                  std::string &normalized_query_out,
                  std::string &error_out) const;

    static std::string NormalizeQuery(const std::string &query);

private:
    const SqliteIndexStore &store_;
};
