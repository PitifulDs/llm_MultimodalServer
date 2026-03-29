#include "serving/rag/Retriever.h"

#include <algorithm>
#include <cctype>
#include <string>

Retriever::Retriever(const SqliteIndexStore &store)
    : store_(store)
{
}

std::string Retriever::NormalizeQuery(const std::string &query)
{
    std::string out;
    out.reserve(query.size());

    bool seen_non_space = false;
    bool pending_space = false;
    for (unsigned char ch : query)
    {
        if (std::isspace(ch) || std::ispunct(ch))
        {
            if (seen_non_space)
                pending_space = true;
            continue;
        }

        if (pending_space && !out.empty())
            out.push_back(' ');
        out.push_back(static_cast<char>(ch));
        seen_non_space = true;
        pending_space = false;
    }

    return out;
}

bool Retriever::Retrieve(const std::string &kb,
                         const std::string &query,
                         int top_k,
                         const std::string &mode,
                         std::vector<RetrievalHit> &hits_out,
                         std::string &normalized_query_out,
                         std::string &error_out) const
{
    hits_out.clear();
    error_out.clear();
    normalized_query_out = NormalizeQuery(query);

    if (kb != "docs" && kb != "repo_code")
    {
        error_out = "rag.kb must be one of: docs, repo_code";
        return false;
    }

    if (!mode.empty() && mode != "lexical")
    {
        error_out = "rag.mode only supports lexical in v1";
        return false;
    }

    const int effective_top_k = std::clamp(top_k, 1, 20);
    return store_.Search(kb, normalized_query_out, effective_top_k, hits_out, error_out);
}
