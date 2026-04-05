#include "serving/core/SessionManager.h"
#include "serving/core/agent/code_analysis/CodeAnalysisEvidence.h"
#include "serving/core/agent/code_analysis/CodeAnalysisFormatter.h"
#include "serving/core/agent/code_analysis/CodeAnalysisHeuristics.h"
#include "serving/core/agent/code_analysis/CodeAnalysisPlanner.h"
#include "serving/http/ChatRequestParser.h"
#include "serving/http/HttpGateway.h"
#include "serving/http/http_types.h"
#include "serving/rag/vector/EmbeddingProvider.h"

#include "utils/json.hpp"

#include <glog/logging.h>
#include <sqlite3.h>

#include <cstdlib>
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
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define EXPECT_EQ(a, b)                                                                                     \
    do                                                                                                       \
    {                                                                                                        \
        if (!((a) == (b)))                                                                                   \
        {                                                                                                    \
            std::cerr << "EXPECT_EQ failed: " << #a << " vs " << #b << " at line " << __LINE__ << "\n"; \
            return false;                                                                                    \
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
    const auto path = std::filesystem::temp_directory_path() / ("edge_code_analysis_test_" + std::to_string(++seq));
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
        out.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(float)));
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

    const std::vector<TestChunkRow> rows = {
        {"c1", "repo_code", "serving/http/HttpGateway.cc", "HttpGateway.cc", "HandleChatCompletion", 10, 32, "cpp", "references are appended in non stream response for chat completion.", 12, "", "c2"},
        {"c2", "repo_code", "serving/http/OpenAIStreamWriter.cc", "OpenAIStreamWriter.cc", "OnChunk", 1, 50, "cpp", "stream metadata references are emitted from OpenAIStreamWriter::OnChunk before done.", 14, "", ""},
        {"c3", "docs", "README.md", "README.md", "RAG", 1, 20, "markdown", "README documents hybrid retrieval and structured agent output.", 10, "", ""},
    };

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

    EmbeddingProvider embedding_provider;
    std::vector<std::vector<float>> matrix;
    json id_map = {{"chunk_ids", json::array()}};
    std::ofstream chunks_out(artifacts.chunks_path);
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
    sqlite3_close(db);
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

bool test_question_type_classification()
{
    EXPECT_EQ(ToString(ClassifyCodeAnalysisQuestion("HttpGateway 和 SessionExecutor 的调用链是什么")), std::string("call_chain"));
    EXPECT_EQ(ToString(ClassifyCodeAnalysisQuestion("HttpGateway 模块职责是什么")), std::string("module_responsibility"));
    EXPECT_EQ(ToString(ClassifyCodeAnalysisQuestion("OpenAIStreamWriter::OnChunk 做什么")), std::string("symbol_behavior"));
    EXPECT_EQ(ToString(ClassifyCodeAnalysisQuestion("references 在哪里拼出来的")), std::string("location_lookup"));
    EXPECT_EQ(ToString(ClassifyCodeAnalysisQuestion("stream metadata 的配置和接口在哪")), std::string("config_interface"));
    return true;
}

bool test_evidence_dedup()
{
    CodeAnalysisEvidenceStore store;
    const std::string output =
        "Code matches for query: HandleChatCompletion\n"
        "- file=serving/http/HttpGateway.cc:10 score=2 text=void HandleChatCompletion()\n"
        "- file=serving/http/HttpGateway.cc:10 score=4 text=void HandleChatCompletion(HttpRequest&, HttpResponse&)\n";
    const size_t added = store.AddToolOutput("search_code", json::object(), output, "HandleChatCompletion 在哪", CodeAnalysisQuestionType::location_lookup);
    EXPECT_EQ(added, 1u);
    EXPECT_EQ(store.Size(), 1u);
    EXPECT_TRUE(store.All().front().score >= 4.0);
    return true;
}

bool test_primary_search_query_smart_pointer_topic()
{
    const std::string question = "结合仓库和外部网页资料，说明 EdgeLLM-Serving 的智能指针用法";
    const auto hints = ExtractCodeAnalysisHints(question);
    const auto query = BuildPrimarySearchQuery(question, hints);
    EXPECT_TRUE(query.find("shared_ptr") != std::string::npos);
    EXPECT_TRUE(query.find("unique_ptr") != std::string::npos);
    EXPECT_TRUE(query.find("weak_ptr") != std::string::npos);
    EXPECT_EQ(InferPreferredSearchPath(question, hints), std::string(""));
    return true;
}

