/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Local on-device GGUF inference provider behind IAiProvider (llama.dll + built-in fallback)
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

typedef void *(*PFN_AI_LLAMA_OPEN)(
    const char *ModelPath,
    uint32_t ContextLen,
    uint32_t Threads,
    char *ErrBuf,
    size_t ErrCap);

typedef void (*PFN_AI_LLAMA_CLOSE)(void *Handle);

typedef void (*PFN_AI_LLAMA_GET_INFO)(
    void *Handle,
    char *ModelDescOut,
    size_t DescCap,
    char *BackendOut,
    size_t BackendCap);

typedef int (*PFN_AI_LLAMA_CHAT)(
    void *Handle,
    const char **Roles,
    const char **Contents,
    uint32_t MsgCount,
    uint32_t MaxTokens,
    char *OutUtf8,
    size_t OutCap,
    char *ErrBuf,
    size_t ErrCap);

typedef int (*PFN_AI_LLAMA_EMBED)(
    void *Handle,
    const char *TextUtf8,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim);

typedef struct _AI_LLAMA_DLL
{
#ifdef _WIN32
    HMODULE Module;
#endif
    PFN_AI_LLAMA_OPEN Open;
    PFN_AI_LLAMA_CLOSE Close;
    PFN_AI_LLAMA_GET_INFO GetInfo;
    PFN_AI_LLAMA_CHAT Chat;
    PFN_AI_LLAMA_EMBED Embed;
} AI_LLAMA_DLL;

