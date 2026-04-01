#include "serving/rag/vector/FaissIndexStore.h"

#include "utils/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using json = nlohmann::json;

std::string now_iso8601()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &now);
#else
    gmtime_r(&now, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool parse_npy_header(const std::string &header, size_t &rows_out, size_t &cols_out)
{
    if (header.find("'descr': '<f4'") == std::string::npos &&
        header.find("\"descr\": \"<f4\"") == std::string::npos)
    {
        return false;
    }
    if (header.find("False") == std::string::npos)
        return false;

    const auto shape_pos = header.find("shape");
    if (shape_pos == std::string::npos)
        return false;
    const auto lparen = header.find('(', shape_pos);
    const auto comma = header.find(',', lparen);
    const auto rparen = header.find(')', comma);
    if (lparen == std::string::npos || comma == std::string::npos || rparen == std::string::npos)
        return false;

    try
    {
        rows_out = static_cast<size_t>(std::stoull(header.substr(lparen + 1, comma - lparen - 1)));
        cols_out = static_cast<size_t>(std::stoull(header.substr(comma + 1, rparen - comma - 1)));
    }
    catch (...)
    {
        return false;
    }
    return rows_out > 0 && cols_out > 0;
}
} // namespace

FaissIndexStore::FaissIndexStore(Options options)
    : options_(std::move(options))
{
    status_.index_path = options_.index_path;
    status_.metadata_path = options_.metadata_path;
    status_.embeddings_path = options_.embeddings_path;
    status_.id_map_path = options_.id_map_path;
}

bool FaissIndexStore::Reload(std::string &error_out)
{
    error_out.clear();

    embeddings_.clear();
    row_to_chunk_id_.clear();
    chunk_id_to_row_.clear();
    chunks_.clear();
    status_ = VectorIndexStatus{};
    status_.index_path = options_.index_path;
    status_.metadata_path = options_.metadata_path;
    status_.embeddings_path = options_.embeddings_path;
    status_.id_map_path = options_.id_map_path;

    if (options_.metadata_path.empty() || options_.embeddings_path.empty() || options_.id_map_path.empty())
    {
        error_out = "vector index paths are empty";
        return false;
    }

    if (!load_chunk_metadata(error_out))
        return false;

    std::vector<std::string> ids;
    if (!load_id_map(ids, error_out))
        return false;

    if (!load_embeddings(ids.size(), error_out))
        return false;

    if (status_.dimension == 0)
    {
        error_out = "vector embeddings dimension is zero";
        return false;
    }

    std::vector<float> compact_embeddings;
    compact_embeddings.reserve(embeddings_.size());
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (chunks_.find(ids[i]) == chunks_.end())
            continue;
        const float *src = embeddings_.data() + i * status_.dimension;
        compact_embeddings.insert(compact_embeddings.end(), src, src + status_.dimension);
        row_to_chunk_id_.push_back(ids[i]);
        chunk_id_to_row_[ids[i]] = row_to_chunk_id_.size() - 1;
    }

    if (row_to_chunk_id_.empty())
    {
        error_out = "vector index contains no usable chunk ids";
        return false;
    }

    embeddings_.swap(compact_embeddings);

    status_.loaded = true;
    status_.total_chunks = row_to_chunk_id_.size();
    status_.last_loaded_at = now_iso8601();
    return true;
}

bool FaissIndexStore::Search(const std::string &kb,
                             const std::vector<float> &query_embedding,
                             int top_k,
                             std::vector<VectorSearchHit> &hits_out,
                             std::string &error_out) const
{
    hits_out.clear();
    error_out.clear();

    if (!status_.loaded)
    {
        error_out = "vector index is not loaded";
        return false;
    }
    if (query_embedding.size() != status_.dimension)
    {
        error_out = "query embedding dimension mismatch";
        return false;
    }

    const int effective_top_k = std::max(1, top_k);
    std::vector<VectorSearchHit> all_hits;
    all_hits.reserve(row_to_chunk_id_.size());
    for (size_t row = 0; row < row_to_chunk_id_.size(); ++row)
    {
        const auto chunk_it = chunks_.find(row_to_chunk_id_[row]);
        if (chunk_it == chunks_.end())
            continue;
        if (!kb.empty() && chunk_it->second.kb_name != kb)
            continue;

        const float *base = embeddings_.data() + row * status_.dimension;
        double dot = 0.0;
        for (size_t col = 0; col < status_.dimension; ++col)
            dot += static_cast<double>(query_embedding[col]) * static_cast<double>(base[col]);

        if (!std::isfinite(dot))
            continue;
        all_hits.push_back({row_to_chunk_id_[row], dot});
    }

    std::sort(all_hits.begin(), all_hits.end(), [](const VectorSearchHit &a, const VectorSearchHit &b)
    {
        if (a.score != b.score)
            return a.score > b.score;
        return a.chunk_id < b.chunk_id;
    });

    if (static_cast<int>(all_hits.size()) > effective_top_k)
        all_hits.resize(static_cast<size_t>(effective_top_k));
    hits_out = std::move(all_hits);
    return true;
}