bool test_planner_strategy_selection()
{
    CodeAnalysisPlanner planner("谁调用 HttpGateway::HandleChatCompletion", {"search_code", "read_file", "search_kb", "open_chunk"}, 4);
    CodeAnalysisEvidenceStore store;
    std::vector<AgentTraceStep> trace;

    const auto first = planner.NextStep(store, trace);
    EXPECT_EQ(first.tool_name, std::string("search_code"));

    AgentTraceStep search_trace;
    search_trace.step_index = 1;
    search_trace.selected_tool = "search_code";
    search_trace.tool_input = {{"query", "HttpGateway::HandleChatCompletion"}, {"limit", 8}};
    search_trace.tool_output_summary =
        "Code matches for query: HttpGateway::HandleChatCompletion\n"
        "- file=serving/http/HttpGateway.cc:642 score=2 text=void HttpGateway::HandleChatCompletion(const HttpRequest &req, HttpResponse &res)\n";
    trace.push_back(search_trace);

    const auto second = planner.NextStep(store, trace);
    EXPECT_EQ(second.tool_name, std::string("read_file"));
    EXPECT_EQ(second.tool_input.value("path", ""), std::string("serving/http/HttpGateway.cc"));
    return true;
}

bool test_structured_final_answer_formatting()
{
    std::vector<CodeEvidence> evidence = {
        {"search_code", "repo_code", "", "serving/http/HttpGateway.cc", "", "", 642, 642, "HttpGateway::HandleChatCompletion", "void HttpGateway::HandleChatCompletion(...)", "这里是 chat completion 非流式入口。", 4.0},
        {"read_file", "repo_code", "", "serving/http/HttpGateway.cc", "", "", 642, 700, "", "session_executor_.Submit(... agent_executor_->Run(ctx) ...)", "这里展示了请求如何进入 AgentExecutor。", 1.0},
    };
    std::vector<AgentTraceStep> trace = {
        {1, "search_code", {{"query", "HandleChatCompletion"}}, "Code matches ...", 1, "先粗召回"},
        {2, "read_file", {{"path", "serving/http/HttpGateway.cc"}}, "FILE ...", 2, "再精读"},
    };

    const auto answer = CodeAnalysisFormatter::Build("agent 请求如何进入 AgentExecutor", CodeAnalysisQuestionType::call_chain, evidence, trace);
    const auto out = ToJson(answer);
    EXPECT_TRUE(out["summary"].is_string());
    EXPECT_TRUE(out["analysis"].is_array());
    EXPECT_TRUE(out["evidence"].is_array());
    EXPECT_TRUE(!out["evidence"].empty());
    return true;
}

bool test_request_fields_and_backward_compat()
{
    SessionManager::Options opt;
    SessionManager session_mgr(opt);
    const std::string body = R"json({
        "model":"llama",
        "agent":true,
        "agent_mode":"code_analysis",
        "agent_debug":true,
        "agent_include_trace":true,
        "agent_output_format":"structured",
        "messages":[{"role":"user","content":"references 在哪里拼出来的"}]
    })json";
    auto parsed = ParseChatRequestBody(body, false, session_mgr, "llama", 128, "req-agent");
    EXPECT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.request.ctx->use_agent);
    EXPECT_TRUE(parsed.request.ctx->agent_debug);
    EXPECT_TRUE(parsed.request.ctx->agent_include_trace);
    EXPECT_EQ(parsed.request.ctx->agent_output_format, std::string("structured"));

    const std::string web_body = R"json({
        "model":"llama",
        "agent":true,
        "agent_mode":"web_research",
        "messages":[{"role":"user","content":"结合仓库和网页资料说明 references 输出"}]
    })json";
    auto web = ParseChatRequestBody(web_body, false, session_mgr, "llama", 128, "req-web");
    EXPECT_TRUE(web.ok);
    EXPECT_TRUE(web.request.ctx->use_agent);
    EXPECT_EQ(web.request.ctx->agent_mode, std::string("web_research"));

    const std::string plain_body = R"json({
        "model":"llama",
        "messages":[{"role":"user","content":"hello"}]
    })json";
    auto plain = ParseChatRequestBody(plain_body, false, session_mgr, "llama", 128, "req-plain");
    EXPECT_TRUE(plain.ok);
    EXPECT_TRUE(!plain.request.ctx->use_agent);
    EXPECT_TRUE(plain.request.ctx->agent_mode.empty());
    EXPECT_EQ(plain.request.ctx->agent_output_format, std::string("text"));
    return true;
}

