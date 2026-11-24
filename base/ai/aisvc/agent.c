/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Multi-turn tool-calling OS agent loop, Tier 2 approval flow, and AI folder auto-summarizer
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "ai_json.h"
#include "policy.h"

#define AI_AGENT_MAX_ROUNDS    6U
#define AI_AGENT_MAX_HISTORY   20U
#define AI_AGENT_MAX_PENDING   8U

typedef struct _AI_OWNED_MESSAGE
{
    char Role[16];
    char *Content;
    char ToolCallId[AI_MAX_TOOL_ID];
    AI_TOOL_CALL ToolCalls[AI_MAX_TOOL_CALLS];
    uint32_t ToolCallCount;
} AI_OWNED_MESSAGE;

typedef struct _AI_PENDING_SLOT
{
    int Active;
    uint32_t ApprovalId;
    uint32_t Mode;
    uint32_t RoundsUsed;
    uint32_t ToolCallsExecuted;
    AI_TOOL_CALL PendingCall;
    AI_CAPABILITY_TIER PendingTier;
    char Target[AI_MAX_PATH_UTF8];
    AI_OWNED_MESSAGE History[AI_AGENT_MAX_HISTORY];
    uint32_t HistoryCount;
} AI_PENDING_SLOT;

struct _AI_AGENT
{
    AI_STORE *Store;
    AI_PROVIDER *Provider;
    AI_AGENT_ENV Env;
    uint32_t NextApprovalId;
    AI_PENDING_SLOT Pending[AI_AGENT_MAX_PENDING];
};

static const char g_SystemPromptAsk[] =
    "You are the LiberNT OS assistant (read-only Ask mode). "
    "Use search_files, read_file_snippet, list_directory, get_folder_description, and system_status "
    "to ground your answers in real local files and folder descriptions. Be concise and accurate.";

static const char g_SystemPromptDo[] =
    "You are the LiberNT OS agent (Action mode). "
    "Use search_files, read_file_snippet, list_directory, get_folder_description, set_folder_description, "
    "system_status, write_file, delete_file, and run_command as needed. "
    "Prefer inspecting existing files/descriptions before mutating anything. Be concise.";

static void CopyStrSafe(char *Dst, size_t Cap, const char *Src)
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

static char *DupStr(const char *Src)
{
    size_t Len;
    char *Copy;

    if (Src == NULL)
        return NULL;
    Len = strlen(Src);
    Copy = (char *)malloc(Len + 1);
    if (Copy == NULL)
        return NULL;
    memcpy(Copy, Src, Len + 1);
    return Copy;
}

static void FreeHistory(AI_OWNED_MESSAGE *History, uint32_t Count)
{
    uint32_t i;

    for (i = 0; i < Count; i++)
    {
        free(History[i].Content);
        History[i].Content = NULL;
    }
}

static void ClearPendingSlot(AI_PENDING_SLOT *Slot)
{
    if (Slot == NULL)
        return;
    FreeHistory(Slot->History, Slot->HistoryCount);
    memset(Slot, 0, sizeof(*Slot));
}

static int PushHistory(
    AI_OWNED_MESSAGE *History,
    uint32_t *Count,
    const char *Role,
    const char *Content,
    const char *ToolCallId,
    const AI_TOOL_CALL *Calls,
    uint32_t CallCount)
{
    AI_OWNED_MESSAGE *Msg;

    if (*Count >= AI_AGENT_MAX_HISTORY)
        return -1;
    Msg = &History[*Count];
    memset(Msg, 0, sizeof(*Msg));
    CopyStrSafe(Msg->Role, sizeof(Msg->Role), Role);
    if (Content != NULL)
    {
        Msg->Content = DupStr(Content);
        if (Msg->Content == NULL)
            return -1;
    }
    if (ToolCallId != NULL)
        CopyStrSafe(Msg->ToolCallId, sizeof(Msg->ToolCallId), ToolCallId);
    if (Calls != NULL && CallCount > 0)
    {
        if (CallCount > AI_MAX_TOOL_CALLS)
            CallCount = AI_MAX_TOOL_CALLS;
        memcpy(Msg->ToolCalls, Calls, CallCount * sizeof(AI_TOOL_CALL));
        Msg->ToolCallCount = CallCount;
    }
    (*Count)++;
    return 0;
}

