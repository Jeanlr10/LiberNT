/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Capability tier policy gate and OpenAI tool schema definitions
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <string.h>

#include "ai_json.h"
#include "policy.h"

typedef struct _AI_TOOL_POLICY_ENTRY
{
    const char *Name;
    AI_CAPABILITY_TIER Tier;
    const char *TargetKey;
} AI_TOOL_POLICY_ENTRY;

static const AI_TOOL_POLICY_ENTRY g_ToolPolicy[] =
{
    { "search_files",           AiTier0ReadOnly,   "query"   },
    { "read_file_snippet",      AiTier0ReadOnly,   "path"    },
    { "list_directory",         AiTier0ReadOnly,   "path"    },
    { "get_folder_description", AiTier0ReadOnly,   "path"    },
    { "system_status",          AiTier0ReadOnly,   NULL      },
    { "list_windows",           AiTier0ReadOnly,   NULL      },
    { "inspect_window",         AiTier0ReadOnly,   "target"  },
    { "set_folder_description", AiTier1Reversible, "path"    },
    { "focus_window",           AiTier1Reversible, "target"  },
    { "arrange_windows",        AiTier1Reversible, "layout"  },
    { "launch_app",             AiTier1Reversible, "app"     },
    { "write_file",             AiTier2Mutating,   "path"    },
    { "delete_file",            AiTier2Mutating,   "path"    },
    { "run_command",            AiTier2Mutating,   "command" },
    { "close_window",           AiTier2Mutating,   "target"  }
};

static const char g_AskToolsSchema[] =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"search_files\","
    "\"description\":\"Search indexed files, file contents, and folder descriptions.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},"
    "\"required\":[\"query\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file_snippet\","
    "\"description\":\"Read a short UTF-8 text preview from a file path.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"max_bytes\":{\"type\":\"integer\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"list_directory\","
    "\"description\":\"List files and subdirectories in a folder along with any folder descriptions.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"get_folder_description\","
    "\"description\":\"Get the user-visible NTFS description attached to a folder.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"system_status\","
    "\"description\":\"Return LiberNT AI index statistics and OS uptime/memory summary.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"list_windows\","
    "\"description\":\"List visible desktop application windows with titles, classes, and rectangles.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"inspect_window\","
    "\"description\":\"Inspect visible child controls and text inside a desktop window.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}}"
    "]";

static const char g_DoToolsSchema[] =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"search_files\","
    "\"description\":\"Search indexed files, file contents, and folder descriptions.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},"
    "\"required\":[\"query\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file_snippet\","
    "\"description\":\"Read a short UTF-8 text preview from a file path.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"max_bytes\":{\"type\":\"integer\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"list_directory\","
    "\"description\":\"List files and subdirectories in a folder along with any folder descriptions.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"get_folder_description\","
    "\"description\":\"Get the user-visible NTFS description attached to a folder.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"system_status\","
    "\"description\":\"Return LiberNT AI index statistics and OS uptime/memory summary.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"list_windows\","
    "\"description\":\"List visible desktop application windows.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"inspect_window\","
    "\"description\":\"Inspect visible child controls and text inside a desktop window.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_folder_description\","
    "\"description\":\"Set a user-visible NTFS description on a folder.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},"
    "\"tags\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"description\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"focus_window\","
    "\"description\":\"Bring a desktop window to the foreground.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"arrange_windows\","
    "\"description\":\"Tile, split, maximize, minimize, or undo desktop window layout.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"layout\":{\"type\":\"string\"},\"targets\":{\"type\":\"string\"}},"
    "\"required\":[\"layout\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"launch_app\","
    "\"description\":\"Launch a desktop application or open a file/folder.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"app\":{\"type\":\"string\"},\"args\":{\"type\":\"string\"}},"
    "\"required\":[\"app\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\","
    "\"description\":\"Create or overwrite a UTF-8 text file (Tier 2: requires explicit user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"delete_file\","
    "\"description\":\"Delete a file (Tier 2: requires explicit user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"run_command\","
    "\"description\":\"Run a cmd.exe command (Tier 2: requires explicit user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"close_window\","
    "\"description\":\"Close an open desktop window (Tier 2: requires explicit user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}}"
    "]";

AI_CAPABILITY_TIER AiPolicyGetToolTier(const char *ToolName)
{
    size_t i;

    if (ToolName == NULL)
        return AiTierUnknown;

    for (i = 0; i < sizeof(g_ToolPolicy) / sizeof(g_ToolPolicy[0]); i++)
    {
        if (strcmp(g_ToolPolicy[i].Name, ToolName) == 0)
            return g_ToolPolicy[i].Tier;
    }
    return AiTierUnknown;
}

AI_POLICY_DECISION AiPolicyEvaluate(
    const char *ToolName,
    int AskOnlyMode,
    int RequireApprovalTier1)
{
    AI_CAPABILITY_TIER Tier = AiPolicyGetToolTier(ToolName);

    switch (Tier)
    {
        case AiTier0ReadOnly:
            return AiPolicyAllow;

        case AiTier1Reversible:
            if (AskOnlyMode)
                return AiPolicyDeny;
            return RequireApprovalTier1 ? AiPolicyNeedApproval : AiPolicyAllow;

        case AiTier2Mutating:
            if (AskOnlyMode)
                return AiPolicyDeny;
            return AiPolicyNeedApproval;

        default:
            return AiPolicyDeny;
    }
}

void AiPolicyExtractTarget(
    const char *ToolName,
    const char *ArgsJson,
    char *TargetOut,
    size_t TargetCap)
{
    size_t i;

    if (TargetOut == NULL || TargetCap == 0)
        return;
    TargetOut[0] = '\0';

    if (ToolName == NULL || ArgsJson == NULL)
        return;

    for (i = 0; i < sizeof(g_ToolPolicy) / sizeof(g_ToolPolicy[0]); i++)
    {
        if (strcmp(g_ToolPolicy[i].Name, ToolName) == 0)
        {
            if (g_ToolPolicy[i].TargetKey != NULL)
            {
                AiJsonObjGetString(ArgsJson, strlen(ArgsJson),
                                   g_ToolPolicy[i].TargetKey, TargetOut, TargetCap);
            }
            return;
        }
    }
}

const char *AiPolicyGetToolsSchemaJson(int AskOnlyMode)
{
    return AskOnlyMode ? g_AskToolsSchema : g_DoToolsSchema;
}
