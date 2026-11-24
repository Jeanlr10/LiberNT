/*
 * PROJECT:     LiberNT AI Subsystem Tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host-native unit tests for SQLite FTS5 + vector semantic store and folder descriptions
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include "ai_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int Checks, Failures;

#define CHECK(cond)                                                               \
    do                                                                            \
    {                                                                             \
        Checks++;                                                                 \
        if (!(cond))                                                              \
        {                                                                         \
            Failures++;                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                         \
    } while (0)

int main(void)
{
    AI_STORE *Store = NULL;
    AI_STORE_ENTRY Entry;
    AI_STORE_STATS Stats;
    AI_SEARCH_HIT Hits[8];
    uint32_t Count = 0;
    char Desc[AI_MAX_DESC_UTF8];
    char Source[AI_MAX_SOURCE_UTF8];
    const float VecTax[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    const float VecKernel[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    const float QueryTax[4] = { 0.98f, 0.05f, 0.0f, 0.0f };

    CHECK(AiStoreOpen(":memory:", &Store) == AiStatusOk);
    CHECK(Store != NULL);

    memset(&Entry, 0, sizeof(Entry));
    Entry.Path = "C:\\Users\\Admin\\Documents\\Taxes2026";
    Entry.IsDir = 1;
    CHECK(AiStoreUpsertEntry(Store, &Entry) == AiStatusOk);

    CHECK(AiStoreSetFolderDescription(
              Store,
              "C:\\Users\\Admin\\Documents\\Taxes2026",
              "Quarterly estimated tax filings and W2 receipts",
              "user",
              "finance,taxes") == AiStatusOk);

    CHECK(AiStoreSetFolderDescription(
              Store,
              "C:\\Users\\Admin\\Documents\\Taxes2026",
              "AI guess that must not overwrite user text",
              "ai",
              "auto") == AiStatusOk);

    CHECK(AiStoreGetFolderDescription(
              Store,
              "C:\\Users\\Admin\\Documents\\Taxes2026",
              Desc,
              sizeof(Desc),
              Source,
              sizeof(Source)) == AiStatusOk);
    CHECK(strcmp(Desc, "Quarterly estimated tax filings and W2 receipts") == 0);
    CHECK(strcmp(Source, "user") == 0);

    memset(&Entry, 0, sizeof(Entry));
    Entry.Path = "C:\\Users\\Admin\\Documents\\Taxes2026\\q4_notes.md";
    Entry.IsDir = 0;
    Entry.Size = 128;
    Entry.Mtime = 1700000000;
    Entry.Content = "Remember to attach charitable donation receipts before April deadline.";
    CHECK(AiStoreUpsertEntry(Store, &Entry) == AiStatusOk);

    memset(&Entry, 0, sizeof(Entry));
    Entry.Path = "C:\\Users\\Admin\\Projects\\LiberNT\\sched.c";
    Entry.IsDir = 0;
    Entry.Size = 4096;
    Entry.Mtime = 1700000100;
    Entry.Content = "KiDeferredReadyThread acquires ThreadLock before placing threads on ready queues.";
    CHECK(AiStoreUpsertEntry(Store, &Entry) == AiStatusOk);

    CHECK(AiStoreSearch(Store, "estimated tax filings", NULL, 0, Hits, 8, &Count) == AiStatusOk);
    CHECK(Count >= 1);
    CHECK(Hits[0].IsDir == 1);
    CHECK(strcmp(Hits[0].Path, "C:\\Users\\Admin\\Documents\\Taxes2026") == 0);
    CHECK(strstr(Hits[0].Description, "Quarterly estimated tax") != NULL);

    CHECK(AiStoreSearch(Store, "KiDeferredReadyThread ThreadLock", NULL, 0, Hits, 8, &Count) == AiStatusOk);
    CHECK(Count == 1);
    CHECK(strcmp(Hits[0].Path, "C:\\Users\\Admin\\Projects\\LiberNT\\sched.c") == 0);

    CHECK(AiStoreSearch(Store, "C:\\weird-path/with:punctuation!!!", NULL, 0, Hits, 8, &Count) == AiStatusOk);

    CHECK(AiStoreUpsertEmbedding(Store, "C:\\Users\\Admin\\Documents\\Taxes2026\\q4_notes.md", "test-embed", VecTax, 4) == AiStatusOk);
    CHECK(AiStoreUpsertEmbedding(Store, "C:\\Users\\Admin\\Projects\\LiberNT\\sched.c", "test-embed", VecKernel, 4) == AiStatusOk);

    CHECK(AiStoreSearch(Store, "", QueryTax, 4, Hits, 8, &Count) == AiStatusOk);
    CHECK(Count == 1);
    CHECK(strcmp(Hits[0].Path, "C:\\Users\\Admin\\Documents\\Taxes2026\\q4_notes.md") == 0);

    CHECK(AiStoreLogAudit(Store, 1, "arrange_windows", "Notepad", "ok", "split_vertical") == AiStatusOk);

    CHECK(AiStoreGetStats(Store, &Stats) == AiStatusOk);
    CHECK(Stats.FileCount == 2);
    CHECK(Stats.DirCount == 1);
    CHECK(Stats.DescribedDirCount == 1);
    CHECK(Stats.EmbeddingCount == 2);
    CHECK(Stats.AuditCount == 1);

    CHECK(AiStoreClearFolderDescription(Store, "C:\\Users\\Admin\\Documents\\Taxes2026") == AiStatusOk);
    CHECK(AiStoreGetFolderDescription(
              Store,
              "C:\\Users\\Admin\\Documents\\Taxes2026",
              Desc,
              sizeof(Desc),
              Source,
              sizeof(Source)) == AiStatusNotFound);

    CHECK(AiStoreDeletePath(Store, "C:\\Users\\Admin\\Documents\\Taxes2026") == AiStatusOk);
    CHECK(AiStoreGetStats(Store, &Stats) == AiStatusOk);
    CHECK(Stats.FileCount == 1);
    CHECK(Stats.DirCount == 0);
    CHECK(Stats.EmbeddingCount == 1);

    AiStoreClose(Store);
    printf("t_store_sqlite: %d checks, %d failures\n", Checks, Failures);
    return Failures != 0;
}
