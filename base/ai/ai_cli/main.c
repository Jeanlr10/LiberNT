/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Command-line interface (ai.exe) for semantic NTFS search, service status, and folder descriptions
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>

#include "libai.h"

static void PrintUsage(void)
{
    printf("LiberNT AI CLI (Milestone 1)\n");
    printf("Usage:\n");
    printf("  ai status                              Show aisvc health and index statistics\n");
    printf("  ai index                               Run an immediate incremental NTFS crawl\n");
    printf("  ai search <query...>                   Search files, contents, and folder descriptions\n");
    printf("  ai describe <folder>                   Show a folder's description\n");
    printf("  ai describe <folder> <text...>         Set a folder's user-visible NTFS description\n");
    printf("  ai describe --clear <folder>           Clear a folder's NTFS description\n");
}

static BOOL JoinArgsUtf8(int Argc, wchar_t **Argv, int StartIdx, char *Out, DWORD Cap)
{
    DWORD Pos = 0;
    int i;

    if (Out == NULL || Cap == 0)
        return FALSE;
    Out[0] = '\0';

    for (i = StartIdx; i < Argc; i++)
    {
        char Part[256];
        int Len;

        Len = WideCharToMultiByte(CP_UTF8, 0, Argv[i], -1, Part, sizeof(Part), NULL, NULL);
        if (Len <= 1)
            continue;
        Len--;

        if (Pos > 0)
        {
            if (Pos + 1 >= Cap)
                break;
            Out[Pos++] = ' ';
        }
        if (Pos + (DWORD)Len >= Cap)
            Len = (int)(Cap - Pos - 1);
        memcpy(Out + Pos, Part, (size_t)Len);
        Pos += (DWORD)Len;
        Out[Pos] = '\0';
    }
    return Pos > 0;
}

static int CmdStatus(void)
{
    HANDLE Port = NULL;
    AI_STATUS_INFO Info;
    DWORD Res;

    Res = AiClientConnect(&Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: failed to connect to aisvc ALPC port (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    Res = AiClientGetStatus(Port, &Info);
    AiClientClose(Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: status query failed (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    printf("aisvc: ready=%u protocol=%u indexing=%u roots=%u\n",
           Info.ServiceReady,
           Info.ProtocolVersion,
           Info.IndexingActive,
           Info.WatchedRoots);
    printf("index: files=%llu dirs=%llu described_dirs=%llu embeddings=%llu\n",
           (unsigned long long)Info.FileCount,
           (unsigned long long)Info.DirCount,
           (unsigned long long)Info.DescribedDirCount,
           (unsigned long long)Info.EmbeddingCount);
    return 0;
}

static int CmdIndex(void)
{
    HANDLE Port = NULL;
    DWORD Count = 0;
    DWORD Res;

    Res = AiClientConnect(&Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: failed to connect to aisvc ALPC port (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    Res = AiClientIndexNow(Port, &Count);
    AiClientClose(Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: index request failed (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    printf("indexed %lu entries\n", (unsigned long)Count);
    return 0;
}

static int CmdSearch(int Argc, wchar_t **Argv)
{
    char Query[AI_MAX_QUERY_UTF8];
    AI_SEARCH_HIT Hits[AI_MAX_SEARCH_RESULTS];
    HANDLE Port = NULL;
    DWORD Count = 0;
    DWORD Res;
    DWORD i;

    if (!JoinArgsUtf8(Argc, Argv, 2, Query, sizeof(Query)))
    {
        PrintUsage();
        return 1;
    }

    Res = AiClientConnect(&Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: failed to connect to aisvc ALPC port (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    Res = AiClientSearch(Port, Query, AI_MAX_SEARCH_RESULTS, Hits, &Count);
    AiClientClose(Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: search failed (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    printf("results: %lu\n", (unsigned long)Count);
    for (i = 0; i < Count; i++)
    {
        printf("[%s] %s", Hits[i].IsDir ? "DIR" : "FILE", Hits[i].Path);
        if (Hits[i].Description[0] != '\0')
            printf("  -- %s", Hits[i].Description);
        printf("\n");
        if (Hits[i].Snippet[0] != '\0')
            printf("       %s\n", Hits[i].Snippet);
    }
    return 0;
}

static int CmdDescribe(int Argc, wchar_t **Argv)
{
    HANDLE Port = NULL;
    DWORD Res;

    if (Argc < 3)
    {
        PrintUsage();
        return 1;
    }

    AiClientConnect(&Port);

    if (_wcsicmp(Argv[2], L"--clear") == 0)
    {
        if (Argc < 4)
        {
            AiClientClose(Port);
            PrintUsage();
            return 1;
        }
        Res = AiClientClearFolderDescription(Port, Argv[3]);
        AiClientClose(Port);
        if (Res != AiStatusOk)
        {
            fprintf(stderr, "ai: failed to clear folder description (err=%lu)\n", (unsigned long)Res);
            return 1;
        }
        printf("cleared description\n");
        return 0;
    }

    if (Argc == 3)
    {
        char Desc[AI_MAX_DESC_UTF8];
        char Source[AI_MAX_SOURCE_UTF8];

        Res = AiClientGetFolderDescription(Port, Argv[2], Desc, sizeof(Desc), Source, sizeof(Source));
        AiClientClose(Port);
        if (Res != AiStatusOk)
        {
            printf("(no description)\n");
            return 0;
        }
        printf("[%s] %s\n", Source[0] ? Source : "user", Desc);
        return 0;
    }

    {
        char Desc[AI_MAX_DESC_UTF8];

        if (!JoinArgsUtf8(Argc, Argv, 3, Desc, sizeof(Desc)))
        {
            AiClientClose(Port);
            PrintUsage();
            return 1;
        }
        Res = AiClientSetFolderDescription(Port, Argv[2], Desc, "user", "");
        AiClientClose(Port);
        if (Res != AiStatusOk)
        {
            fprintf(stderr, "ai: failed to set folder description (err=%lu)\n", (unsigned long)Res);
            return 1;
        }
        printf("description saved\n");
        return 0;
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    if (_wcsicmp(argv[1], L"status") == 0)
        return CmdStatus();
    if (_wcsicmp(argv[1], L"index") == 0)
        return CmdIndex();
    if (_wcsicmp(argv[1], L"search") == 0)
        return CmdSearch(argc, argv);
    if (_wcsicmp(argv[1], L"describe") == 0)
        return CmdDescribe(argc, argv);

    PrintUsage();
    return 1;
}
