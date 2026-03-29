#include "serving/rag/SqliteIndexStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
double sanitize_score(double raw_bm25)
{
    return -raw_bm25;
}
} // namespace

SqliteIndexStore::SqliteIndexStore(std::string index_path)
    : index_path_(std::move(index_path))
{
}

bool SqliteIndexStore::Search(const std::string &kb,
                              const std::string &query,
                              int top_k,
                              std::vector<RetrievalHit> &hits_out,
                              std::string &error_out) const
{
    hits_out.clear();
    error_out.clear();

    if (index_path_.empty())
    {
        error_out = "RAG index path is empty";
        return false;
    }
    if (!std::filesystem::exists(index_path_))
    {
        error_out = "RAG index not found: " + index_path_;
        return false;
    }
    if (query.empty())
    {
        return true;
    }

    sqlite3 *db = nullptr;
    const int open_rc = sqlite3_open_v2(index_path_.c_str(),
                                        &db,
                                        SQLITE_OPEN_READONLY,
                                        nullptr);
    if (open_rc != SQLITE_OK || !db)
    {
        const char *msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(open_rc);
        error_out = "sqlite open failed: " + std::string(msg ? msg : "unknown");
        if (db)
            sqlite3_close(db);
        return false;
    }

    static constexpr const char *kSql =
        "SELECT "
        "c.chunk_id, c.kb_name, c.doc_id, c.path, c.title, c.symbol, "
        "c.start_line, c.end_line, c.language, c.text, c.token_estimate, "
        "bm25(chunks_fts) AS lexical_score "
        "FROM chunks_fts "
        "JOIN chunks c ON c.chunk_id = chunks_fts.chunk_id "
        "WHERE chunks_fts.kb_name = ? AND chunks_fts MATCH ? "
        "ORDER BY bm25(chunks_fts) "
        "LIMIT ?;";

    sqlite3_stmt *stmt = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr);
    if (prepare_rc != SQLITE_OK)
    {
        error_out = "sqlite prepare failed: " + std::string(sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, kb.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, std::max(1, top_k));

    while (true)
    {
        const int step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_DONE)
            break;
        if (step_rc != SQLITE_ROW)
        {
            error_out = "sqlite query failed: " + std::string(sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return false;
        }

        RetrievalHit hit;
        auto text_col = [](sqlite3_stmt *s, int col) -> std::string
        {
            const unsigned char *text = sqlite3_column_text(s, col);
            if (!text)
                return {};
            return std::string(reinterpret_cast<const char *>(text));
        };

        hit.chunk.chunk_id = text_col(stmt, 0);
        hit.chunk.kb_name = text_col(stmt, 1);
        hit.chunk.doc_id = text_col(stmt, 2);
        hit.chunk.path = text_col(stmt, 3);
        hit.chunk.title = text_col(stmt, 4);
        hit.chunk.symbol = text_col(stmt, 5);
        hit.chunk.start_line = sqlite3_column_int(stmt, 6);
        hit.chunk.end_line = sqlite3_column_int(stmt, 7);
        hit.chunk.language = text_col(stmt, 8);
        hit.chunk.text = text_col(stmt, 9);
        hit.chunk.token_estimate = sqlite3_column_int(stmt, 10);

        const double raw_score = sqlite3_column_double(stmt, 11);
        hit.lexical_score = sanitize_score(raw_score);
        hit.final_score = hit.lexical_score;
        hits_out.push_back(std::move(hit));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}
