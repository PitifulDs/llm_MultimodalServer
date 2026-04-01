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

std::string text_col(sqlite3_stmt *stmt, int col)
{
    const unsigned char *text = sqlite3_column_text(stmt, col);
    if (!text)
        return {};
    return std::string(reinterpret_cast<const char *>(text));
}

void fill_chunk_from_stmt(sqlite3_stmt *stmt, RagChunk &chunk)
{
    chunk.chunk_id = text_col(stmt, 0);
    chunk.kb_name = text_col(stmt, 1);
    chunk.doc_id = text_col(stmt, 2);
    chunk.path = text_col(stmt, 3);
    chunk.title = text_col(stmt, 4);
    chunk.symbol = text_col(stmt, 5);
    chunk.start_line = sqlite3_column_int(stmt, 6);
    chunk.end_line = sqlite3_column_int(stmt, 7);
    chunk.language = text_col(stmt, 8);
    chunk.text = text_col(stmt, 9);
    chunk.token_estimate = sqlite3_column_int(stmt, 10);
    chunk.prev_chunk_id = text_col(stmt, 11);
    chunk.next_chunk_id = text_col(stmt, 12);
}

bool open_db_readonly(const std::string &path, sqlite3 **db_out, std::string &error_out)
{
    *db_out = nullptr;
    if (path.empty())
    {
        error_out = "RAG index path is empty";
        return false;
    }
    if (!std::filesystem::exists(path))
    {
        error_out = "RAG index not found: " + path;
        return false;
    }

    const int open_rc = sqlite3_open_v2(path.c_str(), db_out, SQLITE_OPEN_READONLY, nullptr);
    if (open_rc != SQLITE_OK || !*db_out)
    {
        const char *msg = *db_out ? sqlite3_errmsg(*db_out) : sqlite3_errstr(open_rc);
        error_out = "sqlite open failed: " + std::string(msg ? msg : "unknown");
        if (*db_out)
            sqlite3_close(*db_out);
        *db_out = nullptr;
        return false;
    }
    return true;
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

    if (query.empty())
    {
        return true;
    }

    sqlite3 *db = nullptr;
    if (!open_db_readonly(index_path_, &db, error_out))
        return false;

    static constexpr const char *kSql =
        "SELECT "
        "c.chunk_id, c.kb_name, c.doc_id, c.path, c.title, c.symbol, "
        "c.start_line, c.end_line, c.language, c.text, c.token_estimate, "
        "COALESCE(c.prev_chunk_id, ''), COALESCE(c.next_chunk_id, ''), "
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
        fill_chunk_from_stmt(stmt, hit.chunk);

        const double raw_score = sqlite3_column_double(stmt, 13);
        hit.lexical_score = sanitize_score(raw_score);
        hit.final_score = hit.lexical_score;
        hits_out.push_back(std::move(hit));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

bool SqliteIndexStore::GetChunkById(const std::string &chunk_id,
                                    RagChunk &chunk_out,
                                    std::string &error_out) const
{
    error_out.clear();
    sqlite3 *db = nullptr;
    if (!open_db_readonly(index_path_, &db, error_out))
        return false;

    static constexpr const char *kSql =
        "SELECT chunk_id, kb_name, doc_id, path, title, symbol, "
        "start_line, end_line, language, text, token_estimate, "
        "COALESCE(prev_chunk_id, ''), COALESCE(next_chunk_id, '') "
        "FROM chunks WHERE chunk_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        error_out = "sqlite prepare failed: " + std::string(sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, chunk_id.c_str(), -1, SQLITE_TRANSIENT);

    const int step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW)
    {
        fill_chunk_from_stmt(stmt, chunk_out);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return true;
    }

    if (step_rc != SQLITE_DONE)
        error_out = "sqlite query failed: " + std::string(sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return false;
}

int SqliteIndexStore::CountChunks(const std::string &kb, std::string &error_out) const
{
    error_out.clear();
    sqlite3 *db = nullptr;
    if (!open_db_readonly(index_path_, &db, error_out))
        return 0;

    const char *kSql = kb.empty()
                           ? "SELECT COUNT(*) FROM chunks;"
                           : "SELECT COUNT(*) FROM chunks WHERE kb_name = ?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        error_out = "sqlite prepare failed: " + std::string(sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    if (!kb.empty())
        sqlite3_bind_text(stmt, 1, kb.c_str(), -1, SQLITE_TRANSIENT);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    else
        error_out = "sqlite query failed: " + std::string(sqlite3_errmsg(db));

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}
