/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Local on-device GGUF inference provider behind IAiProvider (with optional llama.dll hook)
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <windef.h>
#include <winbase.h>
#endif

#include "ai_json.h"
#include "ai_provider.h"
#include "gguf_engine.h"

typedef struct _AI_LOCAL_PROVIDER
{
    AI_PROVIDER Base;
    AI_PROVIDER_CONFIG Cfg;
    AI_GGUF_MODEL *Model;
    int HasExternalLlamaDll;
    char LoadError[160];
} AI_LOCAL_PROVIDER;

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

static void TrimWhitespace(char *Text)
{
    size_t Len;
    char *Start = Text;

    if (Text == NULL)
        return;
    while (*Start == ' ' || *Start == '\t' || *Start == '\r' || *Start == '\n')
        Start++;
    if (Start != Text)
        memmove(Text, Start, strlen(Start) + 1);
    Len = strlen(Text);
    while (Len > 0 && (Text[Len - 1] == ' ' || Text[Len - 1] == '\t' ||
                       Text[Len - 1] == '\r' || Text[Len - 1] == '\n'))
    {
        Text[--Len] = '\0';
    }
}

static int TryParseLocalToolCall(const char *Raw, AI_CHAT_RESPONSE *OutResp)
{
    const char *OpenTag;
    const char *CloseTag;
    const char *JsonStart;
    size_t JsonLen;
    char ToolName[AI_MAX_TOOL_NAME];
    AI_JSON_SPAN ArgsSpan;

    OpenTag = strstr(Raw, "<tool_call>");
    if (OpenTag == NULL)
        return 0;

    JsonStart = OpenTag + 11;
    CloseTag = strstr(JsonStart, "</tool_call>");
    JsonLen = (CloseTag != NULL) ? (size_t)(CloseTag - JsonStart) : strlen(JsonStart);

    if (AiJsonObjGetString(JsonStart, JsonLen, "name", ToolName, sizeof(ToolName)) != 0 ||
        ToolName[0] == '\0')
    {
        return 0;
    }

    OutResp->ToolCallCount = 1;
    CopyStrSafe(OutResp->ToolCalls[0].Id, sizeof(OutResp->ToolCalls[0].Id), "local_call_1");
    CopyStrSafe(OutResp->ToolCalls[0].Name, sizeof(OutResp->ToolCalls[0].Name), ToolName);

    if (AiJsonObjGet(JsonStart, JsonLen, "arguments", &ArgsSpan) == 0)
    {
        if (ArgsSpan.Len >= 2 && ArgsSpan.Start[0] == '"')
        {
            AiJsonUnescapeString(ArgsSpan.Start, ArgsSpan.Len,
                                 OutResp->ToolCalls[0].ArgumentsJson,
                                 sizeof(OutResp->ToolCalls[0].ArgumentsJson));
        }
        else
        {
            size_t CopyLen = ArgsSpan.Len;
            if (CopyLen >= sizeof(OutResp->ToolCalls[0].ArgumentsJson))
                CopyLen = sizeof(OutResp->ToolCalls[0].ArgumentsJson) - 1;
            memcpy(OutResp->ToolCalls[0].ArgumentsJson, ArgsSpan.Start, CopyLen);
            OutResp->ToolCalls[0].ArgumentsJson[CopyLen] = '\0';
        }
    }
    else
    {
        CopyStrSafe(OutResp->ToolCalls[0].ArgumentsJson,
                    sizeof(OutResp->ToolCalls[0].ArgumentsJson), "{}");
    }

    return 1;
}

static int LocalChatComplete(
    AI_PROVIDER *Self,
    const AI_CHAT_MESSAGE *Messages,
    uint32_t MessageCount,
    const char *ToolsJson,
    AI_CHAT_RESPONSE *OutResponse)
{
    AI_LOCAL_PROVIDER *Prov = (AI_LOCAL_PROVIDER *)Self;
    AI_JSON_BUF Prompt;
    char Generated[AI_MAX_CHAT_TEXT];
    uint32_t i;
    uint32_t MaxTok;

    if (Prov == NULL || OutResponse == NULL)
        return -1;
    memset(OutResponse, 0, sizeof(*OutResponse));

    if (Prov->Model == NULL)
    {
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText),
                    Prov->LoadError[0] ? Prov->LoadError : "local GGUF model not loaded");
        return -1;
    }

    AiJsonBufInit(&Prompt, 1024);
    for (i = 0; i < MessageCount; i++)
    {
        const AI_CHAT_MESSAGE *Msg = &Messages[i];
        AiJsonBufAppendStr(&Prompt, "<|im_start|>");
        AiJsonBufAppendStr(&Prompt, Msg->Role ? Msg->Role : "user");
        AiJsonBufAppendChar(&Prompt, '\n');
        if (Msg->Content != NULL)
            AiJsonBufAppendStr(&Prompt, Msg->Content);
        if (i == 0 && ToolsJson != NULL && ToolsJson[0] != '\0')
        {
            AiJsonBufAppendStr(&Prompt, "\nAvailable tools: ");
            AiJsonBufAppendStr(&Prompt, ToolsJson);
        }
        AiJsonBufAppendStr(&Prompt, "<|im_end|>\n");
    }
    AiJsonBufAppendStr(&Prompt, "<|im_start|>assistant\n");

    if (Prompt.Failed || Prompt.Data == NULL)
    {
        AiJsonBufFree(&Prompt);
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "failed to build local prompt");
        return -1;
    }

    MaxTok = Prov->Cfg.MaxTokens ? Prov->Cfg.MaxTokens : AI_DEFAULT_MAX_TOKENS;
    Generated[0] = '\0';
    if (AiGgufGenerate(Prov->Model, Prompt.Data, MaxTok, Generated, sizeof(Generated)) != 0)
    {
        AiJsonBufFree(&Prompt);
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "local GGUF generation failed");
        return -1;
    }
    AiJsonBufFree(&Prompt);

    TrimWhitespace(Generated);
    OutResponse->HttpStatus = 200;

    if (!TryParseLocalToolCall(Generated, OutResponse))
    {
        CopyStrSafe(OutResponse->Content, sizeof(OutResponse->Content), Generated);
    }

    return 0;
}

