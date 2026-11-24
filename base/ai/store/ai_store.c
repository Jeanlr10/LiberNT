/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SQLite FTS5 + float32 embedding hybrid semantic index implementation
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include "ai_store.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <windef.h>
#include <winbase.h>
#else
#include <pthread.h>
#endif

#include "sqlite3.h"

struct _AI_STORE
{
    sqlite3 *Db;
#ifdef _WIN32
    CRITICAL_SECTION Lock;
#else
    pthread_mutex_t Lock;
#endif
};

static void StoreLock(AI_STORE *Store)
{
#ifdef _WIN32
    EnterCriticalSection(&Store->Lock);
#else
    pthread_mutex_lock(&Store->Lock);
#endif
}

static void StoreUnlock(AI_STORE *Store)
{
#ifdef _WIN32
    LeaveCriticalSection(&Store->Lock);
#else
    pthread_mutex_unlock(&Store->Lock);
#endif
}

static void CopyStr(char *Dst, size_t Cap, const char *Src)
{
    size_t Len;

    if (Dst == NULL || Cap == 0)
        return;
    if (Src == NULL)
    {
        Dst[0] = '\0';
        return;
    }
    Len = strlen(Src);
    if (Len >= Cap)
        Len = Cap - 1;
    memcpy(Dst, Src, Len);
    Dst[Len] = '\0';
}

static const char *BaseNameOf(const char *Path)
{
    const char *Slash;
    const char *Back;
    const char *Last;

    if (Path == NULL)
        return "";
    Slash = strrchr(Path, '/');
    Back = strrchr(Path, '\\');
    Last = (Slash > Back) ? Slash : Back;
    return (Last != NULL && Last[1] != '\0') ? (Last + 1) : Path;
}

static void BuildFtsQuery(const char *Raw, char *Out, size_t Cap)
{
    size_t OutPos = 0;
    size_t TokenCount = 0;
    const unsigned char *P = (const unsigned char *)Raw;

    if (Out == NULL || Cap == 0)
        return;
    Out[0] = '\0';
    if (Raw == NULL)
        return;

    while (*P != '\0' && TokenCount < 16)
    {
        const unsigned char *Start;
        size_t Len;

        while (*P != '\0' && !isalnum(*P))
            P++;
        if (*P == '\0')
            break;
        Start = P;
        while (*P != '\0' && (isalnum(*P) || *P == '_'))
            P++;
        Len = (size_t)(P - Start);
        if (Len == 0)
            continue;
        if (Len > 48)
            Len = 48;

        if (TokenCount > 0)
        {
            if (OutPos + 4 >= Cap)
                break;
            memcpy(Out + OutPos, " OR ", 4);
            OutPos += 4;
        }
        if (OutPos + Len + 3 >= Cap)
            break;
        Out[OutPos++] = '"';
        memcpy(Out + OutPos, Start, Len);
        OutPos += Len;
        Out[OutPos++] = '"';
        Out[OutPos++] = '*';
        TokenCount++;
    }
    Out[OutPos] = '\0';
}

