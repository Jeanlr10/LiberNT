/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Versioned ALPC wire protocol and shared types for aisvc, libai, ai_cli, and aishell
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stdint.h>

#define AI_PROTOCOL_VERSION     1U
#define AI_PORT_NAME            L"\\RPC Control\\LiberNtAiPort"
#define AI_DESKTOP_PORT_NAME    L"\\RPC Control\\LiberNtAiDesktopPort"
#define AI_DESC_STREAM_SUFFIX   L":LiberNT.Description"
#define AI_SERVICE_NAME         L"AiSvc"
#define AI_REG_KEY              L"SOFTWARE\\LiberNT\\AI"

#define AI_MAX_PATH_UTF8        260U
#define AI_MAX_QUERY_UTF8       256U
#define AI_MAX_DESC_UTF8        512U
#define AI_MAX_SOURCE_UTF8      16U
#define AI_MAX_TAGS_UTF8        128U
#define AI_MAX_SNIPPET_UTF8     160U
#define AI_MAX_SEARCH_RESULTS   6U
#define AI_MAX_PROMPT_UTF8      1024U
#define AI_MAX_REPLY_UTF8       2400U

#define AI_PROMPT_MODE_ASK      0U
#define AI_PROMPT_MODE_DO       1U

typedef enum _AI_OPCODE
{
    AiOpPing = 1,
    AiOpGetStatus = 2,
    AiOpSearch = 3,
    AiOpIndexNow = 4,
    AiOpSetFolderDesc = 5,
    AiOpGetFolderDesc = 6,
    AiOpClearFolderDesc = 7,
    AiOpAutoDescribe = 8,
    AiOpGetConfig = 9,
    AiOpSubmitPrompt = 10,
    AiOpApproveAction = 11,
    AiOpDesktopTool = 12,
    AiOpSetConfig = 13
} AI_OPCODE;

typedef enum _AI_RESULT_CODE
{
    AiStatusOk = 0,
    AiStatusInvalidArg = 1,
    AiStatusNotFound = 2,
    AiStatusIoError = 3,
    AiStatusDbError = 4,
    AiStatusAccessDenied = 5,
    AiStatusInternal = 6
} AI_RESULT_CODE;

typedef enum _AI_PROMPT_STATE
{
    AiPromptDone = 0,
    AiPromptPendingApproval = 1,
    AiPromptError = 2
} AI_PROMPT_STATE;

typedef struct _AI_STATUS_INFO
{
    uint32_t ProtocolVersion;
    uint32_t ServiceReady;
    uint32_t IndexingActive;
    uint32_t WatchedRoots;
    uint64_t FileCount;
    uint64_t DirCount;
    uint64_t DescribedDirCount;
    uint64_t EmbeddingCount;
} AI_STATUS_INFO;

typedef struct _AI_SEARCH_HIT
{
    uint32_t IsDir;
    float Score;
    uint64_t Size;
    int64_t Mtime;
    char Path[AI_MAX_PATH_UTF8];
    char Description[AI_MAX_SNIPPET_UTF8];
    char Snippet[AI_MAX_SNIPPET_UTF8];
} AI_SEARCH_HIT;

typedef struct _AI_PROMPT_RESPONSE
{
    uint32_t State;
    uint32_t ApprovalId;
    uint32_t ToolTier;
    uint32_t ToolCallsExecuted;
    char ToolName[64];
    char ToolTarget[AI_MAX_PATH_UTF8];
    char ToolArgs[512];
    char ReplyText[AI_MAX_REPLY_UTF8];
} AI_PROMPT_RESPONSE;

typedef struct _AI_CONFIG_DATA
{
    uint32_t TimeoutMs;
    uint32_t HasApiKey;
    uint32_t MaxTokens;
    uint32_t Threads;
    char ProviderType[32];
    char Endpoint[256];
    char Model[128];
    char EmbedModel[128];
    char ApiKey[256];
    char LocalModelPath[AI_MAX_PATH_UTF8];
    char BackendInfo[160];
} AI_CONFIG_DATA;

typedef struct _AI_MSG_PAYLOAD
{
    uint32_t Version;
    uint32_t Opcode;
    uint32_t Status;
    uint32_t Count;
    union
    {
        AI_STATUS_INFO StatusInfo;
        struct
        {
            uint32_t MaxResults;
            char Query[AI_MAX_QUERY_UTF8];
        } SearchReq;
        struct
        {
            AI_SEARCH_HIT Hits[AI_MAX_SEARCH_RESULTS];
        } SearchResp;
        struct
        {
            uint32_t Force;
            char Path[AI_MAX_PATH_UTF8];
            char Source[AI_MAX_SOURCE_UTF8];
            char Tags[AI_MAX_TAGS_UTF8];
            char Description[AI_MAX_DESC_UTF8];
        } FolderDesc;
        struct
        {
            uint32_t IndexedCount;
        } IndexResp;
        struct
        {
            uint32_t Mode;
            char Prompt[AI_MAX_PROMPT_UTF8];
        } PromptReq;
        struct
        {
            uint32_t ApprovalId;
            uint32_t Approved;
        } ApproveReq;
        AI_PROMPT_RESPONSE PromptResp;
        AI_CONFIG_DATA Config;
        struct
        {
            char ToolName[64];
            char ArgsJson[512];
            char ResultJson[AI_MAX_REPLY_UTF8];
        } DesktopTool;
    } Data;
} AI_MSG_PAYLOAD;

#if defined(_WIN32) && defined(PORT_MESSAGE)
typedef struct _AI_ALPC_MESSAGE
{
    PORT_MESSAGE Header;
    AI_MSG_PAYLOAD Payload;
} AI_ALPC_MESSAGE;
#endif
