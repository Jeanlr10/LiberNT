/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Capability tier policy gate, fail-closed shell risk classifier, and OpenAI tool schemas
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <ctype.h>
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
    { "get_clipboard",          AiTier0ReadOnly,   NULL      },
    { "set_folder_description", AiTier1Reversible, "path"    },
    { "focus_window",           AiTier1Reversible, "target"  },
    { "arrange_windows",        AiTier1Reversible, "layout"  },
    { "launch_app",             AiTier1Reversible, "app"     },
    { "set_clipboard",          AiTier1Reversible, "text"    },
    { "write_file",             AiTier2Mutating,   "path"    },
    { "delete_file",            AiTier2Mutating,   "path"    },
    { "run_command",            AiTier2Mutating,   "command" },
    { "close_window",           AiTier2Mutating,   "target"  },
    { "click_control",          AiTier2Mutating,   "control" },
    { "send_keys",              AiTier2Mutating,   "target"  },
    { "power_action",           AiTier2Mutating,   "action"  }
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
    "{\"type\":\"function\",\"function\":{\"name\":\"run_command\","
    "\"description\":\"Run a single read-only OS inspection command via cmd.exe (e.g. tasklist, sc query, reg query, ipconfig, netstat, systeminfo, driverquery, dir, whoami, hostname, ver, where, findstr). No pipes or chaining.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"list_windows\","
    "\"description\":\"List visible desktop application windows with titles, classes, and rectangles.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"inspect_window\","
    "\"description\":\"Inspect visible child controls, IDs, and text inside a desktop window.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"get_clipboard\","
    "\"description\":\"Read current UTF-8 text from the interactive desktop clipboard.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}}"
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
    "{\"type\":\"function\",\"function\":{\"name\":\"run_command\","
    "\"description\":\"Run an OS shell command via cmd.exe (processes, services, registry, network, file ops, admin tools). Read-only inspection runs directly; mutating or chained commands require user approval.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_folder_description\","
    "\"description\":\"Set a user-visible NTFS description on a folder.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},"
    "\"tags\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"description\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\","
    "\"description\":\"Create or overwrite a UTF-8 text file (Tier 2: requires user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"delete_file\","
    "\"description\":\"Delete a file (Tier 2: requires user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"list_windows\","
    "\"description\":\"List visible desktop application windows.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"inspect_window\","
    "\"description\":\"Inspect visible child controls, IDs, and text inside a desktop window.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}},"
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
    "{\"type\":\"function\",\"function\":{\"name\":\"close_window\","
    "\"description\":\"Close an open desktop window (Tier 2: requires user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"}},\"required\":[\"target\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"get_clipboard\","
    "\"description\":\"Read current UTF-8 text from the desktop clipboard.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_clipboard\","
    "\"description\":\"Copy UTF-8 text to the desktop clipboard.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"click_control\","
    "\"description\":\"Click a child control/button inside a desktop window (Tier 2: requires user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"},\"control\":{\"type\":\"string\"}},"
    "\"required\":[\"target\",\"control\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"send_keys\","
    "\"description\":\"Type text or send a key shortcut to a desktop window (Tier 2: requires user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"target\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},"
    "\"key\":{\"type\":\"string\"},\"enter\":{\"type\":\"boolean\"}}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"power_action\","
    "\"description\":\"Perform a session/power action: lock, logoff, reboot, or shutdown (Tier 2: requires user approval).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\"}},\"required\":[\"action\"]}}}"
    "]";

static int StrEqI(const char *A, const char *B)
{
    if (A == NULL || B == NULL)
        return 0;
    while (*A && *B)
    {
        if (tolower((unsigned char)*A) != tolower((unsigned char)*B))
            return 0;
        A++;
        B++;
    }
    return (*A == '\0' && *B == '\0');
}