static void BuildMessageView(
    const AI_OWNED_MESSAGE *Owned,
    uint32_t Count,
    AI_CHAT_MESSAGE *View)
{
    uint32_t i;

    for (i = 0; i < Count; i++)
    {
        View[i].Role = Owned[i].Role;
        View[i].Content = Owned[i].Content;
        View[i].ToolCallId = Owned[i].ToolCallId[0] ? Owned[i].ToolCallId : NULL;
        View[i].ToolCalls = Owned[i].ToolCallCount ? Owned[i].ToolCalls : NULL;
        View[i].ToolCallCount = Owned[i].ToolCallCount;
    }
}

static int ExecuteTool(
    AI_AGENT *Agent,
    const AI_TOOL_CALL *Call,
    char *OutBuf,
    size_t OutCap)
{
    const char *Args = Call->ArgumentsJson;
    size_t ArgsLen = strlen(Args);

    OutBuf[0] = '\0';

    if (strcmp(Call->Name, "search_files") == 0)
    {
        char Query[AI_MAX_QUERY_UTF8];
        int64_t MaxRes = AI_MAX_SEARCH_RESULTS;
        AI_SEARCH_HIT Hits[AI_MAX_SEARCH_RESULTS];
        float QueryVec[AI_MAX_EMBED_DIM];
        uint32_t QueryDim = 0;
        uint32_t Count = 0;
        uint32_t i;
        AI_JSON_BUF Buf;

        if (AiJsonObjGetString(Args, ArgsLen, "query", Query, sizeof(Query)) != 0 || Query[0] == '\0')
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing query\"}");
            return -1;
        }
        AiJsonObjGetInt64(Args, ArgsLen, "max_results", &MaxRes);
        if (MaxRes <= 0 || MaxRes > (int64_t)AI_MAX_SEARCH_RESULTS)
            MaxRes = AI_MAX_SEARCH_RESULTS;

        memset(QueryVec, 0, sizeof(QueryVec));
        if (Agent->Provider != NULL && Agent->Provider->Vtbl->Embed != NULL)
        {
            if (Agent->Provider->Vtbl->Embed(Agent->Provider, Query, QueryVec, AI_MAX_EMBED_DIM, &QueryDim) != 0)
                QueryDim = 0;
        }

        if (AiStoreSearch(Agent->Store, Query, QueryDim ? QueryVec : NULL, QueryDim,
                          Hits, (uint32_t)MaxRes, &Count) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"search failed\"}");
            return -1;
        }

        AiJsonBufInit(&Buf, 512);
        AiJsonBufAppendStr(&Buf, "{\"count\":");
        AiJsonBufAppendInt64(&Buf, (int64_t)Count);
        AiJsonBufAppendStr(&Buf, ",\"hits\":[");
        for (i = 0; i < Count; i++)
        {
            if (i > 0)
                AiJsonBufAppendChar(&Buf, ',');
            AiJsonBufAppendStr(&Buf, "{\"type\":");
            AiJsonBufAppendJsonString(&Buf, Hits[i].IsDir ? "dir" : "file");
            AiJsonBufAppendStr(&Buf, ",\"path\":");
            AiJsonBufAppendJsonString(&Buf, Hits[i].Path);
            if (Hits[i].Description[0] != '\0')
            {
                AiJsonBufAppendStr(&Buf, ",\"description\":");
                AiJsonBufAppendJsonString(&Buf, Hits[i].Description);
            }
            if (Hits[i].Snippet[0] != '\0')
            {
                AiJsonBufAppendStr(&Buf, ",\"snippet\":");
                AiJsonBufAppendJsonString(&Buf, Hits[i].Snippet);
            }
            AiJsonBufAppendChar(&Buf, '}');
        }
        AiJsonBufAppendStr(&Buf, "]}");
        CopyStrSafe(OutBuf, OutCap, Buf.Data ? Buf.Data : "{\"count\":0,\"hits\":[]}");
        AiJsonBufFree(&Buf);
        return 0;
    }

    if (strcmp(Call->Name, "read_file_snippet") == 0)
    {
        char Path[AI_MAX_PATH_UTF8];
        int64_t MaxBytes = 2048;

        if (AiJsonObjGetString(Args, ArgsLen, "path", Path, sizeof(Path)) != 0 || Path[0] == '\0')
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing path\"}");
            return -1;
        }
        AiJsonObjGetInt64(Args, ArgsLen, "max_bytes", &MaxBytes);
        if (MaxBytes <= 0 || MaxBytes > 3072)
            MaxBytes = 2048;

        if (Agent->Env.ReadFileSnippet == NULL)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"read_file_snippet unavailable\"}");
            return -1;
        }
        if (Agent->Env.ReadFileSnippet(Agent->Env.UserCtx, Path, (size_t)MaxBytes, OutBuf, OutCap) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"failed to read file\"}");
            return -1;
        }
        return 0;
    }

    if (strcmp(Call->Name, "list_directory") == 0)
    {
        char Path[AI_MAX_PATH_UTF8];

        if (AiJsonObjGetString(Args, ArgsLen, "path", Path, sizeof(Path)) != 0 || Path[0] == '\0')
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing path\"}");
            return -1;
        }
        if (Agent->Env.ListDirectory == NULL)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"list_directory unavailable\"}");
            return -1;
        }
        if (Agent->Env.ListDirectory(Agent->Env.UserCtx, Path, OutBuf, OutCap) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"failed to list directory\"}");
            return -1;
        }
        return 0;
    }

    if (strcmp(Call->Name, "get_folder_description") == 0)
    {
        char Path[AI_MAX_PATH_UTF8];
        char Desc[AI_MAX_DESC_UTF8];
        char Source[AI_MAX_SOURCE_UTF8];
        AI_JSON_BUF Buf;

        if (AiJsonObjGetString(Args, ArgsLen, "path", Path, sizeof(Path)) != 0 || Path[0] == '\0')
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing path\"}");
            return -1;
        }
        if (AiStoreGetFolderDescription(Agent->Store, Path, Desc, sizeof(Desc), Source, sizeof(Source)) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"found\":false}");
            return 0;
        }
        AiJsonBufInit(&Buf, 256);
        AiJsonBufAppendStr(&Buf, "{\"found\":true,\"source\":");
        AiJsonBufAppendJsonString(&Buf, Source);
        AiJsonBufAppendStr(&Buf, ",\"description\":");
        AiJsonBufAppendJsonString(&Buf, Desc);
        AiJsonBufAppendChar(&Buf, '}');
        CopyStrSafe(OutBuf, OutCap, Buf.Data ? Buf.Data : "{\"found\":true}");
        AiJsonBufFree(&Buf);
        return 0;
    }

    if (strcmp(Call->Name, "set_folder_description") == 0)
    {
        char Path[AI_MAX_PATH_UTF8];
        char Desc[AI_MAX_DESC_UTF8];
        char Tags[AI_MAX_TAGS_UTF8];
        char Source[AI_MAX_SOURCE_UTF8];
        char ExistingDesc[AI_MAX_DESC_UTF8];
        char ExistingSource[AI_MAX_SOURCE_UTF8];

        Tags[0] = '\0';
        CopyStrSafe(Source, sizeof(Source), "ai");

        if (AiJsonObjGetString(Args, ArgsLen, "path", Path, sizeof(Path)) != 0 ||
            AiJsonObjGetString(Args, ArgsLen, "description", Desc, sizeof(Desc)) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing path or description\"}");
            return -1;
        }
        AiJsonObjGetString(Args, ArgsLen, "tags", Tags, sizeof(Tags));
        AiJsonObjGetString(Args, ArgsLen, "source", Source, sizeof(Source));
        if (strcmp(Source, "user") != 0)
            CopyStrSafe(Source, sizeof(Source), "ai");

        if (strcmp(Source, "ai") == 0 &&
            AiStoreGetFolderDescription(Agent->Store, Path, ExistingDesc, sizeof(ExistingDesc),
                                        ExistingSource, sizeof(ExistingSource)) == 0 &&
            strcmp(ExistingSource, "user") == 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"folder has a user-pinned description\"}");
            return -1;
        }

        if (Agent->Env.WriteFolderAds != NULL &&
            Agent->Env.WriteFolderAds(Agent->Env.UserCtx, Path, Desc, Source, Tags) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"failed to write folder description ADS\"}");
            return -1;
        }
        if (AiStoreSetFolderDescription(Agent->Store, Path, Desc, Source, Tags) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"failed to save folder description in store\"}");
            return -1;
        }
        CopyStrSafe(OutBuf, OutCap, "{\"ok\":true}");
        return 0;
    }

    if (strcmp(Call->Name, "system_status") == 0)
    {
        if (Agent->Env.GetSystemSummary != NULL &&
            Agent->Env.GetSystemSummary(Agent->Env.UserCtx, OutBuf, OutCap) == 0)
        {
            return 0;
        }
        {
            AI_STORE_STATS Stats;
            memset(&Stats, 0, sizeof(Stats));
            AiStoreGetStats(Agent->Store, &Stats);
            snprintf(OutBuf, OutCap,
                     "{\"files\":%llu,\"dirs\":%llu,\"described_dirs\":%llu,\"embeddings\":%llu,\"audits\":%llu}",
                     (unsigned long long)Stats.FileCount,
                     (unsigned long long)Stats.DirCount,
                     (unsigned long long)Stats.DescribedDirCount,
                     (unsigned long long)Stats.EmbeddingCount,
                     (unsigned long long)Stats.AuditCount);
            return 0;
        }
    }

    if (strcmp(Call->Name, "write_file") == 0)
    {
        char Path[AI_MAX_PATH_UTF8];
        char Content[2048];
        char Err[128];

        if (AiJsonObjGetString(Args, ArgsLen, "path", Path, sizeof(Path)) != 0 ||
            AiJsonObjGetString(Args, ArgsLen, "content", Content, sizeof(Content)) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing path or content\"}");
            return -1;
        }
        if (Agent->Env.WriteFileUtf8 == NULL)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"write_file unavailable\"}");
            return -1;
        }
        Err[0] = '\0';
        if (Agent->Env.WriteFileUtf8(Agent->Env.UserCtx, Path, Content, Err, sizeof(Err)) != 0)
        {
            snprintf(OutBuf, OutCap, "{\"error\":\"%s\"}", Err[0] ? Err : "write_file failed");
            return -1;
        }
        snprintf(OutBuf, OutCap, "{\"ok\":true,\"path\":\"%s\",\"bytes\":%u}",
                 Path, (unsigned int)strlen(Content));
        return 0;
    }

    if (strcmp(Call->Name, "delete_file") == 0)
    {
        char Path[AI_MAX_PATH_UTF8];
        char Err[128];

        if (AiJsonObjGetString(Args, ArgsLen, "path", Path, sizeof(Path)) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing path\"}");
            return -1;
        }
        if (Agent->Env.DeleteFileUtf8 == NULL)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"delete_file unavailable\"}");
            return -1;
        }
        Err[0] = '\0';
        if (Agent->Env.DeleteFileUtf8(Agent->Env.UserCtx, Path, Err, sizeof(Err)) != 0)
        {
            snprintf(OutBuf, OutCap, "{\"error\":\"%s\"}", Err[0] ? Err : "delete_file failed");
            return -1;
        }
        CopyStrSafe(OutBuf, OutCap, "{\"ok\":true}");
        return 0;
    }

    if (strcmp(Call->Name, "run_command") == 0)
    {
        char Cmd[512];
        char CmdOut[1536];
        int ExitCode = 0;
        AI_JSON_BUF Buf;

        if (AiJsonObjGetString(Args, ArgsLen, "command", Cmd, sizeof(Cmd)) != 0 || Cmd[0] == '\0')
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"missing command\"}");
            return -1;
        }
        if (Agent->Env.RunCommandUtf8 == NULL)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"run_command unavailable\"}");
            return -1;
        }
        CmdOut[0] = '\0';
        if (Agent->Env.RunCommandUtf8(Agent->Env.UserCtx, Cmd, CmdOut, sizeof(CmdOut), &ExitCode) != 0)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"run_command failed\"}");
            return -1;
        }
        AiJsonBufInit(&Buf, 512);
        AiJsonBufAppendStr(&Buf, "{\"exit_code\":");
        AiJsonBufAppendInt64(&Buf, ExitCode);
        AiJsonBufAppendStr(&Buf, ",\"output\":");
        AiJsonBufAppendJsonString(&Buf, CmdOut);
        AiJsonBufAppendChar(&Buf, '}');
        CopyStrSafe(OutBuf, OutCap, Buf.Data ? Buf.Data : "{\"exit_code\":0}");
        AiJsonBufFree(&Buf);
        return 0;
    }

    if (strcmp(Call->Name, "list_windows") == 0 ||
        strcmp(Call->Name, "inspect_window") == 0 ||
        strcmp(Call->Name, "focus_window") == 0 ||
        strcmp(Call->Name, "arrange_windows") == 0 ||
        strcmp(Call->Name, "launch_app") == 0 ||
        strcmp(Call->Name, "close_window") == 0)
    {
        if (Agent->Env.DesktopTool == NULL)
        {
            CopyStrSafe(OutBuf, OutCap, "{\"error\":\"desktop shell (aishell.exe) is not running\"}");
            return -1;
        }
        return Agent->Env.DesktopTool(Agent->Env.UserCtx, Call->Name, Args, OutBuf, OutCap);
    }

    CopyStrSafe(OutBuf, OutCap, "{\"error\":\"unknown tool\"}");
    return -1;
}

