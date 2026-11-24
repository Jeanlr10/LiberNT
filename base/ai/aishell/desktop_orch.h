/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Interactive desktop window orchestrator, layout undo stack, and control inspector
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <windef.h>

#ifdef __cplusplus
extern "C" {
#endif

int AiDesktopExecuteTool(
    _In_ const char *ToolName,
    _In_ const char *ArgsJson,
    _In_opt_ HWND ExcludeHwnd,
    _Out_writes_(OutCap) char *OutJson,
    _In_ size_t OutCap);

#ifdef __cplusplus
}
#endif
