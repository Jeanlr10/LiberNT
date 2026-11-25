/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host-native ASan/UBSan tests for ai_json, openai_codec, policy, and agent loop
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "ai_json.h"
#include "ai_provider.h"
#include "ai_store.h"
#include "policy.h"

static int g_Checks = 0;
static int g_Failures = 0;

#define CHECK(cond, msg) do { \
    g_Checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        g_Failures++; \
    } \
} while (0)

typedef struct _MOCK_ENV_STATE
{
    int Writes;
    int Deletes;
    int AdsWrites;
    int CommandsRun;
    uint32_t LastTimeoutMs;
    char LastWritePath[256];
    char LastWriteContent[512];
    char LastAdsDesc[256];
    char LastAdsSource[32];
    char LastCmd[256];
    char LastCwd[256];
} MOCK_ENV_STATE;

static int MockListDir(void *Ctx, const char *DirPath, char *OutBuf, size_t OutCap)
{
    (void)Ctx;
    (void)DirPath;
    snprintf(OutBuf, OutCap, "[FILE] q4_budget.md (412 bytes)\n[FILE] receipts_2026.csv (1280 bytes)\n");
    return 0;
}

static int MockReadSnippet(void *Ctx, const char *Path, size_t MaxBytes, char *OutBuf, size_t OutCap)
{
    (void)Ctx;
    (void)Path;
    (void)MaxBytes;
    snprintf(OutBuf, OutCap, "LiberNT Q4 budget summary: kernel stability milestone complete.");
    return 0;
}

static int MockWriteFile(void *Ctx, const char *Path, const char *Content, char *ErrOut, size_t ErrCap)
{
    MOCK_ENV_STATE *St = (MOCK_ENV_STATE *)Ctx;
    (void)ErrOut;
    (void)ErrCap;
    St->Writes++;
    snprintf(St->LastWritePath, sizeof(St->LastWritePath), "%s", Path);
    snprintf(St->LastWriteContent, sizeof(St->LastWriteContent), "%s", Content);
    return 0;
}

static int MockDeleteFile(void *Ctx, const char *Path, char *ErrOut, size_t ErrCap)
{
    MOCK_ENV_STATE *St = (MOCK_ENV_STATE *)Ctx;
    (void)Path;
    (void)ErrOut;
    (void)ErrCap;
    St->Deletes++;
    return 0;
}

static int MockWriteAds(void *Ctx, const char *Dir, const char *Desc, const char *Source, const char *Tags)
{
    MOCK_ENV_STATE *St = (MOCK_ENV_STATE *)Ctx;
    (void)Dir;
    (void)Tags;
    St->AdsWrites++;
    snprintf(St->LastAdsDesc, sizeof(St->LastAdsDesc), "%s", Desc);
    snprintf(St->LastAdsSource, sizeof(St->LastAdsSource), "%s", Source ? Source : "");
    return 0;
}

static int MockRunCommand(
    void *Ctx,
    const char *Cmd,
    const char *Cwd,
    uint32_t TimeoutMs,
    char *OutBuf,
    size_t OutCap,
    int *OutExitCode)
{
    MOCK_ENV_STATE *St = (MOCK_ENV_STATE *)Ctx;
    St->CommandsRun++;
    St->LastTimeoutMs = TimeoutMs;
    snprintf(St->LastCmd, sizeof(St->LastCmd), "%s", Cmd ? Cmd : "");
    snprintf(St->LastCwd, sizeof(St->LastCwd), "%s", Cwd ? Cwd : "");
    snprintf(OutBuf, OutCap, "Image Name PID\naisvc.exe 412\n");
    if (OutExitCode)
        *OutExitCode = 0;
    return 0;
}

typedef struct _MOCK_PROVIDER
{
    AI_PROVIDER Base;
    int Scenario;
    int Turn;
} MOCK_PROVIDER;