bool test_web_research_evidence_formatting()
{
    CodeAnalysisEvidenceStore store;
    const std::string search_output =
        "Web search hits for query: OpenAI API docs\n"
        "- title=OpenAI API Platform url=https://openai.com/api/\n"
        "  snippet=Latest OpenAI API overview and safety guides.\n";
    const std::string fetch_output =
        "WEB_PAGE\n"
        "title=OpenAI API Platform\n"
        "canonical_url=https://openai.com/api/\n"
        "url=https://openai.com/api/\n"
        "text=OpenAI API platform provides models, docs, and guides.\n";

    EXPECT_EQ(store.AddToolOutput("search_web", json::object(), search_output, "OpenAI API docs", CodeAnalysisQuestionType::unknown), 1u);
    EXPECT_EQ(store.AddToolOutput("fetch_url", json::object(), fetch_output, "OpenAI API docs", CodeAnalysisQuestionType::unknown), 0u);

    std::vector<AgentTraceStep> trace = {
        {1, "search_web", {{"query", "OpenAI API docs"}}, search_output, 1, "先查网页搜索"},
        {2, "fetch_url", {{"url", "https://openai.com/api/"}}, fetch_output, 1, "再抓正文"},
    };
    CodeAnalysisSynthesis synthesis;
    synthesis.summary = "基于网页证据，已定位到 OpenAI API 平台主页。";
    synthesis.analysis = {"网页正文说明该站点提供模型、文档与使用指南。"};
    const auto answer = CodeAnalysisFormatter::BuildWebResearch("OpenAI API docs", store.Top(4), trace, &synthesis);
    const auto out = ToJson(answer);
    EXPECT_EQ(out["summary"].get<std::string>(), std::string("基于网页证据，已定位到 OpenAI API 平台主页。"));
    EXPECT_TRUE(out["analysis"].is_array());
    EXPECT_TRUE(out["evidence"].is_array());
    EXPECT_EQ(out["evidence"][0]["reference_source"].get<std::string>(), std::string("web"));
    EXPECT_EQ(out["evidence"][0]["url"].get<std::string>(), std::string("https://openai.com/api/"));
    return true;
}

bool test_gateway_structured_debug_response(const TestArtifacts &artifacts)
{
    setenv("RAG_INDEX_PATH", artifacts.sqlite_path.c_str(), 1);
    setenv("RAG_VECTOR_INDEX_PATH", artifacts.faiss_path.c_str(), 1);
    setenv("RAG_CHUNK_METADATA_PATH", artifacts.chunks_path.c_str(), 1);
    setenv("RAG_EMBEDDINGS_PATH", artifacts.embeddings_path.c_str(), 1);
    setenv("RAG_ID_MAP_PATH", artifacts.id_map_path.c_str(), 1);
    setenv("RAG_ENABLE_RETRIEVAL_DEBUG_API", "1", 1);

    HttpGateway gateway;
    FakeRequest req;
    req.body = R"json({
        "model":"llama",
        "agent":true,
        "agent_mode":"code_analysis",
        "agent_debug":true,
        "agent_output_format":"structured",
        "tools":["search_kb","open_chunk"],
        "messages":[{"role":"user","content":"stream metadata references 在哪里输出"}]
    })json";
    FakeResponse res;
    gateway.HandleChatCompletion(req, res);
    if (res.status != 200)
        std::cerr << "structured debug response status=" << res.status << " body=" << res.body << "\n";
    EXPECT_EQ(res.status, 200);

    const auto out = json::parse(res.body);
    EXPECT_TRUE(out.contains("agent_result"));
    EXPECT_TRUE(out.contains("evidence"));
    EXPECT_TRUE(out.contains("agent_trace"));
    EXPECT_TRUE(out["agent_trace"].is_array());
    EXPECT_TRUE(out["agent_trace"].size() >= 1);
    EXPECT_TRUE(out["evidence"].is_array());
    EXPECT_TRUE(!out["evidence"].empty());
    const auto content = out["choices"][0]["message"]["content"].get<std::string>();
    EXPECT_TRUE(content.find("\"summary\"") != std::string::npos);
    return true;
}

