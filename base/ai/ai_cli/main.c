/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Command-line interface (ai.exe) for semantic search, folder descriptions, and OS agent
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
    printf("LiberNT AI CLI (Milestone 2)\n");
    printf("Usage:\n");
    printf("  ai status                                         Show aisvc health and index statistics\n");
    printf("  ai index                                          Run an immediate incremental NTFS crawl\n");
    printf("  ai search <query...>                              Search files, contents, and folder descriptions\n");
    printf("  ai describe <folder>                              Show a folder's description\n");
    printf("  ai describe <folder> <text...>                    Set a folder's user-visible NTFS description\n");
    printf("  ai describe --clear <folder>                      Clear a folder's NTFS description\n");
    printf("  ai describe --auto [--force] <folder>             AI-generate a folder description from contents\n");
    printf("  ai ask <question...>                              Read-only grounded OS Q&A (Tier 0 tools only)\n");
    printf("  ai do [--yes|--deny] <instruction...>             Agent action mode with Tier 2 approval gate\n");
    printf("  ai config                                         Show active LLM provider configuration\n");
    printf("  ai config set [--endpoint U] [--model M] [--embed-model E] [--api-key K]\n");
}

static BOOL WideArgToUtf8(PCWSTR Wide, char *Out, DWORD Cap)
{
    if (Wide == NULL || Out == NULL || Cap == 0)
        return FALSE;
    if (WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Out, (int)Cap, NULL, NULL) <= 0)
    {
        Out[0] = '\0';
        return FALSE;
    }
    return TRUE;
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

    if (_wcsicmp(Argv[2], L"--auto") == 0)
    {
        BOOL Force = FALSE;
        PCWSTR Folder = NULL;
        char Desc[AI_MAX_DESC_UTF8];
        char Tags[AI_MAX_TAGS_UTF8];
        char Source[AI_MAX_SOURCE_UTF8];

        if (Port == NULL)
        {
            fprintf(stderr, "ai: aisvc connection required for --auto\n");
            return 1;
        }
        if (Argc >= 5 && _wcsicmp(Argv[3], L"--force") == 0)
        {
            Force = TRUE;
            Folder = Argv[4];
        }
        else if (Argc >= 4)
        {
            Folder = Argv[3];
        }
        else
        {
            AiClientClose(Port);
            PrintUsage();
            return 1;
        }

        Res = AiClientAutoDescribeFolder(
            Port, Folder, Force,
            Desc, sizeof(Desc),
            Tags, sizeof(Tags),
            Source, sizeof(Source));
        AiClientClose(Port);
        if (Res == AiStatusAccessDenied)
        {
            printf("pinned [user]: %s (pass --force to overwrite)\n", Desc);
            return 0;
        }
        if (Res != AiStatusOk)
        {
            fprintf(stderr, "ai: auto-describe failed (err=%lu)\n", (unsigned long)Res);
            return 1;
        }
        printf("[%s] %s", Source[0] ? Source : "ai", Desc);
        if (Tags[0] != '\0')
            printf("  (tags: %s)", Tags);
        printf("\n");
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

static int CmdPrompt(int Argc, wchar_t **Argv, DWORD Mode)
{
    int StartIdx = 2;
    int AutoDecision = 0; /* 0 = interactive prompt, 1 = --yes, -1 = --deny */
    char Prompt[AI_MAX_PROMPT_UTF8];
    AI_PROMPT_RESPONSE Resp;
    HANDLE Port = NULL;
    DWORD Res;

    while (StartIdx < Argc)
    {
        if (_wcsicmp(Argv[StartIdx], L"--yes") == 0 || _wcsicmp(Argv[StartIdx], L"-y") == 0)
        {
            AutoDecision = 1;
            StartIdx++;
        }
        else if (_wcsicmp(Argv[StartIdx], L"--deny") == 0)
        {
            AutoDecision = -1;
            StartIdx++;
        }
        else
        {
            break;
        }
    }

    if (!JoinArgsUtf8(Argc, Argv, StartIdx, Prompt, sizeof(Prompt)))
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

    Res = AiClientSubmitPrompt(Port, Prompt, Mode, &Resp);
    while (Res == AiStatusOk && Resp.State == AiPromptPendingApproval)
    {
        BOOL Approve = FALSE;

        printf("[APPROVAL REQUIRED] id=%u tier=%u tool=%s target=%s\n",
               Resp.ApprovalId, Resp.ToolTier, Resp.ToolName,
               Resp.ToolTarget[0] ? Resp.ToolTarget : "(none)");
        printf("  args: %s\n", Resp.ToolArgs);

        if (AutoDecision == 1)
        {
            printf("  decision: approved (--yes)\n");
            Approve = TRUE;
        }
        else if (AutoDecision == -1)
        {
            printf("  decision: denied (--deny)\n");
            Approve = FALSE;
        }
        else
        {
            char Line[32];
            printf("Approve this Tier %u action? [y/N]: ", Resp.ToolTier);
            fflush(stdout);
            if (fgets(Line, sizeof(Line), stdin) != NULL && (Line[0] == 'y' || Line[0] == 'Y'))
                Approve = TRUE;
        }

        Res = AiClientApproveAction(Port, Resp.ApprovalId, Approve, &Resp);
    }

    AiClientClose(Port);

    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: agent request failed (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    if (Resp.State == AiPromptError)
    {
        printf("error: %s\n", Resp.ReplyText);
        return 1;
    }

    printf("%s\n", Resp.ReplyText);
    if (Resp.ToolCallsExecuted > 0)
        printf("[tools_executed=%u]\n", Resp.ToolCallsExecuted);
    return 0;
}

static int CmdConfig(int Argc, wchar_t **Argv)
{
    HANDLE Port = NULL;
    AI_CONFIG_DATA Cfg;
    DWORD Res;

    Res = AiClientConnect(&Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: failed to connect to aisvc ALPC port (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    if (Argc >= 3 && _wcsicmp(Argv[2], L"set") == 0)
    {
        int i;

        memset(&Cfg, 0, sizeof(Cfg));
        for (i = 3; i + 1 < Argc; i += 2)
        {
            if (_wcsicmp(Argv[i], L"--endpoint") == 0)
                WideArgToUtf8(Argv[i + 1], Cfg.Endpoint, sizeof(Cfg.Endpoint));
            else if (_wcsicmp(Argv[i], L"--model") == 0)
                WideArgToUtf8(Argv[i + 1], Cfg.Model, sizeof(Cfg.Model));
            else if (_wcsicmp(Argv[i], L"--embed-model") == 0)
                WideArgToUtf8(Argv[i + 1], Cfg.EmbedModel, sizeof(Cfg.EmbedModel));
            else if (_wcsicmp(Argv[i], L"--api-key") == 0)
            {
                WideArgToUtf8(Argv[i + 1], Cfg.ApiKey, sizeof(Cfg.ApiKey));
                Cfg.HasApiKey = 1;
            }
            else if (_wcsicmp(Argv[i], L"--timeout") == 0)
                Cfg.TimeoutMs = (uint32_t)_wtol(Argv[i + 1]);
        }

        Res = AiClientSetProviderConfig(Port, &Cfg);
        AiClientClose(Port);
        if (Res != AiStatusOk)
        {
            fprintf(stderr, "ai: failed to update provider config (err=%lu)\n", (unsigned long)Res);
            return 1;
        }
        printf("provider config updated\n");
        return 0;
    }

    Res = AiClientGetProviderConfig(Port, &Cfg);
    AiClientClose(Port);
    if (Res != AiStatusOk)
    {
        fprintf(stderr, "ai: failed to query provider config (err=%lu)\n", (unsigned long)Res);
        return 1;
    }

    printf("endpoint: %s\n", Cfg.Endpoint);
    printf("model: %s\n", Cfg.Model);
    printf("embed_model: %s\n", Cfg.EmbedModel);
    printf("api_key: %s\n", Cfg.HasApiKey ? "(configured)" : "(none)");
    printf("timeout_ms: %u\n", Cfg.TimeoutMs);
    return 0;
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
    if (_wcsicmp(argv[1], L"ask") == 0)
        return CmdPrompt(argc, argv, AI_PROMPT_MODE_ASK);
    if (_wcsicmp(argv[1], L"do") == 0)
        return CmdPrompt(argc, argv, AI_PROMPT_MODE_DO);
    if (_wcsicmp(argv[1], L"config") == 0)
        return CmdConfig(argc, argv);

    PrintUsage();
    return 1;
}