static int MockChat(
    AI_PROVIDER *Self,
    const AI_CHAT_MESSAGE *Msgs,
    uint32_t MsgCount,
    const char *ToolsJson,
    AI_CHAT_RESPONSE *OutResp)
{
    MOCK_PROVIDER *Mp = (MOCK_PROVIDER *)Self;
    (void)ToolsJson;

    memset(OutResp, 0, sizeof(*OutResp));
    Mp->Turn++;

    if (Mp->Scenario == 1)
    {
        /* Ask mode: search_files -> final answer */
        if (Mp->Turn == 1)
        {
            OutResp->ToolCallCount = 1;
            snprintf(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "call_1");
            snprintf(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), "search_files");
            snprintf(OutResp->ToolCalls[0].ArgumentsJson, sizeof(OutResp->ToolCalls[0].ArgumentsJson),
                     "{\"query\":\"fiscal\"}");
            return 0;
        }
        snprintf(OutResp->Content, sizeof(OutResp->Content),
                 "Found report via tool: %s",
                 (MsgCount > 0 && Msgs[MsgCount - 1].Content) ? Msgs[MsgCount - 1].Content : "");
        return 0;
    }

    if (Mp->Scenario == 2)
    {
        /* Ask mode tries write_file -> must be denied by policy */
        if (Mp->Turn == 1)
        {
            OutResp->ToolCallCount = 1;
            snprintf(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "call_w1");
            snprintf(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), "write_file");
            snprintf(OutResp->ToolCalls[0].ArgumentsJson, sizeof(OutResp->ToolCalls[0].ArgumentsJson),
                     "{\"path\":\"C:\\\\evil.txt\",\"content\":\"nope\"}");
            return 0;
        }
        snprintf(OutResp->Content, sizeof(OutResp->Content),
                 "Policy response: %s",
                 (MsgCount > 0 && Msgs[MsgCount - 1].Content) ? Msgs[MsgCount - 1].Content : "");
        return 0;
    }

    if (Mp->Scenario == 3)
    {
        /* Do mode: write_file -> Tier 2 approval -> approved -> final answer */
        if (Mp->Turn == 1)
        {
            OutResp->ToolCallCount = 1;
            snprintf(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "call_t2");
            snprintf(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), "write_file");
            snprintf(OutResp->ToolCalls[0].ArgumentsJson, sizeof(OutResp->ToolCalls[0].ArgumentsJson),
                     "{\"path\":\"C:\\\\Users\\\\Admin\\\\note.txt\",\"content\":\"hello libernt\"}");
            return 0;
        }
        snprintf(OutResp->Content, sizeof(OutResp->Content), "Created C:\\Users\\Admin\\note.txt successfully.");
        return 0;
    }

    if (Mp->Scenario == 4)
    {
        /* Do mode: delete_file -> Tier 2 approval -> denied */
        OutResp->ToolCallCount = 1;
        snprintf(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "call_del");
        snprintf(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), "delete_file");
        snprintf(OutResp->ToolCalls[0].ArgumentsJson, sizeof(OutResp->ToolCalls[0].ArgumentsJson),
                 "{\"path\":\"C:\\\\Users\\\\Admin\\\\important.txt\"}");
        return 0;
    }

    if (Mp->Scenario == 5)
    {
        /* Auto-describe folder */
        snprintf(OutResp->Content, sizeof(OutResp->Content),
                 "{\"description\":\"Quarterly financial budgets and 2026 expense receipts\",\"tags\":\"finance,taxes,2026\"}");
        return 0;
    }

    if (Mp->Scenario == 6)
    {
        /* Ask mode: read-only run_command (tasklist) -> allowed at Tier 0 */
        if (Mp->Turn == 1)
        {
            OutResp->ToolCallCount = 1;
            snprintf(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "call_ro_cmd");
            snprintf(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), "run_command");
            snprintf(OutResp->ToolCalls[0].ArgumentsJson, sizeof(OutResp->ToolCalls[0].ArgumentsJson),
                     "{\"command\":\"tasklist\",\"cwd\":\"C:\\\\Windows\",\"timeout_ms\":5000}");
            return 0;
        }
        snprintf(OutResp->Content, sizeof(OutResp->Content),
                 "Processes: %s",
                 (MsgCount > 0 && Msgs[MsgCount - 1].Content) ? Msgs[MsgCount - 1].Content : "");
        return 0;
    }

    if (Mp->Scenario == 7)
    {
        /* Do mode: mutating run_command (taskkill) -> Tier 2 approval -> approved */
        if (Mp->Turn == 1)
        {
            OutResp->ToolCallCount = 1;
            snprintf(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "call_mut_cmd");
            snprintf(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), "run_command");
            snprintf(OutResp->ToolCalls[0].ArgumentsJson, sizeof(OutResp->ToolCalls[0].ArgumentsJson),
                     "{\"command\":\"taskkill /F /IM notepad.exe\"}");
            return 0;
        }
        snprintf(OutResp->Content, sizeof(OutResp->Content), "Terminated notepad.exe.");
        return 0;
    }

    return -1;
}