static int HasShellMetaChars(const char *Cmd)
{
    const char *p;

    for (p = Cmd; *p != '\0'; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '&' || c == '|' || c == '>' || c == '<' ||
            c == ';' || c == '^' || c == '`' || c == '%')
        {
            return 1;
        }
    }
    return 0;
}

static const char *ReadToken(const char *p, char *Out, size_t Cap)
{
    size_t n = 0;

    if (Out && Cap > 0)
        Out[0] = '\0';
    if (p == NULL)
        return NULL;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return p;

    if (*p == '"')
    {
        p++;
        while (*p != '\0' && *p != '"')
        {
            if (Out && n + 1 < Cap)
                Out[n++] = *p;
            p++;
        }
        if (*p == '"')
            p++;
    }
    else
    {
        while (*p != '\0' && *p != ' ' && *p != '\t')
        {
            if (Out && n + 1 < Cap)
                Out[n++] = *p;
            p++;
        }
    }
    if (Out && Cap > 0)
        Out[n] = '\0';
    return p;
}

static void StripExeSuffix(char *Verb)
{
    size_t Len = strlen(Verb);

    if (Len > 4 && StrEqI(Verb + Len - 4, ".exe"))
        Verb[Len - 4] = '\0';
}

static int ContainsMutatingFlag(const char *Rest, const char *const *BadFlags, size_t BadCount)
{
    char Tok[64];
    const char *p = Rest;

    while (p != NULL && *p != '\0')
    {
        size_t i;
        p = ReadToken(p, Tok, sizeof(Tok));
        if (Tok[0] == '\0')
            break;
        for (i = 0; i < BadCount; i++)
        {
            if (StrEqI(Tok, BadFlags[i]))
                return 1;
        }
    }
    return 0;
}