static AI_PENDING_SLOT *AllocPendingSlot(AI_AGENT *Agent)
{
    uint32_t i;

    for (i = 0; i < AI_AGENT_MAX_PENDING; i++)
    {
        if (!Agent->Pending[i].Active)
            return &Agent->Pending[i];
    }
    ClearPendingSlot(&Agent->Pending[0]);
    return &Agent->Pending[0];
}

static int ContinueLoop(
    AI_AGENT *Agent,
    AI_OWNED_MESSAGE *History,
    uint32_t *HistoryCount,
    uint32_t Mode,
    uint32_t RoundsUsed,
    uint32_t ToolCallsExecuted,
    AI_AGENT_RESULT *OutResult)
{
    int AskOnly = (Mode == AI_PROMPT_MODE_ASK) ? 1 : 0;
    const char *ToolsJson = AiPolicyGetToolsSchemaJson(AskOnly);
    uint32_t Round;

    for (Round = RoundsUsed; Round < AI_AGENT_MAX_ROUNDS; Round++)
    {
        AI_CHAT_MESSAGE View[AI_AGENT_MAX_HISTORY];
        AI_CHAT_RESPONSE ChatResp;

        BuildMessageView(History, *HistoryCount, View);
        memset(&ChatResp, 0, sizeof(ChatResp));

        if (Agent->Provider->Vtbl->ChatComplete(
                Agent->Provider, View, *HistoryCount, ToolsJson, &ChatResp) != 0)
        {
            OutResult->State = AiPromptError;
            OutResult->ToolCallsExecuted = ToolCallsExecuted;
            CopyStrSafe(OutResult->ReplyText, sizeof(OutResult->ReplyText),
                        ChatResp.ErrorText[0] ? ChatResp.ErrorText : "provider chat completion failed");
            return 0;
        }

        if (ChatResp.ToolCallCount == 0)
        {
            OutResult->State = AiPromptDone;
            OutResult->ToolCallsExecuted = ToolCallsExecuted;
            CopyStrSafe(OutResult->ReplyText, sizeof(OutResult->ReplyText), ChatResp.Content);
            return 0;
        }

        /* Record assistant turn with first tool call so single-step Tier 2 gating stays exact */
        if (PushHistory(History, HistoryCount, "assistant",
                        ChatResp.Content[0] ? ChatResp.Content : NULL,
                        NULL, &ChatResp.ToolCalls[0], 1) != 0)
        {
            OutResult->State = AiPromptError;
            CopyStrSafe(OutResult->ReplyText, sizeof(OutResult->ReplyText), "conversation history limit reached");
            return 0;
        }

        {
            const AI_TOOL_CALL *Call = &ChatResp.ToolCalls[0];
            AI_CAPABILITY_TIER Tier = AiPolicyGetToolTier(Call->Name);
            AI_POLICY_DECISION Decision = AiPolicyEvaluate(Call->Name, AskOnly, 0);
            char Target[AI_MAX_PATH_UTF8];
            char ToolResult[2048];

            AiPolicyExtractTarget(Call->Name, Call->ArgumentsJson, Target, sizeof(Target));

            if (Decision == AiPolicyDeny)
            {
                AiStoreLogAudit(Agent->Store, (uint32_t)Tier, Call->Name, Target, "denied_policy",
                                AskOnly ? "ask_mode_readonly" : "unknown_tool");
                PushHistory(History, HistoryCount, "tool",
                            "{\"error\":\"tool denied by OS capability policy\"}", Call->Id, NULL, 0);
                continue;
            }

            if (Decision == AiPolicyNeedApproval)
            {
                AI_PENDING_SLOT *Slot = AllocPendingSlot(Agent);
                uint32_t Id = ++Agent->NextApprovalId;
                uint32_t h;

                memset(Slot, 0, sizeof(*Slot));
                Slot->Active = 1;
                Slot->ApprovalId = Id;
                Slot->Mode = Mode;
                Slot->RoundsUsed = Round + 1;
                Slot->ToolCallsExecuted = ToolCallsExecuted;
                Slot->PendingCall = *Call;
                Slot->PendingTier = Tier;
                CopyStrSafe(Slot->Target, sizeof(Slot->Target), Target);
                Slot->HistoryCount = *HistoryCount;
                for (h = 0; h < *HistoryCount; h++)
                {
                    Slot->History[h] = History[h];
                    History[h].Content = NULL;
                }
                *HistoryCount = 0;

                AiStoreLogAudit(Agent->Store, (uint32_t)Tier, Call->Name, Target, "pending_approval", Call->ArgumentsJson);

                OutResult->State = AiPromptPendingApproval;
                OutResult->ApprovalId = Id;
                OutResult->ToolTier = (uint32_t)Tier;
                OutResult->ToolCallsExecuted = ToolCallsExecuted;
                CopyStrSafe(OutResult->ToolName, sizeof(OutResult->ToolName), Call->Name);
                CopyStrSafe(OutResult->ToolTarget, sizeof(OutResult->ToolTarget), Target);
                CopyStrSafe(OutResult->ToolArgs, sizeof(OutResult->ToolArgs), Call->ArgumentsJson);
                snprintf(OutResult->ReplyText, sizeof(OutResult->ReplyText),
                         "Approval required (Tier %u): %s on %s",
                         (unsigned int)Tier, Call->Name, Target[0] ? Target : "(unspecified)");
                return 0;
            }

            {
                int Rc = ExecuteTool(Agent, Call, ToolResult, sizeof(ToolResult));
                ToolCallsExecuted++;
                AiStoreLogAudit(Agent->Store, (uint32_t)Tier, Call->Name, Target,
                                (Rc == 0) ? "ok" : "error", "");
                PushHistory(History, HistoryCount, "tool", ToolResult, Call->Id, NULL, 0);
            }
        }
    }

    OutResult->State = AiPromptError;
    OutResult->ToolCallsExecuted = ToolCallsExecuted;
    CopyStrSafe(OutResult->ReplyText, sizeof(OutResult->ReplyText), "maximum tool rounds reached");
    return 0;
}

