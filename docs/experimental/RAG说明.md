# RAG 说明（实验 / 兼容）

RAG 在当前仓库中属于“兼容保留 + 默认关闭”的能力：主线接口（chat/embeddings/rerank/models/healthz/admin status）不依赖它。

## 1. 如何开启

启用开关有两层（任意一种生效即可）：

- `config.json`：`experimental_rag_api_enabled: 1`
- 环境变量：`EXPERIMENTAL_RAG_API_ENABLED=1`

## 2. 对外接口（默认关闭）

- `POST /v1/retrieval/search`
- `GET /admin/rag/status`
- `POST /admin/rag/reload-index`

## 3. 关键配置（索引与默认策略）

RAG 的索引与默认策略主要来自 `config.json`：

- `rag_index_path`
- `rag_vector_index_path`
- `rag_chunk_metadata_path`
- `rag_embeddings_path`
- `rag_id_map_path`
- `rag_default_top_k`
- `rag_default_mode`
- `rag_default_fusion`
- `rag_enable_neighbor_expand`
- `rag_max_neighbor_count`
- `rag_enable_retrieval_debug_api`
- `rag_max_context_chars`

## 4. 阶段性记录

- v2 自测记录：[`../notes/rag_v2_self_test_2026-04-01.md`](../notes/rag_v2_self_test_2026-04-01.md)