AI_CAPABILITY_TIER AiPolicyClassifyCommand(const char *CommandUtf8)
{
    char Verb[64];
    char Sub[64];
    char Sub2[64];
    const char *p;
    size_t i;

    static const char *const kReadOnlyVerbs[] =
    {
        "tasklist",
        "systeminfo",
        "whoami",
        "hostname",
        "ver",
        "vol",
        "netstat",
        "driverquery",
        "where",
        "dir",
        "tree",
        "type",
        "findstr",
        "fc",
        "nslookup",
        "ping",
        "tracert",
        "echo"
    };

    if (CommandUtf8 == NULL || CommandUtf8[0] == '\0')
        return AiTier2Mutating;

    /* Fail closed on any chaining, piping, redirection, variable expansion, or control chars */
    if (HasShellMetaChars(CommandUtf8))
        return AiTier2Mutating;

    p = ReadToken(CommandUtf8, Verb, sizeof(Verb));
    if (Verb[0] == '\0')
        return AiTier2Mutating;

    /* Reject explicit paths/drives for Tier 0 auto-allow so arbitrary binaries cannot masquerade */
    if (strchr(Verb, '\\') != NULL || strchr(Verb, '/') != NULL || strchr(Verb, ':') != NULL)
        return AiTier2Mutating;

    StripExeSuffix(Verb);

    for (i = 0; i < sizeof(kReadOnlyVerbs) / sizeof(kReadOnlyVerbs[0]); i++)
    {
        if (StrEqI(Verb, kReadOnlyVerbs[i]))
            return AiTier0ReadOnly;
    }

    p = ReadToken(p, Sub, sizeof(Sub));

    if (StrEqI(Verb, "ipconfig"))
    {
        static const char *const kBadIpFlags[] =
        {
            "/release", "/release6", "/renew", "/renew6",
            "/flushdns", "/registerdns", "/setclassid", "/setclassid6",
            "-release", "-renew", "-flushdns", "-registerdns"
        };
        if (Sub[0] == '\0')
            return AiTier0ReadOnly;
        for (i = 0; i < sizeof(kBadIpFlags) / sizeof(kBadIpFlags[0]); i++)
        {
            if (StrEqI(Sub, kBadIpFlags[i]))
                return AiTier2Mutating;
        }
        return ContainsMutatingFlag(p, kBadIpFlags, sizeof(kBadIpFlags) / sizeof(kBadIpFlags[0]))
            ? AiTier2Mutating : AiTier0ReadOnly;
    }

    if (StrEqI(Verb, "sc"))
    {
        if (StrEqI(Sub, "query") || StrEqI(Sub, "queryex") || StrEqI(Sub, "qc") ||
            StrEqI(Sub, "qdescription") || StrEqI(Sub, "qfailure") || StrEqI(Sub, "enumdepend"))
        {
            return AiTier0ReadOnly;
        }
        return AiTier2Mutating;
    }

    if (StrEqI(Verb, "reg"))
    {
        return StrEqI(Sub, "query") ? AiTier0ReadOnly : AiTier2Mutating;
    }

    if (StrEqI(Verb, "route"))
    {
        return StrEqI(Sub, "print") ? AiTier0ReadOnly : AiTier2Mutating;
    }

    if (StrEqI(Verb, "arp"))
    {
        return (StrEqI(Sub, "-a") || StrEqI(Sub, "-g") || StrEqI(Sub, "/a"))
            ? AiTier0ReadOnly : AiTier2Mutating;
    }

    if (StrEqI(Verb, "fsutil"))
    {
        if (StrEqI(Sub, "fsinfo"))
            return AiTier0ReadOnly;
        if (StrEqI(Sub, "volume"))
        {
            ReadToken(p, Sub2, sizeof(Sub2));
            return StrEqI(Sub2, "diskfree") ? AiTier0ReadOnly : AiTier2Mutating;
        }
        return AiTier2Mutating;
    }

    if (StrEqI(Verb, "netsh"))
    {
        /* Only allow read-only `netsh ... show ...` queries without mutating verbs */
        int SawShow = StrEqI(Sub, "show");
        char Tok[64];
        const char *q = p;

        while (q != NULL && *q != '\0')
        {
            q = ReadToken(q, Tok, sizeof(Tok));
            if (Tok[0] == '\0')
                break;
            if (StrEqI(Tok, "set") || StrEqI(Tok, "add") || StrEqI(Tok, "delete") ||
                StrEqI(Tok, "reset") || StrEqI(Tok, "exec") || StrEqI(Tok, "install"))
            {
                return AiTier2Mutating;
            }
            if (StrEqI(Tok, "show"))
                SawShow = 1;
        }
        return SawShow ? AiTier0ReadOnly : AiTier2Mutating;
    }

    return AiTier2Mutating;
}

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

AI_CAPABILITY_TIER AiPolicyGetEffectiveTier(
    const char *ToolName,
    const char *ArgsJson)
{
    if (ToolName != NULL && strcmp(ToolName, "run_command") == 0)
    {
        char Cmd[2048];
        if (ArgsJson != NULL &&
            AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "command", Cmd, sizeof(Cmd)) == 0 &&
            Cmd[0] != '\0')
        {
            return AiPolicyClassifyCommand(Cmd);
        }
        return AiTier2Mutating;
    }
    return AiPolicyGetToolTier(ToolName);
}

static AI_POLICY_DECISION EvaluateTier(
    AI_CAPABILITY_TIER Tier,
    int AskOnlyMode,
    int RequireApprovalTier1)
{
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

AI_POLICY_DECISION AiPolicyEvaluate(
    const char *ToolName,
    int AskOnlyMode,
    int RequireApprovalTier1)
{
    return EvaluateTier(AiPolicyGetToolTier(ToolName), AskOnlyMode, RequireApprovalTier1);
}

AI_POLICY_DECISION AiPolicyEvaluateCall(
    const char *ToolName,
    const char *ArgsJson,
    int AskOnlyMode,
    int RequireApprovalTier1)
{
    return EvaluateTier(AiPolicyGetEffectiveTier(ToolName, ArgsJson),
                        AskOnlyMode,
                        RequireApprovalTier1);
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