AI_AGENT *AiAgentCreate(
    AI_STORE *Store,
    AI_PROVIDER *Provider,
    const AI_AGENT_ENV *Env)
{
    AI_AGENT *Agent;

    if (Store == NULL)
        return NULL;

    Agent = (AI_AGENT *)calloc(1, sizeof(*Agent));
    if (Agent == NULL)
        return NULL;
    Agent->Store = Store;
    Agent->Provider = Provider;
    if (Env != NULL)
        Agent->Env = *Env;
    return Agent;
}

void AiAgentDestroy(AI_AGENT *Agent)
{
    uint32_t i;

    if (Agent == NULL)
        return;
    for (i = 0; i < AI_AGENT_MAX_PENDING; i++)
    {
        if (Agent->Pending[i].Active)
            ClearPendingSlot(&Agent->Pending[i]);
    }
    free(Agent);
}

void AiAgentSetProvider(AI_AGENT *Agent, AI_PROVIDER *Provider)
{
    if (Agent == NULL)
        return;
    Agent->Provider = Provider;
}

int AiAgentRunPrompt(
    AI_AGENT *Agent,
    const char *PromptUtf8,
    uint32_t Mode,
    AI_AGENT_RESULT *OutResult)
{
    AI_OWNED_MESSAGE History[AI_AGENT_MAX_HISTORY];
    uint32_t HistoryCount = 0;

    if (Agent == NULL || PromptUtf8 == NULL || OutResult == NULL)
        return AiStatusInvalidArg;

    memset(OutResult, 0, sizeof(*OutResult));
    if (Agent->Provider == NULL)
    {
        OutResult->State = AiPromptError;
        CopyStrSafe(OutResult->ReplyText, sizeof(OutResult->ReplyText), "AI provider not configured");
        return AiStatusOk;
    }

    memset(History, 0, sizeof(History));
    PushHistory(History, &HistoryCount, "system",
                (Mode == AI_PROMPT_MODE_ASK) ? g_SystemPromptAsk : g_SystemPromptDo,
                NULL, NULL, 0);
    PushHistory(History, &HistoryCount, "user", PromptUtf8, NULL, NULL, 0);

    ContinueLoop(Agent, History, &HistoryCount, Mode, 0, 0, OutResult);
    FreeHistory(History, HistoryCount);
    return AiStatusOk;
}

