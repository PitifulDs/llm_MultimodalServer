#include "serving/http/ChatRequestParser.h"
#include "serving/http/HttpUtils.h"
#include "serving/core/SessionManager.h"
#include "serving/rag/PromptAssembler.h"
#include "serving/rag/RAGExecutor.h"
#include "serving/rag/SqliteIndexStore.h"

#include <glog/logging.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
std::filesystem::path make_temp_db_path()
{
    static int seq = 0;
    return std::filesystem::temp_directory_path() / ("edge_rag_test_" + std::to_string(++seq) + ".sqlite");
}

bool exec_sql(sqlite3 *db, const char *sql)
{
    char *errmsg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (errmsg)
        sqlite3_free(errmsg);
    return rc == SQLITE_OK;
}

std::filesystem::path create_test_index()
{
    const auto db_path = make_temp_db_path();
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    sqlite3 *db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK)
        return {};

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
        "token_estimate INTEGER"
        ");"
        "CREATE VIRTUAL TABLE chunks_fts USING fts5("
        "chunk_id UNINDEXED, kb_name UNINDEXED, path, title, symbol, text"
        ");";
    if (!exec_sql(db, schema))
    {
        sqlite3_close(db);
        return {};
    }

    const char *insert_chunks =
        "INSERT INTO chunks VALUES "
        "('c1','repo_code','serving/http/HttpGateway.cc','serving/http/HttpGateway.cc','HttpGateway.cc','HandleChatCompletion',10,40,'cpp','RAG references are appended in non stream response.',12),"
        "('c2','docs','README.md','README.md','README','',1,12,'markdown','README describes how to build the rag index.',10);"
        "INSERT INTO chunks_fts VALUES "
        "('c1','repo_code','serving/http/HttpGateway.cc','HttpGateway.cc','HandleChatCompletion','RAG references are appended in non stream response.'),"
        "('c2','docs','README.md','README','','README describes how to build the rag index.');";
    if (!exec_sql(db, insert_chunks))
    {
        sqlite3_close(db);
        return {};
    }

    sqlite3_close(db);
    return db_path;
}
} // namespace

int main(int argc, char **argv)
{
    google::InitGoogleLogging(argc > 0 ? argv[0] : "rag_test");
    SessionManager::Options opt;
    SessionManager session_mgr(opt);

    const std::string body = R"json({
        "model":"llama",
        "messages":[{"role":"user","content":"How does rag work?"}],
        "rag":{"enabled":true,"kb":"repo_code","top_k":6,"mode":"lexical","return_references":true}
    })json";

    auto parsed = ParseChatRequestBody(body, false, session_mgr, "llama", 128, "req-test");
    EXPECT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.request.ctx->rag_options.enabled);
    EXPECT_EQ(parsed.request.ctx->rag_options.kb, std::string("repo_code"));
    EXPECT_EQ(parsed.request.ctx->rag_options.top_k, 6);
    EXPECT_EQ(parsed.request.ctx->rag_options.mode, std::string("lexical"));
    EXPECT_TRUE(parsed.request.ctx->rag_options.return_references);

    std::vector<RetrievalHit> prompt_hits;
    RetrievalHit hit;
    hit.chunk.kb_name = "repo_code";
    hit.chunk.path = "serving/http/HttpGateway.cc";
    hit.chunk.start_line = 10;
    hit.chunk.end_line = 40;
    hit.chunk.symbol = "HandleChatCompletion";
    hit.chunk.text = "RAG references are appended in non stream response.";
    hit.final_score = 12.3;
    prompt_hits.push_back(hit);

    PromptAssembler assembler(4096);
    auto assembled = assembler.Assemble({{"user", "Tell me about references"}}, prompt_hits);
    EXPECT_EQ(assembled.messages.size(), 2u);
    EXPECT_EQ(assembled.messages[0].role, std::string("system"));
    EXPECT_TRUE(assembled.messages[0].content.find("[1] kb=repo_code") != std::string::npos);
    EXPECT_TRUE(assembled.messages[0].content.find("HandleChatCompletion") != std::string::npos);

    auto refs = http_utils::build_rag_references(prompt_hits);
    EXPECT_TRUE(refs.is_array());
    EXPECT_EQ(refs.size(), 1u);
    EXPECT_EQ(refs[0]["kb"].get<std::string>(), std::string("repo_code"));
    EXPECT_EQ(refs[0]["path"].get<std::string>(), std::string("serving/http/HttpGateway.cc"));

    const auto db_path = create_test_index();
    EXPECT_TRUE(!db_path.empty());

    SqliteIndexStore store(db_path.string());
    std::vector<RetrievalHit> hits;
    std::string error;
    EXPECT_TRUE(store.Search("repo_code", "references", 5, hits, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].chunk.path, std::string("serving/http/HttpGateway.cc"));

    auto ctx = std::make_shared<ServingContext>();
    ctx->request_id = "req-rag";
    ctx->is_chat = true;
    ctx->session = session_mgr.getOrCreate("sid-rag", "llama", "");
    ctx->session->history = {{"user", "hi"}, {"assistant", "hello"}};
    const auto before_history = ctx->session->history;
    ctx->messages = {{"user", "references non stream response"}};
    ctx->rag_options.enabled = true;
    ctx->rag_options.kb = "repo_code";
    ctx->rag_options.mode = "lexical";
    ctx->rag_options.top_k = 3;
    ctx->rag_options.return_references = true;

    RAGExecutor::Options rag_opt;
    rag_opt.index_path = db_path.string();
    rag_opt.default_top_k = 6;
    rag_opt.max_context_chars = 2048;
    RAGExecutor rag_exec(rag_opt);

    EXPECT_TRUE(rag_exec.Apply(ctx));
    EXPECT_EQ(ctx->rag_hits.size(), 1u);
    EXPECT_EQ(ctx->messages.size(), 2u);
    EXPECT_EQ(ctx->messages[0].role, std::string("system"));
    EXPECT_EQ(ctx->session->history.size(), before_history.size());
    EXPECT_EQ(ctx->session->history[0].content, before_history[0].content);
    EXPECT_EQ(ctx->session->history[1].content, before_history[1].content);

    std::filesystem::remove(db_path);
    google::ShutdownGoogleLogging();
    std::cout << "rag_test passed\n";
    return 0;
}