static int MockEmbed(AI_PROVIDER *Self, const char *Text, float *OutVec, uint32_t MaxDim, uint32_t *OutDim)
{
    (void)Self;
    (void)Text;
    if (MaxDim < 4)
        return -1;
    OutVec[0] = 0.5f;
    OutVec[1] = 0.5f;
    OutVec[2] = 0.5f;
    OutVec[3] = 0.5f;
    *OutDim = 4;
    return 0;
}

static void MockGetConfig(AI_PROVIDER *Self, AI_PROVIDER_CONFIG *OutCfg)
{
    (void)Self;
    memset(OutCfg, 0, sizeof(*OutCfg));
    snprintf(OutCfg->EmbedModel, sizeof(OutCfg->EmbedModel), "mock-embed");
}

static void MockDestroy(AI_PROVIDER *Self)
{
    (void)Self;
}

static const AI_PROVIDER_VTBL g_MockVtbl =
{
    MockChat,
    MockEmbed,
    MockGetConfig,
    MockDestroy
};

static void TestJsonAndCodec(void)
{
    AI_JSON_BUF Buf;
    char Out[256];
    int64_t Num = 0;
    int Flag = 0;
    float Vec[8];
    uint32_t Dim = 0;

    AiJsonBufInit(&Buf, 32);
    AiJsonBufAppendStr(&Buf, "{\"text\":");
    AiJsonBufAppendJsonString(&Buf, "line1\n\"quoted\"\\path\t\x01");
    AiJsonBufAppendStr(&Buf, ",\"n\":42,\"ok\":true}");
    CHECK(!Buf.Failed && Buf.Data != NULL, "JSON builder produced buffer");
    CHECK(AiJsonObjGetString(Buf.Data, Buf.Len, "text", Out, sizeof(Out)) == 0, "extract escaped string");
    CHECK(strcmp(Out, "line1\n\"quoted\"\\path\t\x01") == 0, "roundtrip escaped string matches");
    CHECK(AiJsonObjGetInt64(Buf.Data, Buf.Len, "n", &Num) == 0 && Num == 42, "extract int64");
    CHECK(AiJsonObjGetBool(Buf.Data, Buf.Len, "ok", &Flag) == 0 && Flag == 1, "extract bool");
    AiJsonBufFree(&Buf);

    {
        const char *ChatJson =
            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
            "\"tool_calls\":[{\"id\":\"call_abc\",\"type\":\"function\","
            "\"function\":{\"name\":\"search_files\",\"arguments\":\"{\\\"query\\\":\\\"tax\\\"}\"}}]}}]}";
        AI_CHAT_RESPONSE Resp;
        CHECK(AiOpenAiParseChatResponse(ChatJson, strlen(ChatJson), &Resp) == 0, "parse OpenAI tool_call response");
        CHECK(Resp.ToolCallCount == 1, "parsed 1 tool call");
        CHECK(strcmp(Resp.ToolCalls[0].Id, "call_abc") == 0, "tool call id");
        CHECK(strcmp(Resp.ToolCalls[0].Name, "search_files") == 0, "tool call name");
        CHECK(AiJsonObjGetString(Resp.ToolCalls[0].ArgumentsJson, strlen(Resp.ToolCalls[0].ArgumentsJson),
                                 "query", Out, sizeof(Out)) == 0 && strcmp(Out, "tax") == 0,
              "parsed nested tool call argument");
    }

    {
        const char *EmbJson = "{\"data\":[{\"embedding\":[0.25,-0.5,0.75,1.0]}]}";
        CHECK(AiOpenAiParseEmbedResponse(EmbJson, strlen(EmbJson), Vec, 8, &Dim) == 0, "parse embedding response");
        CHECK(Dim == 4 && fabsf(Vec[1] - (-0.5f)) < 1e-5f, "embedding vector values match");
    }

    {
        AI_CHAT_MESSAGE Msgs[2];
        char *Req;
        memset(Msgs, 0, sizeof(Msgs));
        Msgs[0].Role = "system";
        Msgs[0].Content = "sys";
        Msgs[1].Role = "user";
        Msgs[1].Content = "hi";
        Req = AiOpenAiBuildChatRequest("test-model", Msgs, 2, AiPolicyGetToolsSchemaJson(1));
        CHECK(Req != NULL && strstr(Req, "\"test-model\"") != NULL && strstr(Req, "search_files") != NULL,
              "build OpenAI chat request with tools");
        free(Req);
    }
}