int AiAgentResolveApproval(
    AI_AGENT *Agent,
    uint32_t ApprovalId,
    int Approved,
    AI_AGENT_RESULT *OutResult)
{
    uint32_t i;
    AI_PENDING_SLOT *Slot = NULL;
    AI_OWNED_MESSAGE History[AI_AGENT_MAX_HISTORY];
    uint32_t HistoryCount;
    AI_TOOL_CALL Call;
    AI_CAPABILITY_TIER Tier;
    char Target[AI_MAX_PATH_UTF8];
    uint32_t Mode;
    uint32_t RoundsUsed;
    uint32_t ToolCallsExecuted;
    char ToolResult[2048];

    if (Agent == NULL || OutResult == NULL || ApprovalId == 0)
        return AiStatusInvalidArg;

    memset(OutResult, 0, sizeof(*OutResult));

    for (i = 0; i < AI_AGENT_MAX_PENDING; i++)
    {
        if (Agent->Pending[i].Active && Agent->Pending[i].ApprovalId == ApprovalId)
        {
            Slot = &Agent->Pending[i];
            break;
        }
    }
    if (Slot == NULL)
        return AiStatusNotFound;

    /* Move slot ownership out immediately so ApprovalId cannot be replayed */
    HistoryCount = Slot->HistoryCount;
    for (i = 0; i < HistoryCount; i++)
    {
        History[i] = Slot->History[i];
        Slot->History[i].Content = NULL;
    }
    Call = Slot->PendingCall;
    Tier = Slot->PendingTier;
    CopyStrSafe(Target, sizeof(Target), Slot->Target);
    Mode = Slot->Mode;
    RoundsUsed = Slot->RoundsUsed;
    ToolCallsExecuted = Slot->ToolCallsExecuted;
    Slot->HistoryCount = 0;
    Slot->Active = 0;

    if (!Approved)
    {
        AiStoreLogAudit(Agent->Store, (uint32_t)Tier, Call.Name, Target, "denied_user", "");
        OutResult->State = AiPromptDone;
        OutResult->ToolCallsExecuted = ToolCallsExecuted;
        snprintf(OutResult->ReplyText, sizeof(OutResult->ReplyText),
                 "Action cancelled by user: %s on %s", Call.Name, Target[0] ? Target : "(unspecified)");
        FreeHistory(History, HistoryCount);
        return AiStatusOk;
    }

    if (ExecuteTool(Agent, &Call, ToolResult, sizeof(ToolResult)) == 0)
    {
        AiStoreLogAudit(Agent->Store, (uint32_t)Tier, Call.Name, Target, "approved_ok", "");
    }
    else
    {
        AiStoreLogAudit(Agent->Store, (uint32_t)Tier, Call.Name, Target, "approved_error", ToolResult);
    }
    ToolCallsExecuted++;
    PushHistory(History, &HistoryCount, "tool", ToolResult, Call.Id, NULL, 0);

    ContinueLoop(Agent, History, &HistoryCount, Mode, RoundsUsed, ToolCallsExecuted, OutResult);
    FreeHistory(History, HistoryCount);
    return AiStatusOk;
}