bool test_gateway_search_code_read_file_response()
{
    HttpGateway gateway;
    FakeRequest req;
    req.body = R"json({
        "model":"llama",
        "agent":true,
        "agent_mode":"code_analysis",
        "agent_debug":true,
        "tools":["search_code","read_file"],
        "messages":[{"role":"user","content":"HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的"}]
    })json";
    FakeResponse res;
    gateway.HandleChatCompletion(req, res);
    if (res.status != 200)
        std::cerr << "search_code/read_file response status=" << res.status << " body=" << res.body << "\n";
    EXPECT_EQ(res.status, 200);
    const auto out = json::parse(res.body);
    EXPECT_TRUE(out.contains("agent_result"));
    EXPECT_TRUE(out.contains("agent_trace"));
    EXPECT_TRUE(out["agent_trace"].size() >= 1);
    EXPECT_TRUE(out["choices"][0]["message"]["content"].get<std::string>().find("证据") != std::string::npos);
    return true;
}

bool test_agent_debug_endpoint()
{
    HttpGateway gateway;
    FakeRequest req;
    req.body = R"json({
        "model":"llama",
        "mode":"code_analysis",
        "debug":true,
        "tools":["search_code","read_file"],
        "query":"HttpGateway 里 agent 请求是怎么进入 AgentExecutor 的"
    })json";
    FakeResponse res;
    gateway.HandleAgentDebug(req, res);
    EXPECT_EQ(res.status, 200);
    const auto out = json::parse(res.body);
    EXPECT_TRUE(out.contains("planner_steps"));
    EXPECT_TRUE(out.contains("evidence"));
    EXPECT_TRUE(out.contains("final_answer"));
    EXPECT_TRUE(out["planner_steps"].is_array());
    return true;
}

bool test_agent_debug_web_research_endpoint_local_only()
{
    HttpGateway gateway;
    FakeRequest req;
    req.body = R"json({
        "model":"llama",
        "mode":"web_research",
        "debug":true,
        "tools":["search_code","read_file","search_docs"],
        "query":"HttpGateway 里 references 是怎么挂到响应里的"
    })json";
    FakeResponse res;
    gateway.HandleAgentDebug(req, res);
    EXPECT_EQ(res.status, 200);
    const auto out = json::parse(res.body);
    EXPECT_EQ(out["mode"].get<std::string>(), std::string("web_research"));
    EXPECT_TRUE(out.contains("references"));
    EXPECT_TRUE(out.contains("subqueries"));
    EXPECT_TRUE(out["planner_steps"].is_array());
    EXPECT_TRUE(out["evidence"].is_array());
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    google::InitGoogleLogging(argc > 0 ? argv[0] : "code_analysis_agent_test");

    const auto artifacts = create_test_artifacts();
    bool ok = true;
    ok = ok && test_question_type_classification();
    ok = ok && test_evidence_dedup();
    ok = ok && test_primary_search_query_smart_pointer_topic();
    ok = ok && test_planner_strategy_selection();
    ok = ok && test_structured_final_answer_formatting();
    ok = ok && test_request_fields_and_backward_compat();
    ok = ok && test_web_research_evidence_formatting();
    ok = ok && test_gateway_structured_debug_response(artifacts);
    ok = ok && test_gateway_search_code_read_file_response();
    ok = ok && test_agent_debug_endpoint();
    ok = ok && test_agent_debug_web_research_endpoint_local_only();

    std::error_code ec;
    std::filesystem::remove_all(artifacts.dir, ec);
    google::ShutdownGoogleLogging();
    if (!ok)
        return 1;
    std::cout << "code_analysis_agent_test passed\n";
    return 0;
}