static const char *SchemaSql =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS entries ("
    "  path TEXT PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  is_dir INTEGER NOT NULL,"
    "  size INTEGER NOT NULL,"
    "  mtime INTEGER NOT NULL,"
    "  desc_text TEXT NOT NULL DEFAULT '',"
    "  desc_source TEXT NOT NULL DEFAULT '',"
    "  tags TEXT NOT NULL DEFAULT '',"
    "  content TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5("
    "  path, name, desc_text, tags, content,"
    "  content='entries', content_rowid='rowid',"
    "  tokenize='porter unicode61'"
    ");"
    "CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN"
    "  INSERT INTO entries_fts(rowid, path, name, desc_text, tags, content)"
    "  VALUES (new.rowid, new.path, new.name, new.desc_text, new.tags, new.content);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS entries_ad AFTER DELETE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, path, name, desc_text, tags, content)"
    "  VALUES ('delete', old.rowid, old.path, old.name, old.desc_text, old.tags, old.content);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS entries_au AFTER UPDATE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, path, name, desc_text, tags, content)"
    "  VALUES ('delete', old.rowid, old.path, old.name, old.desc_text, old.tags, old.content);"
    "  INSERT INTO entries_fts(rowid, path, name, desc_text, tags, content)"
    "  VALUES (new.rowid, new.path, new.name, new.desc_text, new.tags, new.content);"
    "END;"
    "CREATE TABLE IF NOT EXISTS embeddings ("
    "  path TEXT PRIMARY KEY,"
    "  model TEXT NOT NULL,"
    "  dim INTEGER NOT NULL,"
    "  vec BLOB NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS audit_log ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts_utc INTEGER NOT NULL,"
    "  tier INTEGER NOT NULL,"
    "  tool TEXT NOT NULL,"
    "  target TEXT NOT NULL,"
    "  status TEXT NOT NULL,"
    "  detail TEXT NOT NULL"
    ");";

int AiStoreOpen(const char *DbPath, AI_STORE **OutStore)
{
    AI_STORE *Store;
    int Rc;

    if (DbPath == NULL || OutStore == NULL)
        return AiStatusInvalidArg;

    *OutStore = NULL;
    Store = (AI_STORE *)calloc(1, sizeof(*Store));
    if (Store == NULL)
        return AiStatusInternal;

#ifdef _WIN32
    InitializeCriticalSection(&Store->Lock);
#else
    pthread_mutex_init(&Store->Lock, NULL);
#endif

    Rc = sqlite3_open_v2(
        DbPath,
        &Store->Db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (Rc != SQLITE_OK)
    {
        AiStoreClose(Store);
        return AiStatusDbError;
    }

    sqlite3_busy_timeout(Store->Db, 3000);
    Rc = sqlite3_exec(Store->Db, SchemaSql, NULL, NULL, NULL);
    if (Rc != SQLITE_OK)
    {
        AiStoreClose(Store);
        return AiStatusDbError;
    }

    *OutStore = Store;
    return AiStatusOk;
}

void AiStoreClose(AI_STORE *Store)
{
    if (Store == NULL)
        return;
    if (Store->Db != NULL)
        sqlite3_close(Store->Db);
#ifdef _WIN32
    DeleteCriticalSection(&Store->Lock);
#else
    pthread_mutex_destroy(&Store->Lock);
#endif
    free(Store);
}

int AiStoreUpsertEntry(AI_STORE *Store, const AI_STORE_ENTRY *Entry)
{
    static const char *Sql =
        "INSERT INTO entries(path, name, is_dir, size, mtime, desc_text, desc_source, tags, content)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
        " ON CONFLICT(path) DO UPDATE SET"
        "  name = excluded.name,"
        "  is_dir = excluded.is_dir,"
        "  size = excluded.size,"
        "  mtime = excluded.mtime,"
        "  desc_text = CASE WHEN excluded.desc_text != '' THEN excluded.desc_text ELSE entries.desc_text END,"
        "  desc_source = CASE WHEN excluded.desc_text != '' THEN excluded.desc_source ELSE entries.desc_source END,"
        "  tags = CASE WHEN excluded.tags != '' THEN excluded.tags ELSE entries.tags END,"
        "  content = excluded.content;";
    sqlite3_stmt *Stmt = NULL;
    const char *Name;
    int Rc;

    if (Store == NULL || Entry == NULL || Entry->Path == NULL || Entry->Path[0] == '\0')
        return AiStatusInvalidArg;

    Name = (Entry->Name != NULL && Entry->Name[0] != '\0') ? Entry->Name : BaseNameOf(Entry->Path);

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_text(Stmt, 1, Entry->Path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 2, Name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(Stmt, 3, Entry->IsDir ? 1 : 0);
        sqlite3_bind_int64(Stmt, 4, (sqlite3_int64)Entry->Size);
        sqlite3_bind_int64(Stmt, 5, (sqlite3_int64)Entry->Mtime);
        sqlite3_bind_text(Stmt, 6, Entry->DescText ? Entry->DescText : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 7, Entry->DescSource ? Entry->DescSource : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 8, Entry->Tags ? Entry->Tags : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 9, Entry->Content ? Entry->Content : "", -1, SQLITE_TRANSIENT);
        Rc = sqlite3_step(Stmt);
        sqlite3_finalize(Stmt);
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_DONE) ? AiStatusOk : AiStatusDbError;
}

int AiStoreSetFolderDescription(
    AI_STORE *Store,
    const char *Path,
    const char *DescText,
    const char *Source,
    const char *Tags)
{
    static const char *Sql =
        "INSERT INTO entries(path, name, is_dir, size, mtime, desc_text, desc_source, tags, content)"
        " VALUES(?1, ?2, 1, 0, ?3, ?4, ?5, ?6, '')"
        " ON CONFLICT(path) DO UPDATE SET"
        "  is_dir = 1,"
        "  desc_text = CASE WHEN entries.desc_source = 'user' AND excluded.desc_source = 'ai' THEN entries.desc_text ELSE excluded.desc_text END,"
        "  desc_source = CASE WHEN entries.desc_source = 'user' AND excluded.desc_source = 'ai' THEN entries.desc_source ELSE excluded.desc_source END,"
        "  tags = CASE WHEN entries.desc_source = 'user' AND excluded.desc_source = 'ai' THEN entries.tags ELSE excluded.tags END;";
    sqlite3_stmt *Stmt = NULL;
    int Rc;

    if (Store == NULL || Path == NULL || Path[0] == '\0' || DescText == NULL)
        return AiStatusInvalidArg;

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_text(Stmt, 1, Path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 2, BaseNameOf(Path), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(Stmt, 3, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(Stmt, 4, DescText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 5, (Source && Source[0]) ? Source : "user", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 6, Tags ? Tags : "", -1, SQLITE_TRANSIENT);
        Rc = sqlite3_step(Stmt);
        sqlite3_finalize(Stmt);
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_DONE) ? AiStatusOk : AiStatusDbError;
}

int AiStoreGetFolderDescription(
    AI_STORE *Store,
    const char *Path,
    char *DescOut,
    size_t DescCap,
    char *SourceOut,
    size_t SourceCap)
{
    static const char *Sql = "SELECT desc_text, desc_source FROM entries WHERE path = ?1;";
    sqlite3_stmt *Stmt = NULL;
    int Status = AiStatusNotFound;
    int Rc;

    if (Store == NULL || Path == NULL || Path[0] == '\0')
        return AiStatusInvalidArg;

    CopyStr(DescOut, DescCap, "");
    CopyStr(SourceOut, SourceCap, "");

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_text(Stmt, 1, Path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(Stmt) == SQLITE_ROW)
        {
            const char *Desc = (const char *)sqlite3_column_text(Stmt, 0);
            const char *Src = (const char *)sqlite3_column_text(Stmt, 1);

            if (Desc != NULL && Desc[0] != '\0')
            {
                CopyStr(DescOut, DescCap, Desc);
                CopyStr(SourceOut, SourceCap, Src ? Src : "");
                Status = AiStatusOk;
            }
        }
        sqlite3_finalize(Stmt);
    }
    else
    {
        Status = AiStatusDbError;
    }
    StoreUnlock(Store);

    return Status;
}

int AiStoreClearFolderDescription(AI_STORE *Store, const char *Path)
{
    static const char *Sql = "UPDATE entries SET desc_text = '', desc_source = '', tags = '' WHERE path = ?1;";
    sqlite3_stmt *Stmt = NULL;
    int Rc;

    if (Store == NULL || Path == NULL || Path[0] == '\0')
        return AiStatusInvalidArg;

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_text(Stmt, 1, Path, -1, SQLITE_TRANSIENT);
        Rc = sqlite3_step(Stmt);
        sqlite3_finalize(Stmt);
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_DONE) ? AiStatusOk : AiStatusDbError;
}

int AiStoreDeletePath(AI_STORE *Store, const char *Path)
{
    static const char *SqlEntries =
        "DELETE FROM entries WHERE path = ?1 OR path LIKE ?2 OR path LIKE ?3;";
    static const char *SqlEmbeds =
        "DELETE FROM embeddings WHERE path = ?1 OR path LIKE ?2 OR path LIKE ?3;";
    char PrefixBack[AI_MAX_PATH_UTF8 + 4];
    char PrefixSlash[AI_MAX_PATH_UTF8 + 4];
    sqlite3_stmt *Stmt = NULL;
    int Rc;

    if (Store == NULL || Path == NULL || Path[0] == '\0')
        return AiStatusInvalidArg;

    snprintf(PrefixBack, sizeof(PrefixBack), "%s\\%%", Path);
    snprintf(PrefixSlash, sizeof(PrefixSlash), "%s/%%", Path);

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, SqlEntries, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_text(Stmt, 1, Path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 2, PrefixBack, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 3, PrefixSlash, -1, SQLITE_TRANSIENT);
        Rc = sqlite3_step(Stmt);
        sqlite3_finalize(Stmt);
    }
    if (Rc == SQLITE_DONE)
    {
        Rc = sqlite3_prepare_v2(Store->Db, SqlEmbeds, -1, &Stmt, NULL);
        if (Rc == SQLITE_OK)
        {
            sqlite3_bind_text(Stmt, 1, Path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(Stmt, 2, PrefixBack, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(Stmt, 3, PrefixSlash, -1, SQLITE_TRANSIENT);
            Rc = sqlite3_step(Stmt);
            sqlite3_finalize(Stmt);
        }
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_DONE) ? AiStatusOk : AiStatusDbError;
}

int AiStoreUpsertEmbedding(
    AI_STORE *Store,
    const char *Path,
    const char *Model,
    const float *Vec,
    uint32_t Dim)
{
    static const char *Sql =
        "INSERT INTO embeddings(path, model, dim, vec) VALUES(?1, ?2, ?3, ?4)"
        " ON CONFLICT(path) DO UPDATE SET model = excluded.model, dim = excluded.dim, vec = excluded.vec;";
    sqlite3_stmt *Stmt = NULL;
    int Rc;

    if (Store == NULL || Path == NULL || Model == NULL || Vec == NULL || Dim == 0 || Dim > 4096)
        return AiStatusInvalidArg;

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_text(Stmt, 1, Path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 2, Model, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(Stmt, 3, (int)Dim);
        sqlite3_bind_blob(Stmt, 4, Vec, (int)(Dim * sizeof(float)), SQLITE_TRANSIENT);
        Rc = sqlite3_step(Stmt);
        sqlite3_finalize(Stmt);
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_DONE) ? AiStatusOk : AiStatusDbError;
}

float AiStoreCosineSimilarity(const float *A, const float *B, uint32_t Dim)
{
    double Dot = 0.0;
    double NormA = 0.0;
    double NormB = 0.0;
    double Denom;
    uint32_t i;

    if (A == NULL || B == NULL || Dim == 0)
        return 0.0f;

    for (i = 0; i < Dim; i++)
    {
        Dot += (double)A[i] * (double)B[i];
        NormA += (double)A[i] * (double)A[i];
        NormB += (double)B[i] * (double)B[i];
    }

    Denom = sqrt(NormA) * sqrt(NormB);
    if (Denom <= 1e-12)
        return 0.0f;

    return (float)(Dot / Denom);
}

typedef struct _AI_CANDIDATE
{
    AI_SEARCH_HIT Hit;
    double RrfScore;
} AI_CANDIDATE;

static int CompareCandidates(const void *A, const void *B)
{
    const AI_CANDIDATE *Left = (const AI_CANDIDATE *)A;
    const AI_CANDIDATE *Right = (const AI_CANDIDATE *)B;

    if (Right->RrfScore > Left->RrfScore)
        return 1;
    if (Right->RrfScore < Left->RrfScore)
        return -1;
    return strcmp(Left->Hit.Path, Right->Hit.Path);
}

static AI_CANDIDATE *FindOrAddCandidate(
    AI_CANDIDATE *List,
    uint32_t *Count,
    uint32_t Cap,
    const char *Path)
{
    uint32_t i;

    for (i = 0; i < *Count; i++)
    {
        if (strcmp(List[i].Hit.Path, Path) == 0)
            return &List[i];
    }
    if (*Count >= Cap)
        return NULL;

    memset(&List[*Count], 0, sizeof(List[*Count]));
    CopyStr(List[*Count].Hit.Path, sizeof(List[*Count].Hit.Path), Path);
    return &List[(*Count)++];
}

int AiStoreSearch(
    AI_STORE *Store,
    const char *Query,
    const float *QueryVec,
    uint32_t QueryDim,
    AI_SEARCH_HIT *Hits,
    uint32_t MaxHits,
    uint32_t *OutCount)
{
    static const char *FtsSql =
        "SELECT e.path, e.is_dir, e.size, e.mtime, e.desc_text,"
        " snippet(entries_fts, 4, '[', ']', '...', 16),"
        " bm25(entries_fts, 2.0, 6.0, 10.0, 5.0, 1.0) AS rank"
        " FROM entries_fts JOIN entries e ON e.rowid = entries_fts.rowid"
        " WHERE entries_fts MATCH ?1"
        " ORDER BY rank LIMIT 32;";
    static const char *VecSql =
        "SELECT m.path, m.dim, m.vec, e.is_dir, e.size, e.mtime, e.desc_text, e.content"
        " FROM embeddings m JOIN entries e ON e.path = m.path"
        " WHERE m.dim = ?1;";
    char FtsExpr[512];
    AI_CANDIDATE Candidates[64];
    uint32_t CandCount = 0;
    sqlite3_stmt *Stmt = NULL;
    uint32_t Rank = 0;
    uint32_t Limit;
    uint32_t i;

    if (Store == NULL || Hits == NULL || OutCount == NULL || MaxHits == 0)
        return AiStatusInvalidArg;

    *OutCount = 0;
    BuildFtsQuery(Query, FtsExpr, sizeof(FtsExpr));

    StoreLock(Store);

    if (FtsExpr[0] != '\0')
    {
        if (sqlite3_prepare_v2(Store->Db, FtsSql, -1, &Stmt, NULL) == SQLITE_OK)
        {
            sqlite3_bind_text(Stmt, 1, FtsExpr, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(Stmt) == SQLITE_ROW)
            {
                const char *Path = (const char *)sqlite3_column_text(Stmt, 0);
                const char *Desc = (const char *)sqlite3_column_text(Stmt, 4);
                const char *Snip = (const char *)sqlite3_column_text(Stmt, 5);
                AI_CANDIDATE *Cand;

                if (Path == NULL)
                    continue;
                Cand = FindOrAddCandidate(Candidates, &CandCount, 64, Path);
                if (Cand == NULL)
                    break;
                Cand->Hit.IsDir = (uint32_t)sqlite3_column_int(Stmt, 1);
                Cand->Hit.Size = (uint64_t)sqlite3_column_int64(Stmt, 2);
                Cand->Hit.Mtime = (int64_t)sqlite3_column_int64(Stmt, 3);
                CopyStr(Cand->Hit.Description, sizeof(Cand->Hit.Description), Desc ? Desc : "");
                CopyStr(Cand->Hit.Snippet, sizeof(Cand->Hit.Snippet), Snip ? Snip : "");
                Cand->RrfScore += 1.0 / (60.0 + (double)Rank);
                if (Cand->Hit.IsDir && Cand->Hit.Description[0] != '\0')
                    Cand->RrfScore += 0.005;
                Rank++;
            }
            sqlite3_finalize(Stmt);
        }
    }

    if (QueryVec != NULL && QueryDim > 0)
    {
        if (sqlite3_prepare_v2(Store->Db, VecSql, -1, &Stmt, NULL) == SQLITE_OK)
        {
            sqlite3_bind_int(Stmt, 1, (int)QueryDim);
            while (sqlite3_step(Stmt) == SQLITE_ROW)
            {
                const char *Path = (const char *)sqlite3_column_text(Stmt, 0);
                const void *Blob = sqlite3_column_blob(Stmt, 2);
                int BlobBytes = sqlite3_column_bytes(Stmt, 2);
                float Sim;
                AI_CANDIDATE *Cand;

                if (Path == NULL || Blob == NULL || BlobBytes != (int)(QueryDim * sizeof(float)))
                    continue;

                Sim = AiStoreCosineSimilarity(QueryVec, (const float *)Blob, QueryDim);
                if (Sim <= 0.15f)
                    continue;

                Cand = FindOrAddCandidate(Candidates, &CandCount, 64, Path);
                if (Cand == NULL)
                    continue;
                Cand->Hit.IsDir = (uint32_t)sqlite3_column_int(Stmt, 3);
                Cand->Hit.Size = (uint64_t)sqlite3_column_int64(Stmt, 4);
                Cand->Hit.Mtime = (int64_t)sqlite3_column_int64(Stmt, 5);
                if (Cand->Hit.Description[0] == '\0')
                {
                    const char *Desc = (const char *)sqlite3_column_text(Stmt, 6);
                    CopyStr(Cand->Hit.Description, sizeof(Cand->Hit.Description), Desc ? Desc : "");
                }
                if (Cand->Hit.Snippet[0] == '\0')
                {
                    const char *Content = (const char *)sqlite3_column_text(Stmt, 7);
                    CopyStr(Cand->Hit.Snippet, sizeof(Cand->Hit.Snippet), Content ? Content : "");
                }
                Cand->RrfScore += (double)Sim * 0.05;
            }
            sqlite3_finalize(Stmt);
        }
    }

    StoreUnlock(Store);

    if (CandCount > 1)
        qsort(Candidates, CandCount, sizeof(Candidates[0]), CompareCandidates);

    Limit = (CandCount < MaxHits) ? CandCount : MaxHits;
    for (i = 0; i < Limit; i++)
    {
        Hits[i] = Candidates[i].Hit;
        Hits[i].Score = (float)Candidates[i].RrfScore;
    }
    *OutCount = Limit;
    return AiStatusOk;
}

int AiStoreGetStats(AI_STORE *Store, AI_STORE_STATS *Stats)
{
    static const char *Sql =
        "SELECT"
        " (SELECT COUNT(*) FROM entries WHERE is_dir = 0),"
        " (SELECT COUNT(*) FROM entries WHERE is_dir = 1),"
        " (SELECT COUNT(*) FROM entries WHERE is_dir = 1 AND desc_text != ''),"
        " (SELECT COUNT(*) FROM embeddings),"
        " (SELECT COUNT(*) FROM audit_log);";
    sqlite3_stmt *Stmt = NULL;
    int Rc;

    if (Store == NULL || Stats == NULL)
        return AiStatusInvalidArg;

    memset(Stats, 0, sizeof(*Stats));
    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        if (sqlite3_step(Stmt) == SQLITE_ROW)
        {
            Stats->FileCount = (uint64_t)sqlite3_column_int64(Stmt, 0);
            Stats->DirCount = (uint64_t)sqlite3_column_int64(Stmt, 1);
            Stats->DescribedDirCount = (uint64_t)sqlite3_column_int64(Stmt, 2);
            Stats->EmbeddingCount = (uint64_t)sqlite3_column_int64(Stmt, 3);
            Stats->AuditCount = (uint64_t)sqlite3_column_int64(Stmt, 4);
        }
        sqlite3_finalize(Stmt);
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_OK) ? AiStatusOk : AiStatusDbError;
}

int AiStoreLogAudit(
    AI_STORE *Store,
    uint32_t Tier,
    const char *Tool,
    const char *Target,
    const char *Status,
    const char *Detail)
{
    static const char *Sql =
        "INSERT INTO audit_log(ts_utc, tier, tool, target, status, detail)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6);";
    sqlite3_stmt *Stmt = NULL;
    int Rc;

    if (Store == NULL || Tool == NULL || Status == NULL)
        return AiStatusInvalidArg;

    StoreLock(Store);
    Rc = sqlite3_prepare_v2(Store->Db, Sql, -1, &Stmt, NULL);
    if (Rc == SQLITE_OK)
    {
        sqlite3_bind_int64(Stmt, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_int(Stmt, 2, (int)Tier);
        sqlite3_bind_text(Stmt, 3, Tool, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 4, Target ? Target : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 5, Status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(Stmt, 6, Detail ? Detail : "", -1, SQLITE_TRANSIENT);
        Rc = sqlite3_step(Stmt);
        sqlite3_finalize(Stmt);
    }
    StoreUnlock(Store);

    return (Rc == SQLITE_DONE) ? AiStatusOk : AiStatusDbError;
}
