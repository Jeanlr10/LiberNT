/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NTFS directory crawler and live ReadDirectoryChangesW watcher interface
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <windef.h>
#include "ai_store.h"

typedef struct _AI_INDEXER AI_INDEXER;

BOOL AiIndexerStart(
    _In_ AI_STORE *Store,
    _Out_ AI_INDEXER **OutIndexer);

VOID AiIndexerStop(
    _In_opt_ AI_INDEXER *Indexer);

DWORD AiIndexerCrawlNow(
    _In_ AI_INDEXER *Indexer);

BOOL AiIndexerIndexSinglePath(
    _In_ AI_INDEXER *Indexer,
    _In_ PCWSTR Path);

BOOL AiIndexerIsBusy(
    _In_ AI_INDEXER *Indexer);

DWORD AiIndexerGetRootCount(
    _In_ AI_INDEXER *Indexer);
