/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Pluggable IAiProvider interface and OpenAI-compatible codec definitions
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_MAX_TOOL_CALLS        4U
#define AI_MAX_TOOL_ID           64U
#define AI_MAX_TOOL_NAME         64U
#define AI_MAX_TOOL_ARGS         1024U
#define AI_MAX_CHAT_TEXT         4096U
#define AI_MAX_EMBED_DIM         256U

#define AI_DEFAULT_ENDPOINT      "http://10.0.2.2:11434/v1"
#define AI_DEFAULT_MODEL         "llama3.2"
#define AI_DEFAULT_EMBED_MODEL   "nomic-embed-text"
#define AI_DEFAULT_TIMEOUT_MS    30000U

typedef struct _AI_PROVIDER_CONFIG
{
    char Endpoint[256];
    char Model[128];
    char EmbedModel[128];
    char ApiKey[256];
    uint32_t TimeoutMs;
} AI_PROVIDER_CONFIG;

typedef struct _AI_TOOL_CALL
{
    char Id[AI_MAX_TOOL_ID];
    char Name[AI_MAX_TOOL_NAME];
    char ArgumentsJson[AI_MAX_TOOL_ARGS];
} AI_TOOL_CALL;

typedef struct _AI_CHAT_MESSAGE
{
    const char *Role;
    const char *Content;
    const char *ToolCallId;
    const AI_TOOL_CALL *ToolCalls;
    uint32_t ToolCallCount;
} AI_CHAT_MESSAGE;

typedef struct _AI_CHAT_RESPONSE
{
    char Content[AI_MAX_CHAT_TEXT];
    AI_TOOL_CALL ToolCalls[AI_MAX_TOOL_CALLS];
    uint32_t ToolCallCount;
    int HttpStatus;
    char ErrorText[256];
} AI_CHAT_RESPONSE;

typedef struct _AI_PROVIDER AI_PROVIDER;

typedef struct _AI_PROVIDER_VTBL
{
    int (*ChatComplete)(
        AI_PROVIDER *Self,
        const AI_CHAT_MESSAGE *Messages,
        uint32_t MessageCount,
        const char *ToolsJson,
        AI_CHAT_RESPONSE *OutResponse);

    int (*Embed)(
        AI_PROVIDER *Self,
        const char *Text,
        float *OutVec,
        uint32_t MaxDim,
        uint32_t *OutDim);

    void (*GetConfig)(
        AI_PROVIDER *Self,
        AI_PROVIDER_CONFIG *OutConfig);

    void (*Destroy)(AI_PROVIDER *Self);
} AI_PROVIDER_VTBL;

struct _AI_PROVIDER
{
    const AI_PROVIDER_VTBL *Vtbl;
};

/* Portable OpenAI-compatible JSON codec (host-testable) */
char *AiOpenAiBuildChatRequest(
    const char *Model,
    const AI_CHAT_MESSAGE *Messages,
    uint32_t MessageCount,
    const char *ToolsJson);

int AiOpenAiParseChatResponse(
    const char *Json,
    size_t JsonLen,
    AI_CHAT_RESPONSE *OutResponse);

char *AiOpenAiBuildEmbedRequest(
    const char *Model,
    const char *Text);

int AiOpenAiParseEmbedResponse(
    const char *Json,
    size_t JsonLen,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim);

/* WinHTTP OpenAI-compatible provider & config persistence (Windows/LiberNT) */
#ifdef _WIN32
void AiProviderDefaultConfig(AI_PROVIDER_CONFIG *Cfg);
void AiProviderLoadConfigFromRegistry(AI_PROVIDER_CONFIG *Cfg);
int AiProviderSaveConfigToRegistry(const AI_PROVIDER_CONFIG *Cfg);
AI_PROVIDER *AiProviderCreateOpenAi(const AI_PROVIDER_CONFIG *Cfg);
#endif

#ifdef __cplusplus
}
#endif
