/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Portable SQLite FTS5 + embedding hybrid semantic index and audit store
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ai_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _AI_STORE AI_STORE;

typedef struct _AI_STORE_ENTRY
{
    const char *Path;
    const char *Name;
    uint32_t IsDir;
    uint64_t Size;
    int64_t Mtime;
    const char *DescText;
    const char *DescSource;
    const char *Tags;
    const char *Content;
} AI_STORE_ENTRY;

typedef struct _AI_STORE_STATS
{
    uint64_t FileCount;
    uint64_t DirCount;
    uint64_t DescribedDirCount;
    uint64_t EmbeddingCount;
    uint64_t AuditCount;
} AI_STORE_STATS;

int AiStoreOpen(const char *DbPath, AI_STORE **OutStore);
void AiStoreClose(AI_STORE *Store);

int AiStoreUpsertEntry(AI_STORE *Store, const AI_STORE_ENTRY *Entry);
int AiStoreSetFolderDescription(
    AI_STORE *Store,
    const char *Path,
    const char *DescText,
    const char *Source,
    const char *Tags);
int AiStoreGetFolderDescription(
    AI_STORE *Store,
    const char *Path,
    char *DescOut,
    size_t DescCap,
    char *SourceOut,
    size_t SourceCap);
int AiStoreClearFolderDescription(AI_STORE *Store, const char *Path);
int AiStoreDeletePath(AI_STORE *Store, const char *Path);

int AiStoreUpsertEmbedding(
    AI_STORE *Store,
    const char *Path,
    const char *Model,
    const float *Vec,
    uint32_t Dim);

float AiStoreCosineSimilarity(const float *A, const float *B, uint32_t Dim);

int AiStoreSearch(
    AI_STORE *Store,
    const char *Query,
    const float *QueryVec,
    uint32_t QueryDim,
    AI_SEARCH_HIT *Hits,
    uint32_t MaxHits,
    uint32_t *OutCount);

int AiStoreGetStats(AI_STORE *Store, AI_STORE_STATS *Stats);

int AiStoreLogAudit(
    AI_STORE *Store,
    uint32_t Tier,
    const char *Tool,
    const char *Target,
    const char *Status,
    const char *Detail);

#ifdef __cplusplus
}
#endif
