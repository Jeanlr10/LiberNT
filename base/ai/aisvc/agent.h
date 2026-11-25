/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Multi-turn tool-calling OS agent loop, Tier 2 approval flow, and AI folder auto-summarizer
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ai_protocol.h"
#include "ai_provider.h"
#include "ai_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _AI_AGENT_ENV
{
    void *UserCtx;

    int (*ReadFileSnippet)(
        void *UserCtx,
        const char *PathUtf8,
        size_t MaxBytes,
        char *OutBuf,
        size_t OutCap);

    int (*ListDirectory)(
        void *UserCtx,
        const char *DirPathUtf8,
        char *OutBuf,
        size_t OutCap);

    int (*WriteFileUtf8)(
        void *UserCtx,
        const char *PathUtf8,
        const char *ContentUtf8,
        char *ErrOut,
        size_t ErrCap);

    int (*DeleteFileUtf8)(
        void *UserCtx,
        const char *PathUtf8,
        char *ErrOut,
        size_t ErrCap);

    int (*RunCommandUtf8)(
        void *UserCtx,
        const char *CommandUtf8,
        const char *CwdUtf8,
        uint32_t TimeoutMs,
        char *OutBuf,
        size_t OutCap,
        int *OutExitCode);

    int (*WriteFolderAds)(
        void *UserCtx,
        const char *DirPathUtf8,
        const char *DescUtf8,
        const char *SourceUtf8,
        const char *TagsUtf8);

    int (*GetSystemSummary)(
        void *UserCtx,
        char *OutBuf,
        size_t OutCap);

    int (*DesktopTool)(
        void *UserCtx,
        const char *ToolName,
        const char *ArgsJson,
        char *OutJson,
        size_t OutCap);
} AI_AGENT_ENV;

typedef struct _AI_AGENT AI_AGENT;

typedef struct _AI_AGENT_RESULT
{
    uint32_t State;
    uint32_t ApprovalId;
    uint32_t ToolTier;
    uint32_t ToolCallsExecuted;
    char ToolName[64];
    char ToolTarget[AI_MAX_PATH_UTF8];
    char ToolArgs[512];
    char ReplyText[AI_MAX_REPLY_UTF8];
} AI_AGENT_RESULT;

AI_AGENT *AiAgentCreate(
    AI_STORE *Store,
    AI_PROVIDER *Provider,
    const AI_AGENT_ENV *Env);

void AiAgentDestroy(AI_AGENT *Agent);

void AiAgentSetProvider(AI_AGENT *Agent, AI_PROVIDER *Provider);

int AiAgentRunPrompt(
    AI_AGENT *Agent,
    const char *PromptUtf8,
    uint32_t Mode,
    AI_AGENT_RESULT *OutResult);

int AiAgentResolveApproval(
    AI_AGENT *Agent,
    uint32_t ApprovalId,
    int Approved,
    AI_AGENT_RESULT *OutResult);

int AiAgentAutoDescribeFolder(
    AI_AGENT *Agent,
    const char *FolderPathUtf8,
    int Force,
    char *OutDesc,
    size_t DescCap,
    char *OutTags,
    size_t TagsCap,
    char *OutSource,
    size_t SourceCap);

#ifdef __cplusplus
}
#endif
