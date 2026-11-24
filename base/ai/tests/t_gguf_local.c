/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host-native ASan/UBSan tests for GGUF v3 engine (F32/F16/Q8_0/Q4_0) and local IAiProvider
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "ai_provider.h"
#include "ai_store.h"
#include "gguf_engine.h"

static int g_Checks = 0;
static int g_Failures = 0;

#define CHECK(cond, msg) do { \
    g_Checks++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        g_Failures++; \
    } \
} while (0)

static void TestFp16Conversion(void)
{
    CHECK(fabsf(AiGgufHalfToFloat(0x0000) - 0.0f) < 1e-6f, "fp16 +0.0");
    CHECK(fabsf(AiGgufHalfToFloat(0x3C00) - 1.0f) < 1e-6f, "fp16 +1.0");
    CHECK(fabsf(AiGgufHalfToFloat(0xBC00) - (-1.0f)) < 1e-6f, "fp16 -1.0");
    CHECK(fabsf(AiGgufHalfToFloat(0x3800) - 0.5f) < 1e-6f, "fp16 +0.5");
    CHECK(fabsf(AiGgufHalfToFloat(0x4000) - 2.0f) < 1e-6f, "fp16 +2.0");
}

static void TestGgufQuantModel(const char *Path, const char *ExpectedQuant)
{
    AI_GGUF_MODEL *Model = NULL;
    AI_GGUF_INFO Info;
    char Err[128];
    char Out[512];
    float EmbA[64], EmbB[64], EmbC[64];
    uint32_t DimA = 0, DimB = 0, DimC = 0;
    float NormA = 0.0f;
    uint32_t i;

    CHECK(AiGgufLoadFile(Path, &Model, Err, sizeof(Err)) == 0, "load GGUF model");
    if (Model == NULL)
    {
        fprintf(stderr, "  load error for %s: %s\n", Path, Err);
        return;
    }

    AiGgufGetInfo(Model, &Info);
    CHECK(Info.Version == 3, "GGUF v3 header parsed");
    CHECK(Info.LayerCount == 2 && Info.EmbedDim == 32 && Info.HeadCount == 4 && Info.KvHeadCount == 2,
          "Llama GQA hyperparameters parsed");
    CHECK(strcmp(Info.QuantSummary, ExpectedQuant) == 0, "quant summary matches");

    CHECK(AiGgufGenerate(Model, "<|im_start|>user\nhello<|im_end|>\n<|im_start|>assistant\n",
                         32, Out, sizeof(Out)) == 0, "autoregressive GGUF generation succeeded");
    CHECK(strstr(Out, "LiberNT local on-device GGUF transformer active.") != NULL,
          "generated output matches expected multi-token sequence");

    CHECK(AiGgufEmbed(Model, "quarterly taxes receipts", EmbA, 64, &DimA) == 0 && DimA == 32, "embed text A");
    CHECK(AiGgufEmbed(Model, "quarterly taxes receipts", EmbB, 64, &DimB) == 0 && DimB == 32, "embed text B");
    CHECK(AiGgufEmbed(Model, "kernel drivers project", EmbC, 64, &DimC) == 0 && DimC == 32, "embed text C");

    for (i = 0; i < DimA; i++)
        NormA += EmbA[i] * EmbA[i];
    CHECK(fabsf(sqrtf(NormA) - 1.0f) < 1e-4f, "embedding vector is L2 normalized");
    CHECK(AiStoreCosineSimilarity(EmbA, EmbB, DimA) > 0.999f, "identical text cosine similarity ~1.0");
    CHECK(AiStoreCosineSimilarity(EmbA, EmbC, DimA) < 0.75f, "distinct text cosine similarity lower");

    AiGgufFree(Model);
}

static int MockListDir(void *Ctx, const char *Dir, char *Out, size_t Cap)
{
    (void)Ctx;
    (void)Dir;
    snprintf(Out, Cap, "[FILE] taxes_2026.csv (200 bytes)\n");
    return 0;
}

static int MockWriteAds(void *Ctx, const char *Dir, const char *Desc, const char *Source, const char *Tags)
{
    (void)Ctx;
    (void)Dir;
    (void)Desc;
    (void)Source;
    (void)Tags;
    return 0;
}