int AiAgentAutoDescribeFolder(
    AI_AGENT *Agent,
    const char *FolderPathUtf8,
    int Force,
    char *OutDesc,
    size_t DescCap,
    char *OutTags,
    size_t TagsCap,
    char *OutSource,
    size_t SourceCap)
{
    char ExistingDesc[AI_MAX_DESC_UTF8];
    char ExistingSource[AI_MAX_SOURCE_UTF8];
    char Listing[1536];
    char Prompt[2560];
    AI_CHAT_MESSAGE Msgs[2];
    AI_CHAT_RESPONSE Resp;
    char GenDesc[AI_MAX_DESC_UTF8];
    char GenTags[AI_MAX_TAGS_UTF8];
    float EmbVec[AI_MAX_EMBED_DIM];
    uint32_t EmbDim = 0;

    if (Agent == NULL || FolderPathUtf8 == NULL || OutDesc == NULL || DescCap == 0)
        return AiStatusInvalidArg;

    OutDesc[0] = '\0';
    if (OutTags && TagsCap > 0)
        OutTags[0] = '\0';
    if (OutSource && SourceCap > 0)
        OutSource[0] = '\0';

    if (Agent->Provider == NULL)
        return AiStatusInternal;

    if (!Force &&
        AiStoreGetFolderDescription(Agent->Store, FolderPathUtf8,
                                    ExistingDesc, sizeof(ExistingDesc),
                                    ExistingSource, sizeof(ExistingSource)) == 0 &&
        strcmp(ExistingSource, "user") == 0)
    {
        CopyStrSafe(OutDesc, DescCap, ExistingDesc);
        if (OutSource && SourceCap > 0)
            CopyStrSafe(OutSource, SourceCap, "user");
        return AiStatusAccessDenied;
    }

    Listing[0] = '\0';
    if (Agent->Env.ListDirectory != NULL)
        Agent->Env.ListDirectory(Agent->Env.UserCtx, FolderPathUtf8, Listing, sizeof(Listing));

    snprintf(Prompt, sizeof(Prompt),
             "Summarize the purpose of folder \"%s\" in one concise sentence (under 140 characters) "
             "based on its contents below. Reply with JSON {\"description\":\"...\",\"tags\":\"tag1,tag2\"}.\n"
             "Contents:\n%s",
             FolderPathUtf8, Listing[0] ? Listing : "(empty folder)");

    memset(Msgs, 0, sizeof(Msgs));
    Msgs[0].Role = "system";
    Msgs[0].Content = "You generate concise folder descriptions for the LiberNT OS semantic filesystem.";
    Msgs[1].Role = "user";
    Msgs[1].Content = Prompt;

    memset(&Resp, 0, sizeof(Resp));
    if (Agent->Provider->Vtbl->ChatComplete(Agent->Provider, Msgs, 2, NULL, &Resp) != 0)
        return AiStatusIoError;

    GenDesc[0] = '\0';
    GenTags[0] = '\0';
    if (AiJsonObjGetString(Resp.Content, strlen(Resp.Content), "description", GenDesc, sizeof(GenDesc)) == 0)
    {
        AiJsonObjGetString(Resp.Content, strlen(Resp.Content), "tags", GenTags, sizeof(GenTags));
    }
    else
    {
        CopyStrSafe(GenDesc, sizeof(GenDesc), Resp.Content);
    }

    if (GenDesc[0] == '\0')
        return AiStatusInternal;

    if (Agent->Env.WriteFolderAds != NULL &&
        Agent->Env.WriteFolderAds(Agent->Env.UserCtx, FolderPathUtf8, GenDesc, "ai", GenTags) != 0)
    {
        return AiStatusIoError;
    }

    if (Force)
        AiStoreClearFolderDescription(Agent->Store, FolderPathUtf8);
    if (AiStoreSetFolderDescription(Agent->Store, FolderPathUtf8, GenDesc, "ai", GenTags) != 0)
        return AiStatusDbError;

    if (Agent->Provider->Vtbl->Embed != NULL)
    {
        AI_PROVIDER_CONFIG Cfg;
        char EmbedText[1024];

        memset(&Cfg, 0, sizeof(Cfg));
        if (Agent->Provider->Vtbl->GetConfig != NULL)
            Agent->Provider->Vtbl->GetConfig(Agent->Provider, &Cfg);

        snprintf(EmbedText, sizeof(EmbedText), "%s %s %s", FolderPathUtf8, GenDesc, GenTags);
        if (Agent->Provider->Vtbl->Embed(Agent->Provider, EmbedText, EmbVec, AI_MAX_EMBED_DIM, &EmbDim) == 0 &&
            EmbDim > 0)
        {
            AiStoreUpsertEmbedding(Agent->Store, FolderPathUtf8,
                                   Cfg.EmbedModel[0] ? Cfg.EmbedModel : "default",
                                   EmbVec, EmbDim);
        }
    }

    AiStoreLogAudit(Agent->Store, (uint32_t)AiTier1Reversible, "auto_describe", FolderPathUtf8, "ok", GenDesc);

    CopyStrSafe(OutDesc, DescCap, GenDesc);
    if (OutTags && TagsCap > 0)
        CopyStrSafe(OutTags, TagsCap, GenTags);
    if (OutSource && SourceCap > 0)
        CopyStrSafe(OutSource, SourceCap, "ai");
    return AiStatusOk;
}