bool FaissIndexStore::GetChunkById(const std::string &chunk_id, RagChunk &chunk_out) const
{
    const auto it = chunks_.find(chunk_id);
    if (it == chunks_.end())
        return false;
    chunk_out = it->second;
    return true;
}

VectorIndexStatus FaissIndexStore::status() const
{
    return status_;
}

bool FaissIndexStore::load_chunk_metadata(std::string &error_out)
{
    std::ifstream in(options_.metadata_path);
    if (!in.is_open())
    {
        error_out = "rag chunk metadata not found: " + options_.metadata_path;
        return false;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;

        json item;
        try
        {
            item = json::parse(line);
        }
        catch (const std::exception &e)
        {
            error_out = "failed to parse chunk metadata jsonl: " + std::string(e.what());
            return false;
        }

        RagChunk chunk;
        chunk.chunk_id = item.value("chunk_id", "");
        chunk.kb_name = item.value("kb_name", "");
        chunk.doc_id = item.value("doc_id", "");
        chunk.path = item.value("path", "");
        chunk.title = item.value("title", "");
        chunk.symbol = item.value("symbol", "");
        chunk.start_line = item.value("start_line", 0);
        chunk.end_line = item.value("end_line", 0);
        chunk.language = item.value("language", "");
        chunk.text = item.value("text", "");
        chunk.token_estimate = item.value("token_estimate", 0);
        chunk.prev_chunk_id = item.value("prev_chunk_id", "");
        chunk.next_chunk_id = item.value("next_chunk_id", "");
        if (chunk.chunk_id.empty())
            continue;

        chunks_[chunk.chunk_id] = chunk;
    }

    size_t docs = 0;
    size_t repo = 0;
    for (const auto &entry : chunks_)
    {
        if (entry.second.kb_name == "docs")
            ++docs;
        else if (entry.second.kb_name == "repo_code")
            ++repo;
    }
    status_.docs_chunks = docs;
    status_.repo_code_chunks = repo;
    return true;
}

bool FaissIndexStore::load_id_map(std::vector<std::string> &ids_out, std::string &error_out) const
{
    ids_out.clear();
    const std::string raw = read_file(options_.id_map_path);
    if (raw.empty())
    {
        error_out = "rag id map not found: " + options_.id_map_path;
        return false;
    }

    try
    {
        const json parsed = json::parse(raw);
        if (parsed.is_array())
        {
            for (const auto &item : parsed)
            {
                if (item.is_string())
                    ids_out.push_back(item.get<std::string>());
            }
        }
        else if (parsed.is_object() && parsed.contains("chunk_ids") && parsed["chunk_ids"].is_array())
        {
            for (const auto &item : parsed["chunk_ids"])
            {
                if (item.is_string())
                    ids_out.push_back(item.get<std::string>());
            }
        }
    }
    catch (const std::exception &e)
    {
        error_out = "failed to parse rag id map: " + std::string(e.what());
        return false;
    }

    if (ids_out.empty())
    {
        error_out = "rag id map is empty";
        return false;
    }
    return true;
}

bool FaissIndexStore::load_embeddings(size_t expected_rows, std::string &error_out)
{
    std::ifstream in(options_.embeddings_path, std::ios::binary);
    if (!in.is_open())
    {
        error_out = "rag embeddings file not found: " + options_.embeddings_path;
        return false;
    }

    char magic[6] = {};
    in.read(magic, 6);
    if (std::string(magic, 6) != "\x93NUMPY")
    {
        error_out = "unsupported npy header";
        return false;
    }

    char version[2] = {};
    in.read(version, 2);
    uint16_t header_len = 0;
    if (version[0] == 1)
    {
        char len_buf[2] = {};
        in.read(len_buf, 2);
        header_len = static_cast<uint16_t>(static_cast<unsigned char>(len_buf[0]) |
                                           (static_cast<unsigned char>(len_buf[1]) << 8));
    }
    else
    {
        error_out = "unsupported npy version";
        return false;
    }

    std::string header(header_len, '\0');
    in.read(&header[0], static_cast<std::streamsize>(header_len));
    if (!in.good())
    {
        error_out = "failed to read npy header";
        return false;
    }

    size_t rows = 0;
    size_t cols = 0;
    if (!parse_npy_header(header, rows, cols))
    {
        error_out = "failed to parse npy shape";
        return false;
    }
    if (rows != expected_rows)
    {
        error_out = "rag embeddings rows mismatch id map";
        return false;
    }

    status_.dimension = cols;
    embeddings_.resize(rows * cols);
    in.read(reinterpret_cast<char *>(embeddings_.data()),
            static_cast<std::streamsize>(embeddings_.size() * sizeof(float)));
    if (!in.good() && !in.eof())
    {
        error_out = "failed to read rag embeddings";
        return false;
    }
    return true;
}