static void TestPolicyAndAgent(void)
{
    AI_STORE *Store = NULL;
    MOCK_ENV_STATE EnvState;
    AI_AGENT_ENV Env;
    MOCK_PROVIDER Prov;
    AI_AGENT *Agent = NULL;
    AI_AGENT_RESULT Res;
    AI_STORE_ENTRY Entry;
    AI_STORE_STATS Stats;
    char Desc[256];
    char Tags[128];
    char Source[32];

    CHECK(AiPolicyEvaluate("search_files", 1, 0) == AiPolicyAllow, "Tier 0 allowed in ask mode");
    CHECK(AiPolicyEvaluate("list_windows", 1, 0) == AiPolicyAllow, "Desktop Tier 0 list_windows allowed in ask mode");
    CHECK(AiPolicyEvaluate("inspect_window", 1, 0) == AiPolicyAllow, "Desktop Tier 0 inspect_window allowed in ask mode");
    CHECK(AiPolicyEvaluate("set_folder_description", 1, 0) == AiPolicyDeny, "Tier 1 denied in ask mode");
    CHECK(AiPolicyEvaluate("arrange_windows", 1, 0) == AiPolicyDeny, "Desktop Tier 1 arrange_windows denied in ask mode");
    CHECK(AiPolicyEvaluate("arrange_windows", 0, 0) == AiPolicyAllow, "Desktop Tier 1 arrange_windows allowed in do mode");
    CHECK(AiPolicyEvaluate("set_folder_description", 0, 0) == AiPolicyAllow, "Tier 1 allowed in do mode");
    CHECK(AiPolicyEvaluate("write_file", 1, 0) == AiPolicyDeny, "Tier 2 denied in ask mode");
    CHECK(AiPolicyEvaluate("write_file", 0, 0) == AiPolicyNeedApproval, "Tier 2 requires approval in do mode");
    CHECK(AiPolicyEvaluate("close_window", 0, 0) == AiPolicyNeedApproval, "Desktop Tier 2 close_window requires approval in do mode");
    CHECK(AiPolicyEvaluate("get_clipboard", 1, 0) == AiPolicyAllow, "get_clipboard Tier 0 allowed in ask mode");
    CHECK(AiPolicyEvaluate("set_clipboard", 1, 0) == AiPolicyDeny, "set_clipboard Tier 1 denied in ask mode");
    CHECK(AiPolicyEvaluate("set_clipboard", 0, 0) == AiPolicyAllow, "set_clipboard Tier 1 allowed in do mode");
    CHECK(AiPolicyEvaluate("click_control", 0, 0) == AiPolicyNeedApproval, "click_control Tier 2 requires approval");
    CHECK(AiPolicyEvaluate("send_keys", 0, 0) == AiPolicyNeedApproval, "send_keys Tier 2 requires approval");
    CHECK(AiPolicyEvaluate("power_action", 0, 0) == AiPolicyNeedApproval, "power_action Tier 2 requires approval");
    CHECK(AiPolicyEvaluate("unknown_tool", 0, 0) == AiPolicyDeny, "unknown tool denied");

    /* Fail-closed shell risk classifier checks */
    CHECK(AiPolicyClassifyCommand("tasklist") == AiTier0ReadOnly, "tasklist is Tier 0");
    CHECK(AiPolicyClassifyCommand("TASKLIST.EXE /V") == AiTier0ReadOnly, "TASKLIST.EXE is Tier 0");
    CHECK(AiPolicyClassifyCommand("sc query aisvc") == AiTier0ReadOnly, "sc query is Tier 0");
    CHECK(AiPolicyClassifyCommand("reg query HKLM\\SYSTEM") == AiTier0ReadOnly, "reg query is Tier 0");
    CHECK(AiPolicyClassifyCommand("ipconfig /all") == AiTier0ReadOnly, "ipconfig /all is Tier 0");
    CHECK(AiPolicyClassifyCommand("netstat -ano") == AiTier0ReadOnly, "netstat is Tier 0");
    CHECK(AiPolicyClassifyCommand("netsh interface show interface") == AiTier0ReadOnly, "netsh show is Tier 0");
    CHECK(AiPolicyClassifyCommand("fsutil volume diskfree C:") == AiTier0ReadOnly, "fsutil volume diskfree is Tier 0");
    CHECK(AiPolicyClassifyCommand("sc stop aisvc") == AiTier2Mutating, "sc stop is Tier 2");
    CHECK(AiPolicyClassifyCommand("reg add HKCU\\Test") == AiTier2Mutating, "reg add is Tier 2");
    CHECK(AiPolicyClassifyCommand("ipconfig /release") == AiTier2Mutating, "ipconfig /release is Tier 2");
    CHECK(AiPolicyClassifyCommand("taskkill /F /IM notepad.exe") == AiTier2Mutating, "taskkill is Tier 2");
    CHECK(AiPolicyClassifyCommand("dir & del foo.txt") == AiTier2Mutating, "chained command is Tier 2");
    CHECK(AiPolicyClassifyCommand("tasklist | findstr aisvc") == AiTier2Mutating, "piped command is Tier 2");
    CHECK(AiPolicyClassifyCommand("tasklist > out.txt") == AiTier2Mutating, "redirected command is Tier 2");
    CHECK(AiPolicyClassifyCommand("echo %PATH%") == AiTier2Mutating, "env expansion is Tier 2");
    CHECK(AiPolicyClassifyCommand("C:\\evil\\tasklist.exe") == AiTier2Mutating, "explicit path binary is Tier 2");
    CHECK(AiPolicyClassifyCommand("netsh interface set interface") == AiTier2Mutating, "netsh set is Tier 2");
    CHECK(AiPolicyEvaluateCall("run_command", "{\"command\":\"tasklist\"}", 1, 0) == AiPolicyAllow,
          "read-only run_command allowed in ask mode");
    CHECK(AiPolicyEvaluateCall("run_command", "{\"command\":\"sc stop aisvc\"}", 1, 0) == AiPolicyDeny,
          "mutating run_command denied in ask mode");
    CHECK(AiPolicyEvaluateCall("run_command", "{\"command\":\"sc stop aisvc\"}", 0, 0) == AiPolicyNeedApproval,
          "mutating run_command requires approval in do mode");

    CHECK(AiStoreOpen(":memory:", &Store) == 0, "open in-memory SQLite store");

    memset(&Entry, 0, sizeof(Entry));
    Entry.Path = "C:\\Users\\Admin\\Documents\\q4_fiscal.md";
    Entry.Name = "q4_fiscal.md";
    Entry.IsDir = 0;
    Entry.Size = 128;
    Entry.Mtime = 1000;
    Entry.Content = "Q4 fiscal summary report for LiberNT";
    CHECK(AiStoreUpsertEntry(Store, &Entry) == 0, "seed indexed file");

    memset(&EnvState, 0, sizeof(EnvState));
    memset(&Env, 0, sizeof(Env));
    Env.UserCtx = &EnvState;
    Env.ListDirectory = MockListDir;
    Env.ReadFileSnippet = MockReadSnippet;
    Env.WriteFileUtf8 = MockWriteFile;
    Env.DeleteFileUtf8 = MockDeleteFile;
    Env.RunCommandUtf8 = MockRunCommand;
    Env.WriteFolderAds = MockWriteAds;

    memset(&Prov, 0, sizeof(Prov));
    Prov.Base.Vtbl = &g_MockVtbl;

    Agent = AiAgentCreate(Store, &Prov.Base, &Env);
    CHECK(Agent != NULL, "create agent");

    /* Scenario 1: Ask mode calls search_files and returns grounded reply */
    Prov.Scenario = 1;
    Prov.Turn = 0;
    CHECK(AiAgentRunPrompt(Agent, "find fiscal report", AI_PROMPT_MODE_ASK, &Res) == AiStatusOk, "run ask prompt");
    CHECK(Res.State == AiPromptDone, "ask prompt done");
    CHECK(Res.ToolCallsExecuted == 1, "1 read-only tool executed");
    CHECK(strstr(Res.ReplyText, "q4_fiscal.md") != NULL, "reply grounded in search_files hit");

    /* Scenario 2: Ask mode denies write_file */
    Prov.Scenario = 2;
    Prov.Turn = 0;
    CHECK(AiAgentRunPrompt(Agent, "write evil file", AI_PROMPT_MODE_ASK, &Res) == AiStatusOk, "run ask deny test");
    CHECK(Res.State == AiPromptDone, "ask deny completed");
    CHECK(EnvState.Writes == 0, "write_file was NOT executed in ask mode");
    CHECK(strstr(Res.ReplyText, "denied") != NULL, "policy denial returned to model");

    /* Scenario 3: Do mode Tier 2 write_file -> pending -> approve -> executed */
    Prov.Scenario = 3;
    Prov.Turn = 0;
    CHECK(AiAgentRunPrompt(Agent, "create note.txt", AI_PROMPT_MODE_DO, &Res) == AiStatusOk, "run do write prompt");
    CHECK(Res.State == AiPromptPendingApproval, "Tier 2 paused for approval");
    CHECK(Res.ApprovalId > 0 && Res.ToolTier == 2, "valid approval id and tier 2");
    CHECK(strcmp(Res.ToolName, "write_file") == 0, "pending tool is write_file");
    CHECK(EnvState.Writes == 0, "file not written before approval");

    {
        uint32_t Id = Res.ApprovalId;
        CHECK(AiAgentResolveApproval(Agent, Id, 1, &Res) == AiStatusOk, "resolve approval yes");
        CHECK(Res.State == AiPromptDone, "agent finished after approval");
        CHECK(EnvState.Writes == 1, "file written exactly once after approval");
        CHECK(strcmp(EnvState.LastWriteContent, "hello libernt") == 0, "written content matches");
        CHECK(AiAgentResolveApproval(Agent, Id, 1, &Res) == AiStatusNotFound, "approval replay rejected");
    }

    /* Scenario 4: Do mode Tier 2 delete_file -> pending -> deny -> not executed */
    Prov.Scenario = 4;
    Prov.Turn = 0;
    CHECK(AiAgentRunPrompt(Agent, "delete important.txt", AI_PROMPT_MODE_DO, &Res) == AiStatusOk, "run do delete prompt");
    CHECK(Res.State == AiPromptPendingApproval, "delete_file paused for approval");
    CHECK(AiAgentResolveApproval(Agent, Res.ApprovalId, 0, &Res) == AiStatusOk, "resolve approval no");
    CHECK(Res.State == AiPromptDone, "denial completed cleanly");
    CHECK(EnvState.Deletes == 0, "file NOT deleted when user denied");

    /* Scenario 5: Auto-describe folder + user-pinning protection */
    Prov.Scenario = 5;
    Prov.Turn = 0;
    CHECK(AiAgentAutoDescribeFolder(Agent, "C:\\Users\\Admin\\Taxes", 0,
                                    Desc, sizeof(Desc), Tags, sizeof(Tags), Source, sizeof(Source)) == AiStatusOk,
          "auto-describe undescribed folder");
    CHECK(strcmp(Source, "ai") == 0, "auto-describe source is ai");
    CHECK(strstr(Desc, "Quarterly financial budgets") != NULL, "auto-describe text matches");
    CHECK(strstr(Tags, "finance") != NULL, "auto-describe tags match");
    CHECK(EnvState.AdsWrites == 1, "ADS write invoked");

    AiStoreGetStats(Store, &Stats);
    CHECK(Stats.EmbeddingCount == 1, "auto-describe stored embedding vector");
    CHECK(Stats.AuditCount >= 5, "audit log recorded tool and approval decisions");

    /* Pin as user, then verify auto-describe without --force refuses overwrite */
    CHECK(AiStoreSetFolderDescription(Store, "C:\\Users\\Admin\\Taxes", "My manual pin", "user", "") == 0,
          "pin user description");
    CHECK(AiAgentAutoDescribeFolder(Agent, "C:\\Users\\Admin\\Taxes", 0,
                                    Desc, sizeof(Desc), Tags, sizeof(Tags), Source, sizeof(Source)) == AiStatusAccessDenied,
          "auto-describe refuses to overwrite user-pinned description");
    CHECK(strcmp(Desc, "My manual pin") == 0, "returned pinned user description");

    /* With Force=1 it updates */
    CHECK(AiAgentAutoDescribeFolder(Agent, "C:\\Users\\Admin\\Taxes", 1,
                                    Desc, sizeof(Desc), Tags, sizeof(Tags), Source, sizeof(Source)) == AiStatusOk,
          "auto-describe --force overwrites when explicitly asked");
    CHECK(strstr(Desc, "Quarterly financial budgets") != NULL, "forced description updated");

    /* Scenario 6: Ask mode executes read-only run_command with cwd/timeout_ms directly at Tier 0 */
    Prov.Scenario = 6;
    Prov.Turn = 0;
    CHECK(AiAgentRunPrompt(Agent, "what processes are running?", AI_PROMPT_MODE_ASK, &Res) == AiStatusOk,
          "run ask read-only shell command");
    CHECK(Res.State == AiPromptDone, "read-only shell command completed without approval");
    CHECK(EnvState.CommandsRun == 1, "read-only shell command executed once");
    CHECK(strcmp(EnvState.LastCmd, "tasklist") == 0, "command string forwarded");
    CHECK(strcmp(EnvState.LastCwd, "C:\\Windows") == 0 && EnvState.LastTimeoutMs == 5000U,
          "cwd and timeout_ms forwarded");
    CHECK(strstr(Res.ReplyText, "aisvc.exe") != NULL, "reply grounded in command output");

    /* Scenario 7: Do mode gates mutating run_command at Tier 2 */
    Prov.Scenario = 7;
    Prov.Turn = 0;
    CHECK(AiAgentRunPrompt(Agent, "kill notepad", AI_PROMPT_MODE_DO, &Res) == AiStatusOk,
          "run do mutating shell command");
    CHECK(Res.State == AiPromptPendingApproval && Res.ToolTier == 2,
          "mutating shell command paused for Tier 2 approval");
    CHECK(EnvState.CommandsRun == 1, "mutating command NOT run before approval");
    CHECK(AiAgentResolveApproval(Agent, Res.ApprovalId, 1, &Res) == AiStatusOk, "approve mutating shell command");
    CHECK(Res.State == AiPromptDone && EnvState.CommandsRun == 2, "mutating command executed after approval");

    AiAgentDestroy(Agent);
    AiStoreClose(Store);
}

int main(void)
{
    TestJsonAndCodec();
    TestPolicyAndAgent();
    printf("t_agent_provider: %d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