static int LocalEmbed(
    AI_PROVIDER *Self,
    const char *Text,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim)
{
    AI_LOCAL_PROVIDER *Prov = (AI_LOCAL_PROVIDER *)Self;

    if (Prov == NULL || Prov->Model == NULL)
        return -1;
    return AiGgufEmbed(Prov->Model, Text, OutVec, MaxDim, OutDim);
}

static void LocalGetConfig(AI_PROVIDER *Self, AI_PROVIDER_CONFIG *OutConfig)
{
    AI_LOCAL_PROVIDER *Prov = (AI_LOCAL_PROVIDER *)Self;
    if (Prov == NULL || OutConfig == NULL)
        return;
    *OutConfig = Prov->Cfg;
}

static void LocalDestroy(AI_PROVIDER *Self)
{
    AI_LOCAL_PROVIDER *Prov = (AI_LOCAL_PROVIDER *)Self;
    if (Prov == NULL)
        return;
    AiGgufFree(Prov->Model);
    free(Prov);
}

static const AI_PROVIDER_VTBL g_LocalVtbl =
{
    LocalChatComplete,
    LocalEmbed,
    LocalGetConfig,
    LocalDestroy
};

AI_PROVIDER *AiProviderCreateLocal(const AI_PROVIDER_CONFIG *Cfg)
{
    AI_LOCAL_PROVIDER *Prov;

    Prov = (AI_LOCAL_PROVIDER *)calloc(1, sizeof(*Prov));
    if (Prov == NULL)
        return NULL;

    Prov->Base.Vtbl = &g_LocalVtbl;
    if (Cfg != NULL)
        Prov->Cfg = *Cfg;
    CopyStrSafe(Prov->Cfg.ProviderType, sizeof(Prov->Cfg.ProviderType), AI_PROVIDER_LOCAL);
    if (Prov->Cfg.LocalModelPath[0] == '\0')
        CopyStrSafe(Prov->Cfg.LocalModelPath, sizeof(Prov->Cfg.LocalModelPath), AI_DEFAULT_LOCAL_MODEL);
    if (Prov->Cfg.MaxTokens == 0)
        Prov->Cfg.MaxTokens = AI_DEFAULT_MAX_TOKENS;

#ifdef _WIN32
    {
        HMODULE hLlama = LoadLibraryW(L"llama.dll");
        if (hLlama != NULL)
        {
            Prov->HasExternalLlamaDll = 1;
            FreeLibrary(hLlama);
        }
    }
#endif

    if (AiGgufLoadFile(Prov->Cfg.LocalModelPath, &Prov->Model,
                       Prov->LoadError, sizeof(Prov->LoadError)) == 0)
    {
        AI_GGUF_INFO Info;
        AiGgufGetInfo(Prov->Model, &Info);
        snprintf(Prov->Cfg.BackendInfo, sizeof(Prov->Cfg.BackendInfo),
                 "gguf-native (%s%s, %s, %uL, d=%u, v=%u, %s)",
                 Info.Avx2Enabled ? "AVX2+FMA" : "scalar",
                 Prov->HasExternalLlamaDll ? "+llama.dll" : "",
                 Info.Arch,
                 Info.LayerCount,
                 Info.EmbedDim,
                 Info.VocabSize,
                 Info.QuantSummary);
        if (Info.Name[0] != '\0')
            CopyStrSafe(Prov->Cfg.Model, sizeof(Prov->Cfg.Model), Info.Name);
        CopyStrSafe(Prov->Cfg.EmbedModel, sizeof(Prov->Cfg.EmbedModel), "local-gguf");
    }
    else
    {
        snprintf(Prov->Cfg.BackendInfo, sizeof(Prov->Cfg.BackendInfo),
                 "gguf-native (not loaded: %.120s)",
                 Prov->LoadError[0] ? Prov->LoadError : "missing model");
    }

    return &Prov->Base;
}
