#include "serving/core/SessionManager.h"
#include "serving/core/agent/BuiltinTools.h"
#include "serving/core/agent/ToolRegistry.h"
#include "serving/http/ChatRequestParser.h"
#include "serving/http/HttpGateway.h"
#include "serving/http/HttpUtils.h"
#include "serving/http/OpenAIStreamWriter.h"
#include "serving/http/http_types.h"
#include "serving/rag/RAGExecutor.h"
#include "serving/rag/vector/EmbeddingProvider.h"

#include "utils/json.hpp"

#include <glog/logging.h>
#include <sqlite3.h>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

#define EXPECT_TRUE(cond)                                                                       \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            std::cerr << "EXPECT_TRUE failed: " << #cond << " at line " << __LINE__ << "\n"; \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

#define EXPECT_EQ(a, b)                                                                                     \
    do                                                                                                       \
    {                                                                                                        \
        if (!((a) == (b)))                                                                                   \
        {                                                                                                    \
            std::cerr << "EXPECT_EQ failed: " << #a << " vs " << #b << " at line " << __LINE__ << "\n"; \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

namespace
{
struct TestChunkRow
{
    std::string chunk_id;
    std::string kb_name;
    std::string path;
    std::string title;
    std::string symbol;
    int start_line = 0;
    int end_line = 0;
    std::string language;
    std::string text;
    int token_estimate = 0;
    std::string prev_chunk_id;
    std::string next_chunk_id;
};

struct TestArtifacts
{
    std::filesystem::path dir;
    std::filesystem::path sqlite_path;
    std::filesystem::path chunks_path;
    std::filesystem::path embeddings_path;
    std::filesystem::path faiss_path;
    std::filesystem::path id_map_path;
};

struct FakeRequest : HttpRequest
{
    std::unordered_map<std::string, std::string> query;

    bool HasQuery(const std::string &key) const override
    {
        return query.find(key) != query.end();
    }

    std::string Query(const std::string &key) const override
    {
        const auto it = query.find(key);
        return it == query.end() ? std::string() : it->second;
    }
};

struct FakeResponse : HttpResponse
{
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool ended = false;
    std::function<void()> on_close;

    void SetHeader(const std::string &key, const std::string &value) override
    {
        headers[key] = value;
    }

    void Write(const std::string &data) override
    {
        body += data;
    }

    bool IsAlive() const override
    {
        return true;
    }

    void SetStatus(int code, const std::string &reason = "") override
    {
        (void)reason;
        status = code;
    }

    void End() override
    {
        ended = true;
    }

    void SetOnClose(std::function<void()> cb) override
    {
        on_close = std::move(cb);
    }
};

std::filesystem::path make_temp_dir()
{
    static int seq = 0;
    const auto path = std::filesystem::temp_directory_path() / ("edge_rag_v2_test_" + std::to_string(++seq));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    return path;
}

bool exec_sql(sqlite3 *db, const char *sql)
{
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (errmsg)
        sqlite3_free(errmsg);
    return rc == SQLITE_OK;
}

void write_npy(const std::filesystem::path &path, const std::vector<std::vector<float>> &matrix)
{
    const size_t rows = matrix.size();
    const size_t cols = rows ? matrix.front().size() : 0;
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" +
                         std::to_string(rows) + ", " + std::to_string(cols) + "), }";
    const size_t pad = 16 - ((10 + header.size() + 1) % 16);
    header.append(pad, ' ');
    header.push_back('\n');

    std::ofstream out(path, std::ios::binary);
    out.write("\x93NUMPY", 6);
    const char version[2] = {1, 0};
    out.write(version, 2);
    const uint16_t header_len = static_cast<uint16_t>(header.size());
    out.write(reinterpret_cast<const char *>(&header_len), sizeof(header_len));
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (const auto &row : matrix)
    {
        out.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(float)));
    }
}

std::vector<TestChunkRow> build_rows()
{
    return {
        {"c1", "repo_code", "serving/http/HttpGateway.cc", "HttpGateway.cc", "HandleChatCompletion", 10, 32, "cpp", "references are appended in non stream response for chat completion.", 12, "", "c2"},
        {"c2", "repo_code", "serving/http/HttpGateway.cc", "HttpGateway.cc", "HandleChatCompletionStream", 33, 72, "cpp", "stream mode starts HTTP session writer and sends metadata before done.", 14, "c1", ""},
        {"c3", "repo_code", "serving/http/OpenAIStreamWriter.cc", "OpenAIStreamWriter.cc", "OnChunk", 1, 50, "cpp", "OnChunk writes stream metadata and references before the final done event.", 14, "", ""},
        {"c4", "repo_code", "serving/core/SessionExecutor.cc", "SessionExecutor.cc", "Submit", 1, 44, "cpp", "SessionExecutor serializes tasks for the same session to protect conversation state.", 13, "", ""},
        {"c5", "docs", "README.md", "README.md", "RAG", 1, 20, "markdown", "README documents lexical vector and hybrid rag retrieval.", 10, "", ""},
        {"c6", "repo_code", "node/test/src/llm_task.cc", "llm_task.cc", "set_output", 253, 256, "cpp", "stream metadata references are forwarded into a callback output in node tests.", 10, "", ""},
        {"c7", "repo_code", "thirds/llama.cpp/ggml/src/ggml-sycl/common.hpp", "common.hpp", "stream", 327, 329, "cpp", "stream metadata references are associated with a vendor stream helper.", 10, "", ""},
    };
}

TestArtifacts create_test_artifacts()
{
    const auto dir = make_temp_dir();
    TestArtifacts artifacts;
    artifacts.dir = dir;
    artifacts.sqlite_path = dir / "rag_index.sqlite";
    artifacts.chunks_path = dir / "rag_chunks.jsonl";
    artifacts.embeddings_path = dir / "rag_embeddings.npy";
    artifacts.faiss_path = dir / "rag_faiss.index";
    artifacts.id_map_path = dir / "rag_id_map.json";

    const auto rows = build_rows();

    sqlite3 *db = nullptr;
    sqlite3_open(artifacts.sqlite_path.c_str(), &db);
    const char *schema =
        "CREATE TABLE chunks ("
        "chunk_id TEXT PRIMARY KEY,"
        "kb_name TEXT NOT NULL,"
        "doc_id TEXT NOT NULL,"
        "path TEXT NOT NULL,"
        "title TEXT,"
        "symbol TEXT,"
        "start_line INTEGER,"
        "end_line INTEGER,"
        "language TEXT,"
        "text TEXT NOT NULL,"
        "token_estimate INTEGER,"
        "prev_chunk_id TEXT,"
        "next_chunk_id TEXT"
        ");"
        "CREATE VIRTUAL TABLE chunks_fts USING fts5("
        "chunk_id UNINDEXED, kb_name UNINDEXED, path, title, symbol, text"
        ");";
    exec_sql(db, schema);

    for (const auto &row : rows)
    {
        std::string sql =
            "INSERT INTO chunks VALUES ('" + row.chunk_id + "','" + row.kb_name + "','" + row.path + "','" + row.path + "','" +
            row.title + "','" + row.symbol + "'," + std::to_string(row.start_line) + "," + std::to_string(row.end_line) + ",'" +
            row.language + "','" + row.text + "'," + std::to_string(row.token_estimate) + ",'" + row.prev_chunk_id + "','" +
            row.next_chunk_id + "');";
        exec_sql(db, sql.c_str());
        sql = "INSERT INTO chunks_fts VALUES ('" + row.chunk_id + "','" + row.kb_name + "','" + row.path + "','" + row.title +
              "','" + row.symbol + "','" + row.text + "');";
        exec_sql(db, sql.c_str());
    }
    sqlite3_close(db);

    EmbeddingProvider embedding_provider;
    std::vector<std::vector<float>> matrix;
    json id_map = {{"chunk_ids", json::array()}};
    std::ofstream chunks_out(artifacts.chunks_path);
    for (const auto &row : rows)
    {
        json item = {
            {"chunk_id", row.chunk_id},
            {"kb_name", row.kb_name},
            {"doc_id", row.path},
            {"path", row.path},
            {"title", row.title},
            {"symbol", row.symbol},
            {"start_line", row.start_line},
            {"end_line", row.end_line},
            {"language", row.language},
            {"text", row.text},
            {"token_estimate", row.token_estimate},
            {"prev_chunk_id", row.prev_chunk_id},
            {"next_chunk_id", row.next_chunk_id},
        };
        chunks_out << item.dump() << "\n";
        matrix.push_back(embedding_provider.Embed(row.path + "\n" + row.symbol + "\n" + row.text));
        id_map["chunk_ids"].push_back(row.chunk_id);
    }
    chunks_out.close();
    write_npy(artifacts.embeddings_path, matrix);
    std::ofstream id_map_out(artifacts.id_map_path);
    id_map_out << id_map.dump(2);
    id_map_out.close();
    std::ofstream faiss_out(artifacts.faiss_path);
    faiss_out << "{\"type\":\"faiss_placeholder\",\"rows\":" << rows.size() << "}";
    faiss_out.close();
    return artifacts;
}

int run_agent_tool_tests(RAGExecutor &rag_exec)
{
    BuiltinToolsOptions options;
    options.max_tool_output_chars = 4000;
    options.search_kb_handler = [&rag_exec](const json &input)
    {
        RetrievalRequest request;
        request.kb = input.value("kb", "repo_code");
        request.query = input.value("query", "");
        request.top_k = input.value("top_k", 3);
        request.mode = input.value("mode", "hybrid");
        RetrievalResponse response;
        std::string error;
        if (!rag_exec.Search(request, response, error))
            return error;
        std::string out;
        for (const auto &hit : response.hits)
        {
            out += hit.chunk.chunk_id + " " + hit.chunk.path + "\n";
        }
        return out;
    };
    options.open_chunk_handler = [&rag_exec](const json &input)
    {
        RagChunk chunk;
        std::string error;
        if (!rag_exec.OpenChunk(input.value("chunk_id", ""), chunk, error))
            return error;
        return chunk.text;
    };

    ToolRegistry registry;
    RegisterBuiltinTools(registry, options, []() { return std::string("{}"); });
    EXPECT_TRUE(registry.Has("search_kb"));
    EXPECT_TRUE(registry.Has("open_chunk"));
    const auto search_output = registry.Execute("search_kb", {{"kb", "repo_code"}, {"query", "stream metadata"}, {"top_k", 2}, {"mode", "hybrid"}});
    EXPECT_TRUE(search_output.find("c2") != std::string::npos || search_output.find("c3") != std::string::npos);
    const auto chunk_output = registry.Execute("open_chunk", {{"chunk_id", "c2"}});
    EXPECT_TRUE(chunk_output.find("metadata before done") != std::string::npos);
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    google::InitGoogleLogging(argc > 0 ? argv[0] : "rag_test");

    const auto artifacts = create_test_artifacts();
    SessionManager::Options opt;
    SessionManager session_mgr(opt);

    const std::string body = R"json({
        "model":"llama",
        "messages":[{"role":"user","content":"How does rag work?"}],
        "rag":{
          "enabled":true,
          "kb":"repo_code",
          "top_k":6,
          "mode":"hybrid",
          "lexical_top_k":8,
          "vector_top_k":10,
          "fusion":"rrf",
          "debug":true,
          "return_references":true
        }
    })json";

    auto parsed = ParseChatRequestBody(body, false, session_mgr, "llama", 128, "req-test");
    EXPECT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.request.ctx->rag_options.enabled);
    EXPECT_EQ(parsed.request.ctx->rag_options.mode, std::string("hybrid"));
    EXPECT_EQ(parsed.request.ctx->rag_options.lexical_top_k, 8);
    EXPECT_EQ(parsed.request.ctx->rag_options.vector_top_k, 10);
    EXPECT_EQ(parsed.request.ctx->rag_options.fusion, std::string("rrf"));
    EXPECT_TRUE(parsed.request.ctx->rag_options.debug);

    RAGExecutor::Options rag_opt;
    rag_opt.index_path = artifacts.sqlite_path.string();
    rag_opt.vector_index_path = artifacts.faiss_path.string();
    rag_opt.chunk_metadata_path = artifacts.chunks_path.string();
    rag_opt.vector_embeddings_path = artifacts.embeddings_path.string();
    rag_opt.vector_id_map_path = artifacts.id_map_path.string();
    rag_opt.default_top_k = 4;
    rag_opt.max_context_chars = 4096;
    rag_opt.default_mode = "hybrid";
    rag_opt.default_fusion = "rrf";
    rag_opt.enable_neighbor_expand = true;
    rag_opt.max_neighbor_count = 1;
    RAGExecutor rag_exec(rag_opt);

    RetrievalRequest hybrid_request;
    hybrid_request.kb = "repo_code";
    hybrid_request.query = "stream metadata references";
    hybrid_request.top_k = 2;
    hybrid_request.mode = "hybrid";
    hybrid_request.lexical_top_k = 4;
    hybrid_request.vector_top_k = 4;
    hybrid_request.fusion = "rrf";

    RetrievalResponse hybrid_response;
    std::string error;
    EXPECT_TRUE(rag_exec.Search(hybrid_request, hybrid_response, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(hybrid_response.summary.mode, std::string("hybrid"));
    EXPECT_EQ(hybrid_response.hits.size(), 2u);
    EXPECT_TRUE(hybrid_response.hits[0].chunk.path != hybrid_response.hits[1].chunk.path);
    EXPECT_TRUE(hybrid_response.hits[0].chunk.path.rfind("serving/", 0) == 0);

    RetrievalRequest neighbor_request;
    neighbor_request.kb = "repo_code";
    neighbor_request.query = "non stream response";
    neighbor_request.top_k = 2;
    neighbor_request.mode = "lexical";
    neighbor_request.lexical_top_k = 1;
    neighbor_request.fusion = "rrf";
    RetrievalResponse neighbor_response;
    EXPECT_TRUE(rag_exec.Search(neighbor_request, neighbor_response, error));
    EXPECT_EQ(neighbor_response.hits.size(), 2u);
    EXPECT_EQ(neighbor_response.hits[0].chunk.chunk_id, std::string("c1"));
    EXPECT_EQ(neighbor_response.hits[1].chunk.chunk_id, std::string("c2"));
    EXPECT_TRUE(neighbor_response.hits[1].from_neighbor);

    RetrievalRequest vendor_request;
    vendor_request.kb = "repo_code";
    vendor_request.query = "thirds llama.cpp ggml stream helper";
    vendor_request.top_k = 1;
    vendor_request.mode = "hybrid";
    vendor_request.fusion = "rrf";
    RetrievalResponse vendor_response;
    EXPECT_TRUE(rag_exec.Search(vendor_request, vendor_response, error));
    EXPECT_EQ(vendor_response.hits.size(), 1u);
    EXPECT_TRUE(vendor_response.hits[0].chunk.path.rfind("thirds/", 0) == 0);

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = "req-rag";
    ctx->stream = true;
    ctx->is_chat = true;
    ctx->session = session_mgr.getOrCreate("sid-rag", "llama", "");
    ctx->messages = {{"user", "stream metadata references"}};
    ctx->rag_options.enabled = true;
    ctx->rag_options.kb = "repo_code";
    ctx->rag_options.mode = "hybrid";
    ctx->rag_options.top_k = 2;
    ctx->rag_options.return_references = true;
    ctx->rag_options.debug = true;
    EXPECT_TRUE(rag_exec.Apply(ctx));
    EXPECT_TRUE(!ctx->stream_metadata_json.empty());
    EXPECT_TRUE(ctx->messages.size() >= 2u);

    EXPECT_TRUE(run_agent_tool_tests(rag_exec) == 0);

    setenv("RAG_INDEX_PATH", artifacts.sqlite_path.c_str(), 1);
    setenv("RAG_VECTOR_INDEX_PATH", artifacts.faiss_path.c_str(), 1);
    setenv("RAG_CHUNK_METADATA_PATH", artifacts.chunks_path.c_str(), 1);
    setenv("RAG_EMBEDDINGS_PATH", artifacts.embeddings_path.c_str(), 1);
    setenv("RAG_ID_MAP_PATH", artifacts.id_map_path.c_str(), 1);
    setenv("RAG_ENABLE_RETRIEVAL_DEBUG_API", "1", 1);
    HttpGateway gateway;

    FakeRequest req;
    req.body = R"json({"kb":"repo_code","query":"stream metadata","mode":"hybrid","top_k":2,"debug":true})json";
    FakeResponse res;
    gateway.HandleRetrievalSearch(req, res);
    EXPECT_EQ(res.status, 200);
    const auto search_json = json::parse(res.body);
    EXPECT_EQ(search_json["mode"].get<std::string>(), std::string("hybrid"));
    EXPECT_TRUE(search_json["hits"].is_array());
    EXPECT_TRUE(!search_json["hits"].empty());

    std::vector<std::string> stream_outputs;
    OpenAIStreamWriter writer("req-stream", "llama", [&stream_outputs](const std::string &s)
                              { stream_outputs.push_back(s); });
    StreamChunk meta_chunk;
    meta_chunk.metadata_json = ctx->stream_metadata_json;
    writer.OnChunk(meta_chunk);
    StreamChunk final_chunk;
    final_chunk.is_finished = true;
    final_chunk.finish_reason = FinishReason::stop;
    writer.OnChunk(final_chunk);
    EXPECT_TRUE(stream_outputs.size() >= 3u);
    EXPECT_TRUE(stream_outputs[0].find("\"metadata\"") != std::string::npos);
    EXPECT_TRUE(stream_outputs.back().find("[DONE]") != std::string::npos);

    const auto status = rag_exec.GetStatus();
    EXPECT_TRUE(status.docs_chunk_count >= 1);
    EXPECT_TRUE(status.repo_code_chunk_count >= 4);
    EXPECT_TRUE(status.vector_index_loaded);

    std::error_code ec;
    std::filesystem::remove_all(artifacts.dir, ec);
    google::ShutdownGoogleLogging();
    std::cout << "rag_test passed\n";
    return 0;
}
