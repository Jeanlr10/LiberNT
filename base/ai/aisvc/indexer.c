/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NTFS crawler and live ReadDirectoryChangesW watcher with folder ADS descriptions
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>

#include "ai_protocol.h"
#include "ai_store.h"
#include "libai.h"
#include "indexer.h"

#define AI_MAX_INDEX_ROOTS      8U
#define AI_MAX_TEXT_BYTES       65536U
#define AI_MAX_DEPTH            16U
#define AI_WATCH_BUF_BYTES      16384U

typedef struct _AI_WATCH_CTX
{
    AI_INDEXER *Indexer;
    WCHAR RootPath[MAX_PATH];
    HANDLE DirHandle;
    HANDLE Thread;
} AI_WATCH_CTX;

struct _AI_INDEXER
{
    AI_STORE *Store;
    HANDLE StopEvent;
    HANDLE CrawlThread;
    volatile LONG Busy;
    DWORD RootCount;
    AI_WATCH_CTX Watchers[AI_MAX_INDEX_ROOTS];
};

static BOOL WideToUtf8(PCWSTR Wide, PSTR Utf8, DWORD Cap)
{
    int Written;

    if (Wide == NULL || Utf8 == NULL || Cap == 0)
        return FALSE;
    Written = WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Utf8, (int)Cap, NULL, NULL);
    if (Written <= 0)
    {
        Utf8[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static int64_t FileTimeToUnixSeconds(const FILETIME *Ft)
{
    ULARGE_INTEGER Ul;

    if (Ft == NULL)
        return 0;
    Ul.LowPart = Ft->dwLowDateTime;
    Ul.HighPart = Ft->dwHighDateTime;
    if (Ul.QuadPart < 116444736000000000ULL)
        return 0;
    return (int64_t)((Ul.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

static BOOL IsTextExtension(PCWSTR FileName)
{
    static const PCWSTR TextExts[] = {
        L".txt", L".md", L".markdown", L".c", L".h", L".cpp", L".hpp", L".cc",
        L".py", L".rs", L".go", L".js", L".ts", L".json", L".xml", L".yaml",
        L".yml", L".toml", L".ini", L".cfg", L".conf", L".inf", L".log",
        L".csv", L".cmd", L".bat", L".ps1", L".sh", L".cmake", L".html", L".css"
    };
    PCWSTR Dot;
    SIZE_T i;

    if (FileName == NULL)
        return FALSE;
    Dot = wcsrchr(FileName, L'.');
    if (Dot == NULL)
        return FALSE;
    for (i = 0; i < ARRAYSIZE(TextExts); i++)
    {
        if (_wcsicmp(Dot, TextExts[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static BOOL ShouldSkipDirectoryName(PCWSTR Name)
{
    static const PCWSTR SkipNames[] = {
        L".", L"..", L".git", L"node_modules", L"__pycache__",
        L"Temp", L"Temporary Internet Files", L"Cache", L"AppData"
    };
    SIZE_T i;

    if (Name == NULL)
        return TRUE;
    for (i = 0; i < ARRAYSIZE(SkipNames); i++)
    {
        if (_wcsicmp(Name, SkipNames[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static PSTR ReadTextPreview(PCWSTR FilePath)
{
    HANDLE File;
    LARGE_INTEGER FileSize;
    DWORD ToRead;
    DWORD ReadBytes = 0;
    PSTR Buffer;
    DWORD i;

    File = CreateFileW(
        FilePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (File == INVALID_HANDLE_VALUE)
        return NULL;

    if (!GetFileSizeEx(File, &FileSize) || FileSize.QuadPart <= 0)
    {
        CloseHandle(File);
        return NULL;
    }

    ToRead = (FileSize.QuadPart > AI_MAX_TEXT_BYTES) ? AI_MAX_TEXT_BYTES : (DWORD)FileSize.QuadPart;
    Buffer = (PSTR)malloc(ToRead + 1);
    if (Buffer == NULL)
    {
        CloseHandle(File);
        return NULL;
    }

    if (!ReadFile(File, Buffer, ToRead, &ReadBytes, NULL))
    {
        free(Buffer);
        CloseHandle(File);
        return NULL;
    }
    CloseHandle(File);

    for (i = 0; i < ReadBytes; i++)
    {
        if (Buffer[i] == '\0')
        {
            free(Buffer);
            return NULL;
        }
    }
    Buffer[ReadBytes] = '\0';
    return Buffer;
}

static BOOL IndexPathInternal(AI_STORE *Store, PCWSTR Path, const WIN32_FILE_ATTRIBUTE_DATA *OptAttrs)
{
    WIN32_FILE_ATTRIBUTE_DATA Attrs;
    CHAR PathUtf8[AI_MAX_PATH_UTF8];
    CHAR DescUtf8[AI_MAX_DESC_UTF8];
    CHAR SourceUtf8[AI_MAX_SOURCE_UTF8];
    CHAR TagsUtf8[AI_MAX_TAGS_UTF8];
    PSTR Content = NULL;
    AI_STORE_ENTRY Entry;
    BOOL IsDir;
    int Rc;

    if (Store == NULL || Path == NULL || Path[0] == UNICODE_NULL)
        return FALSE;

    if (OptAttrs != NULL)
    {
        Attrs = *OptAttrs;
    }
    else if (!GetFileAttributesExW(Path, GetFileExInfoStandard, &Attrs))
    {
        if (WideToUtf8(Path, PathUtf8, sizeof(PathUtf8)))
            AiStoreDeletePath(Store, PathUtf8);
        return FALSE;
    }

    if (!WideToUtf8(Path, PathUtf8, sizeof(PathUtf8)))
        return FALSE;

    IsDir = (Attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    DescUtf8[0] = '\0';
    SourceUtf8[0] = '\0';
    TagsUtf8[0] = '\0';

    if (IsDir)
    {
        AiAdsReadFolderDescription(
            Path,
            DescUtf8,
            sizeof(DescUtf8),
            SourceUtf8,
            sizeof(SourceUtf8),
            TagsUtf8,
            sizeof(TagsUtf8));
    }
    else if (IsTextExtension(Path))
    {
        Content = ReadTextPreview(Path);
    }

    memset(&Entry, 0, sizeof(Entry));
    Entry.Path = PathUtf8;
    Entry.IsDir = IsDir ? 1U : 0U;
    Entry.Size = IsDir ? 0ULL : (((uint64_t)Attrs.nFileSizeHigh << 32) | Attrs.nFileSizeLow);
    Entry.Mtime = FileTimeToUnixSeconds(&Attrs.ftLastWriteTime);
    Entry.DescText = DescUtf8;
    Entry.DescSource = SourceUtf8;
    Entry.Tags = TagsUtf8;
    Entry.Content = Content;

    Rc = AiStoreUpsertEntry(Store, &Entry);
    if (Content != NULL)
        free(Content);
    return Rc == AiStatusOk;
}

static DWORD CrawlRecursive(AI_INDEXER *Indexer, PCWSTR DirPath, DWORD Depth)
{
    WCHAR Pattern[MAX_PATH];
    WCHAR ChildPath[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE FindHandle;
    DWORD Indexed = 0;

    if (Depth > AI_MAX_DEPTH)
        return 0;
    if (WaitForSingleObject(Indexer->StopEvent, 0) == WAIT_OBJECT_0)
        return 0;

    if (IndexPathInternal(Indexer->Store, DirPath, NULL))
        Indexed++;

    if (_snwprintf(Pattern, ARRAYSIZE(Pattern), L"%ls\\*", DirPath) < 0)
        return Indexed;
    Pattern[ARRAYSIZE(Pattern) - 1] = UNICODE_NULL;

    FindHandle = FindFirstFileW(Pattern, &FindData);
    if (FindHandle == INVALID_HANDLE_VALUE)
        return Indexed;

    do
    {
        BOOL IsDir;

        if (WaitForSingleObject(Indexer->StopEvent, 0) == WAIT_OBJECT_0)
            break;

        IsDir = (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (IsDir && ShouldSkipDirectoryName(FindData.cFileName))
            continue;
        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;

        if (_snwprintf(ChildPath, ARRAYSIZE(ChildPath), L"%ls\\%ls", DirPath, FindData.cFileName) < 0)
            continue;
        ChildPath[ARRAYSIZE(ChildPath) - 1] = UNICODE_NULL;

        if (IsDir)
        {
            Indexed += CrawlRecursive(Indexer, ChildPath, Depth + 1);
        }
        else
        {
            WIN32_FILE_ATTRIBUTE_DATA Attrs;

            Attrs.dwFileAttributes = FindData.dwFileAttributes;
            Attrs.ftCreationTime = FindData.ftCreationTime;
            Attrs.ftLastAccessTime = FindData.ftLastAccessTime;
            Attrs.ftLastWriteTime = FindData.ftLastWriteTime;
            Attrs.nFileSizeHigh = FindData.nFileSizeHigh;
            Attrs.nFileSizeLow = FindData.nFileSizeLow;
            if (IndexPathInternal(Indexer->Store, ChildPath, &Attrs))
                Indexed++;
        }
    } while (FindNextFileW(FindHandle, &FindData));

    FindClose(FindHandle);
    return Indexed;
}

static VOID ProcessNotifyBuffer(AI_WATCH_CTX *Ctx, const BYTE *Buf, DWORD Bytes)
{
    DWORD Offset = 0;

    while (Offset < Bytes)
    {
        const FILE_NOTIFY_INFORMATION *Info = (const FILE_NOTIFY_INFORMATION *)(Buf + Offset);
        WCHAR RelName[MAX_PATH];
        WCHAR FullPath[MAX_PATH];
        DWORD NameChars = Info->FileNameLength / sizeof(WCHAR);

        if (NameChars > 0 && NameChars < ARRAYSIZE(RelName))
        {
            memcpy(RelName, Info->FileName, NameChars * sizeof(WCHAR));
            RelName[NameChars] = UNICODE_NULL;

            if (_snwprintf(FullPath, ARRAYSIZE(FullPath), L"%ls\\%ls", Ctx->RootPath, RelName) > 0)
            {
                FullPath[ARRAYSIZE(FullPath) - 1] = UNICODE_NULL;
                if (Info->Action == FILE_ACTION_REMOVED || Info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                {
                    CHAR PathUtf8[AI_MAX_PATH_UTF8];
                    if (WideToUtf8(FullPath, PathUtf8, sizeof(PathUtf8)))
                        AiStoreDeletePath(Ctx->Indexer->Store, PathUtf8);
                }
                else
                {
                    IndexPathInternal(Ctx->Indexer->Store, FullPath, NULL);
                }
            }
        }

        if (Info->NextEntryOffset == 0)
            break;
        Offset += Info->NextEntryOffset;
    }
}

static DWORD WINAPI WatchRootThread(PVOID Param)
{
    AI_WATCH_CTX *Ctx = (AI_WATCH_CTX *)Param;
    BYTE *Buffer;
    OVERLAPPED Ov;
    HANDLE WaitHandles[2];

    Buffer = (BYTE *)malloc(AI_WATCH_BUF_BYTES);
    if (Buffer == NULL)
        return 0;

    ZeroMemory(&Ov, sizeof(Ov));
    Ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Ov.hEvent == NULL)
    {
        free(Buffer);
        return 0;
    }

    WaitHandles[0] = Ctx->Indexer->StopEvent;
    WaitHandles[1] = Ov.hEvent;

    while (WaitForSingleObject(Ctx->Indexer->StopEvent, 0) != WAIT_OBJECT_0)
    {
        DWORD BytesReturned = 0;
        DWORD WaitRes;
        BOOL Issued;

        ResetEvent(Ov.hEvent);
        Issued = ReadDirectoryChangesW(
            Ctx->DirHandle,
            Buffer,
            AI_WATCH_BUF_BYTES,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_CREATION,
            NULL,
            &Ov,
            NULL);
        if (!Issued)
        {
            Sleep(1000);
            continue;
        }

        WaitRes = WaitForMultipleObjects(2, WaitHandles, FALSE, INFINITE);
        if (WaitRes == WAIT_OBJECT_0)
        {
            CancelIo(Ctx->DirHandle);
            break;
        }
        if (WaitRes == WAIT_OBJECT_0 + 1)
        {
            if (GetOverlappedResult(Ctx->DirHandle, &Ov, &BytesReturned, FALSE) && BytesReturned > 0)
                ProcessNotifyBuffer(Ctx, Buffer, BytesReturned);
        }
    }

    CloseHandle(Ov.hEvent);
    free(Buffer);
    return 0;
}

static DWORD WINAPI InitialCrawlThread(PVOID Param)
{
    AI_INDEXER *Indexer = (AI_INDEXER *)Param;
    AiIndexerCrawlNow(Indexer);
    return 0;
}

static VOID AddRootIfPresent(AI_INDEXER *Indexer, PCWSTR Path)
{
    DWORD Attrs;
    AI_WATCH_CTX *Ctx;
    SIZE_T Len;

    if (Indexer->RootCount >= AI_MAX_INDEX_ROOTS || Path == NULL || Path[0] == UNICODE_NULL)
        return;

    Attrs = GetFileAttributesW(Path);
    if (Attrs == INVALID_FILE_ATTRIBUTES || !(Attrs & FILE_ATTRIBUTE_DIRECTORY))
        return;

    Ctx = &Indexer->Watchers[Indexer->RootCount];
    ZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Indexer = Indexer;
    wcsncpy(Ctx->RootPath, Path, ARRAYSIZE(Ctx->RootPath) - 1);
    Len = wcslen(Ctx->RootPath);
    while (Len > 3 && (Ctx->RootPath[Len - 1] == L'\\' || Ctx->RootPath[Len - 1] == L'/'))
    {
        Ctx->RootPath[Len - 1] = UNICODE_NULL;
        Len--;
    }

    Ctx->DirHandle = CreateFileW(
        Ctx->RootPath,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);
    if (Ctx->DirHandle == INVALID_HANDLE_VALUE)
        return;

    Indexer->RootCount++;
}

static VOID LoadConfiguredRoots(AI_INDEXER *Indexer)
{
    HKEY Key = NULL;
    WCHAR RegRoots[1024];
    DWORD Type = 0;
    DWORD Size = sizeof(RegRoots);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AI_REG_KEY, 0, KEY_READ, &Key) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(Key, L"IndexRoots", NULL, &Type, (LPBYTE)RegRoots, &Size) == ERROR_SUCCESS &&
            (Type == REG_SZ || Type == REG_EXPAND_SZ))
        {
            PWSTR Cursor = RegRoots;
            RegRoots[ARRAYSIZE(RegRoots) - 1] = UNICODE_NULL;
            while (*Cursor != UNICODE_NULL)
            {
                PWSTR Semi = wcschr(Cursor, L';');
                if (Semi != NULL)
                    *Semi = UNICODE_NULL;
                AddRootIfPresent(Indexer, Cursor);
                if (Semi == NULL)
                    break;
                Cursor = Semi + 1;
            }
        }
        RegCloseKey(Key);
    }

    if (Indexer->RootCount == 0)
        AddRootIfPresent(Indexer, L"C:\\Users");
}

BOOL AiIndexerStart(
    _In_ AI_STORE *Store,
    _Out_ AI_INDEXER **OutIndexer)
{
    AI_INDEXER *Indexer;
    DWORD i;

    if (Store == NULL || OutIndexer == NULL)
        return FALSE;

    *OutIndexer = NULL;
    Indexer = (AI_INDEXER *)calloc(1, sizeof(*Indexer));
    if (Indexer == NULL)
        return FALSE;

    Indexer->Store = Store;
    Indexer->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Indexer->StopEvent == NULL)
    {
        free(Indexer);
        return FALSE;
    }

    LoadConfiguredRoots(Indexer);

    for (i = 0; i < Indexer->RootCount; i++)
    {
        Indexer->Watchers[i].Thread = CreateThread(
            NULL,
            0,
            WatchRootThread,
            &Indexer->Watchers[i],
            0,
            NULL);
    }

    Indexer->CrawlThread = CreateThread(NULL, 0, InitialCrawlThread, Indexer, 0, NULL);
    *OutIndexer = Indexer;
    return TRUE;
}

VOID AiIndexerStop(
    _In_opt_ AI_INDEXER *Indexer)
{
    DWORD i;

    if (Indexer == NULL)
        return;

    SetEvent(Indexer->StopEvent);

    if (Indexer->CrawlThread != NULL)
    {
        WaitForSingleObject(Indexer->CrawlThread, 5000);
        CloseHandle(Indexer->CrawlThread);
    }

    for (i = 0; i < Indexer->RootCount; i++)
    {
        if (Indexer->Watchers[i].Thread != NULL)
        {
            WaitForSingleObject(Indexer->Watchers[i].Thread, 3000);
            CloseHandle(Indexer->Watchers[i].Thread);
        }
        if (Indexer->Watchers[i].DirHandle != NULL &&
            Indexer->Watchers[i].DirHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(Indexer->Watchers[i].DirHandle);
        }
    }

    CloseHandle(Indexer->StopEvent);
    free(Indexer);
}

DWORD AiIndexerCrawlNow(
    _In_ AI_INDEXER *Indexer)
{
    DWORD Total = 0;
    DWORD i;

    if (Indexer == NULL)
        return 0;

    if (InterlockedCompareExchange(&Indexer->Busy, 1, 0) != 0)
        return 0;

    for (i = 0; i < Indexer->RootCount; i++)
        Total += CrawlRecursive(Indexer, Indexer->Watchers[i].RootPath, 0);

    InterlockedExchange(&Indexer->Busy, 0);
    return Total;
}

BOOL AiIndexerIndexSinglePath(
    _In_ AI_INDEXER *Indexer,
    _In_ PCWSTR Path)
{
    if (Indexer == NULL || Path == NULL)
        return FALSE;
    return IndexPathInternal(Indexer->Store, Path, NULL);
}

BOOL AiIndexerIsBusy(
    _In_ AI_INDEXER *Indexer)
{
    return (Indexer != NULL) && (InterlockedCompareExchange(&Indexer->Busy, 0, 0) != 0);
}

DWORD AiIndexerGetRootCount(
    _In_ AI_INDEXER *Indexer)
{
    return (Indexer != NULL) ? Indexer->RootCount : 0;
}
