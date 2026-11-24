/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Win32/NTFS OS tool backend for aisvc agent loop
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include "agent.h"
#include "ai_store.h"
#include "indexer.h"

typedef struct _AI_WIN32_TOOL_CTX
{
    AI_STORE *Store;
    AI_INDEXER *Indexer;
} AI_WIN32_TOOL_CTX;

void AiWin32BuildAgentEnv(
    AI_WIN32_TOOL_CTX *Ctx,
    AI_AGENT_ENV *OutEnv);