static void TestLocalProviderAndOfflineAgent(const char *ChatModelPath, const char *ToolModelPath)
{
    AI_PROVIDER_CONFIG Cfg;
    AI_PROVIDER *Prov = NULL;
    AI_STORE *Store = NULL;
    AI_AGENT_ENV Env;
    AI_AGENT *Agent = NULL;
    AI_AGENT_RESULT Res;
    AI_STORE_STATS Stats;
    char Desc[256], Tags[128], Source[32];

    /* 1. Missing model fails gracefully */
    {
        AI_GGUF_MODEL *Bad = NULL;
        char Err[128];
        CHECK(AiGgufLoadFile("/tmp/nonexistent_libernt.gguf", &Bad, Err, sizeof(Err)) != 0 && Bad == NULL,
              "missing GGUF fails cleanly");
    }

    /* 2. Local chat provider + offline AiAgentAutoDescribeFolder + neural embedding storage */
    memset(&Cfg, 0, sizeof(Cfg));
    snprintf(Cfg.ProviderType, sizeof(Cfg.ProviderType), "local");
    snprintf(Cfg.LocalModelPath, sizeof(Cfg.LocalModelPath), "%s", ChatModelPath);
    Cfg.MaxTokens = 32;

    Prov = AiProviderCreateLocal(&Cfg);
    CHECK(Prov != NULL, "create local GGUF provider");
    if (Prov == NULL)
        return;
    Prov->Vtbl->GetConfig(Prov, &Cfg);
    CHECK(strstr(Cfg.BackendInfo, "gguf-native") != NULL, "BackendInfo reports gguf-native");

    CHECK(AiStoreOpen(":memory:", &Store) == 0, "open in-memory SQLite store");
    memset(&Env, 0, sizeof(Env));
    Env.ListDirectory = MockListDir;
    Env.WriteFolderAds = MockWriteAds;

    Agent = AiAgentCreate(Store, Prov, &Env);
    CHECK(Agent != NULL, "create offline agent");

    CHECK(AiAgentRunPrompt(Agent, "hello local model", AI_PROMPT_MODE_ASK, &Res) == AiStatusOk,
          "offline agent ask succeeded");
    CHECK(Res.State == AiPromptDone, "offline agent state done");
    CHECK(strstr(Res.ReplyText, "LiberNT local on-device GGUF transformer active.") != NULL,
          "offline agent reply came from local GGUF");

    CHECK(AiAgentAutoDescribeFolder(Agent, "C:\\Users\\Admin\\LocalDocs", 0,
                                    Desc, sizeof(Desc), Tags, sizeof(Tags), Source, sizeof(Source)) == AiStatusOk,
          "offline auto-describe folder succeeded");
    CHECK(strcmp(Source, "ai") == 0 && strstr(Desc, "LiberNT local") != NULL,
          "offline auto-describe saved local GGUF output");

    AiStoreGetStats(Store, &Stats);
    CHECK(Stats.EmbeddingCount == 1, "offline auto-describe stored real 32-dim GGUF embedding in SQLite");

    AiAgentDestroy(Agent);
    Prov->Vtbl->Destroy(Prov);

    /* 3. Local tool-calling provider parses <tool_call> and invokes Tier 0 system_status offline */
    memset(&Cfg, 0, sizeof(Cfg));
    snprintf(Cfg.ProviderType, sizeof(Cfg.ProviderType), "local");
    snprintf(Cfg.LocalModelPath, sizeof(Cfg.LocalModelPath), "%s", ToolModelPath);
    Cfg.MaxTokens = 32;

    Prov = AiProviderCreateLocal(&Cfg);
    CHECK(Prov != NULL, "create local tool GGUF provider");
    if (Prov == NULL)
    {
        AiStoreClose(Store);
        return;
    }
    {
        AI_CHAT_MESSAGE Msgs[1];
        AI_CHAT_RESPONSE ChatResp;
        memset(Msgs, 0, sizeof(Msgs));
        Msgs[0].Role = "user";
        Msgs[0].Content = "check system status";
        CHECK(Prov->Vtbl->ChatComplete(Prov, Msgs, 1, NULL, &ChatResp) == 0, "local tool chat succeeded");
        CHECK(ChatResp.ToolCallCount == 1 && strcmp(ChatResp.ToolCalls[0].Name, "system_status") == 0,
              "local GGUF <tool_call> parsed into structured AI_TOOL_CALL");
    }

    Prov->Vtbl->Destroy(Prov);
    AiStoreClose(Store);
}

int main(void)
{
    TestFp16Conversion();
    TestGgufQuantModel("/tmp/test_chat_f32.gguf", "F32");
    TestGgufQuantModel("/tmp/test_chat_f16.gguf", "F16");
    TestGgufQuantModel("/tmp/test_chat_q8_0.gguf", "Q8_0");
    TestGgufQuantModel("/tmp/test_chat_q4_0.gguf", "Q4_0");
    TestLocalProviderAndOfflineAgent("/tmp/test_chat_q8_0.gguf", "/tmp/test_tool_f32.gguf");
    printf("t_gguf_local: %d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