typedef struct _AI_LOCAL_PROVIDER
{
    AI_PROVIDER Base;
    AI_PROVIDER_CONFIG Cfg;
    AI_LLAMA_DLL LlamaDll;
    void *LlamaHandle;
    AI_GGUF_MODEL *Model;
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

static int TryLoadLlamaDll(AI_LLAMA_DLL *Dll)
{
    memset(Dll, 0, sizeof(*Dll));
#ifdef _WIN32
    Dll->Module = LoadLibraryW(L"llama.dll");
    if (Dll->Module == NULL)
        return 0;

    Dll->Open = (PFN_AI_LLAMA_OPEN)(void *)GetProcAddress(Dll->Module, "AiLlamaOpen");
    Dll->Close = (PFN_AI_LLAMA_CLOSE)(void *)GetProcAddress(Dll->Module, "AiLlamaClose");
    Dll->GetInfo = (PFN_AI_LLAMA_GET_INFO)(void *)GetProcAddress(Dll->Module, "AiLlamaGetInfo");
    Dll->Chat = (PFN_AI_LLAMA_CHAT)(void *)GetProcAddress(Dll->Module, "AiLlamaChat");
    Dll->Embed = (PFN_AI_LLAMA_EMBED)(void *)GetProcAddress(Dll->Module, "AiLlamaEmbed");

    if (Dll->Open && Dll->Close && Dll->GetInfo && Dll->Chat && Dll->Embed)
        return 1;

    FreeLibrary(Dll->Module);
    memset(Dll, 0, sizeof(*Dll));
#endif
    return 0;
}

static void UnloadLlamaDll(AI_LLAMA_DLL *Dll)
{
#ifdef _WIN32
    if (Dll->Module != NULL)
        FreeLibrary(Dll->Module);
#endif
    memset(Dll, 0, sizeof(*Dll));
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
    char Generated[AI_MAX_CHAT_TEXT];
    uint32_t MaxTok;

    if (Prov == NULL || OutResponse == NULL)
        return -1;
    memset(OutResponse, 0, sizeof(*OutResponse));
    MaxTok = Prov->Cfg.MaxTokens ? Prov->Cfg.MaxTokens : AI_DEFAULT_MAX_TOKENS;

    /* Primary path: real llama.cpp via llama.dll */
    if (Prov->LlamaHandle != NULL && Prov->LlamaDll.Chat != NULL)
    {
        const char *Roles[24];
        const char *Contents[24];
        AI_JSON_BUF SysWithTools;
        uint32_t Count = (MessageCount > 24U) ? 24U : MessageCount;
        uint32_t i;
        int Rc;

        AiJsonBufInit(&SysWithTools, 0);
        for (i = 0; i < Count; i++)
        {
            Roles[i] = Messages[i].Role ? Messages[i].Role : "user";
            Contents[i] = Messages[i].Content ? Messages[i].Content : "";
            if (i == 0 && ToolsJson != NULL && ToolsJson[0] != '\0' && strcmp(Roles[i], "system") == 0)
            {
                AiJsonBufInit(&SysWithTools, 512);
                AiJsonBufAppendStr(&SysWithTools, Contents[i]);
                AiJsonBufAppendStr(&SysWithTools,
                    "\nIf you need to call a tool, reply ONLY with "
                    "<tool_call>{\"name\":\"...\",\"arguments\":{...}}</tool_call>.\n");
                if (SysWithTools.Data != NULL && !SysWithTools.Failed)
                    Contents[i] = SysWithTools.Data;
            }
        }

        Generated[0] = '\0';
        Rc = Prov->LlamaDll.Chat(
            Prov->LlamaHandle,
            Roles,
            Contents,
            Count,
            MaxTok,
            Generated,
            sizeof(Generated),
            OutResponse->ErrorText,
            sizeof(OutResponse->ErrorText));
        AiJsonBufFree(&SysWithTools);
        if (Rc != 0)
        {
            if (OutResponse->ErrorText[0] == '\0')
                CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "llama.dll chat failed");
            return -1;
        }

        TrimWhitespace(Generated);
        OutResponse->HttpStatus = 200;
        if (!TryParseLocalToolCall(Generated, OutResponse))
            CopyStrSafe(OutResponse->Content, sizeof(OutResponse->Content), Generated);
        return 0;
    }

    /* Fallback path: built-in C11 GGUF v3 transformer engine */
    if (Prov->Model == NULL)
    {
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText),
                    Prov->LoadError[0] ? Prov->LoadError : "local GGUF model not loaded");
        return -1;
    }

    {
        AI_JSON_BUF Prompt;
        uint32_t i;

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

        Generated[0] = '\0';
        if (AiGgufGenerate(Prov->Model, Prompt.Data, MaxTok, Generated, sizeof(Generated)) != 0)
        {
            AiJsonBufFree(&Prompt);
            CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "local GGUF generation failed");
            return -1;
        }
        AiJsonBufFree(&Prompt);
    }

    TrimWhitespace(Generated);
    OutResponse->HttpStatus = 200;

    if (!TryParseLocalToolCall(Generated, OutResponse))
        CopyStrSafe(OutResponse->Content, sizeof(OutResponse->Content), Generated);

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

    if (Prov == NULL)
        return -1;
    if (Prov->LlamaHandle != NULL && Prov->LlamaDll.Embed != NULL)
        return Prov->LlamaDll.Embed(Prov->LlamaHandle, Text, OutVec, MaxDim, OutDim);
    if (Prov->Model != NULL)
        return AiGgufEmbed(Prov->Model, Text, OutVec, MaxDim, OutDim);
    return -1;
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
    if (Prov->LlamaHandle != NULL && Prov->LlamaDll.Close != NULL)
        Prov->LlamaDll.Close(Prov->LlamaHandle);
    UnloadLlamaDll(&Prov->LlamaDll);
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
    uint32_t Threads;

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
    Threads = Prov->Cfg.Threads ? Prov->Cfg.Threads : 4U;

    /* 1. Prefer real llama.cpp via llama.dll when present */
    if (TryLoadLlamaDll(&Prov->LlamaDll))
    {
        Prov->LlamaHandle = Prov->LlamaDll.Open(
            Prov->Cfg.LocalModelPath, 2048U, Threads, Prov->LoadError, sizeof(Prov->LoadError));
        if (Prov->LlamaHandle != NULL)
        {
            char Desc[128];
            char Backend[160];

            Desc[0] = '\0';
            Backend[0] = '\0';
            Prov->LlamaDll.GetInfo(Prov->LlamaHandle, Desc, sizeof(Desc), Backend, sizeof(Backend));
            if (Desc[0] != '\0')
                CopyStrSafe(Prov->Cfg.Model, sizeof(Prov->Cfg.Model), Desc);
            CopyStrSafe(Prov->Cfg.BackendInfo, sizeof(Prov->Cfg.BackendInfo),
                        Backend[0] ? Backend : "llama.cpp (llama.dll)");
            CopyStrSafe(Prov->Cfg.EmbedModel, sizeof(Prov->Cfg.EmbedModel), "llama.cpp");
            return &Prov->Base;
        }
    }

    /* 2. Fall back to built-in zero-dependency C11 GGUF engine */
    if (AiGgufLoadFile(Prov->Cfg.LocalModelPath, &Prov->Model,
                       Prov->LoadError, sizeof(Prov->LoadError)) == 0)
    {
        AI_GGUF_INFO Info;
        AiGgufGetInfo(Prov->Model, &Info);
        snprintf(Prov->Cfg.BackendInfo, sizeof(Prov->Cfg.BackendInfo),
                 "gguf-native (%s, %s, %uL, d=%u, v=%u, %s)",
                 Info.Avx2Enabled ? "AVX2+FMA" : "scalar",
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
                 "local (not loaded: %.120s)",
                 Prov->LoadError[0] ? Prov->LoadError : "missing model");
    }

    return &Prov->Base;
}
