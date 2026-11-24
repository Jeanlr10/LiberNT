/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Portable OpenAI-compatible /v1/chat/completions and /v1/embeddings JSON codec
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_json.h"
#include "ai_provider.h"

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

char *AiOpenAiBuildChatRequest(
    const char *Model,
    const AI_CHAT_MESSAGE *Messages,
    uint32_t MessageCount,
    const char *ToolsJson)
{
    AI_JSON_BUF Buf;
    uint32_t i;

    if (Model == NULL || Messages == NULL || MessageCount == 0)
        return NULL;

    AiJsonBufInit(&Buf, 1024);
    AiJsonBufAppendStr(&Buf, "{\"model\":");
    AiJsonBufAppendJsonString(&Buf, Model);
    AiJsonBufAppendStr(&Buf, ",\"temperature\":0.2,\"messages\":[");

    for (i = 0; i < MessageCount; i++)
    {
        const AI_CHAT_MESSAGE *Msg = &Messages[i];

        if (i > 0)
            AiJsonBufAppendChar(&Buf, ',');
        AiJsonBufAppendStr(&Buf, "{\"role\":");
        AiJsonBufAppendJsonString(&Buf, Msg->Role ? Msg->Role : "user");

        if (Msg->Content != NULL)
        {
            AiJsonBufAppendStr(&Buf, ",\"content\":");
            AiJsonBufAppendJsonString(&Buf, Msg->Content);
        }
        else
        {
            AiJsonBufAppendStr(&Buf, ",\"content\":null");
        }

        if (Msg->ToolCallId != NULL && Msg->ToolCallId[0] != '\0')
        {
            AiJsonBufAppendStr(&Buf, ",\"tool_call_id\":");
            AiJsonBufAppendJsonString(&Buf, Msg->ToolCallId);
        }

        if (Msg->ToolCalls != NULL && Msg->ToolCallCount > 0)
        {
            uint32_t t;
            AiJsonBufAppendStr(&Buf, ",\"tool_calls\":[");
            for (t = 0; t < Msg->ToolCallCount; t++)
            {
                if (t > 0)
                    AiJsonBufAppendChar(&Buf, ',');
                AiJsonBufAppendStr(&Buf, "{\"id\":");
                AiJsonBufAppendJsonString(&Buf, Msg->ToolCalls[t].Id);
                AiJsonBufAppendStr(&Buf, ",\"type\":\"function\",\"function\":{\"name\":");
                AiJsonBufAppendJsonString(&Buf, Msg->ToolCalls[t].Name);
                AiJsonBufAppendStr(&Buf, ",\"arguments\":");
                AiJsonBufAppendJsonString(&Buf, Msg->ToolCalls[t].ArgumentsJson);
                AiJsonBufAppendStr(&Buf, "}}");
            }
            AiJsonBufAppendChar(&Buf, ']');
        }

        AiJsonBufAppendChar(&Buf, '}');
    }

    AiJsonBufAppendChar(&Buf, ']');
    if (ToolsJson != NULL && ToolsJson[0] != '\0')
    {
        AiJsonBufAppendStr(&Buf, ",\"tools\":");
        AiJsonBufAppendStr(&Buf, ToolsJson);
    }
    AiJsonBufAppendChar(&Buf, '}');

    if (Buf.Failed)
    {
        AiJsonBufFree(&Buf);
        return NULL;
    }
    return Buf.Data;
}

int AiOpenAiParseChatResponse(
    const char *Json,
    size_t JsonLen,
    AI_CHAT_RESPONSE *OutResponse)
{
    AI_JSON_SPAN Choices;
    AI_JSON_SPAN Choice0;
    AI_JSON_SPAN Message;
    AI_JSON_SPAN ToolCalls;
    AI_JSON_SPAN ErrObj;

    if (Json == NULL || OutResponse == NULL)
        return -1;

    OutResponse->Content[0] = '\0';
    OutResponse->ToolCallCount = 0;

    if (AiJsonObjGet(Json, JsonLen, "error", &ErrObj) == 0)
    {
        if (AiJsonObjGetString(ErrObj.Start, ErrObj.Len, "message",
                               OutResponse->ErrorText, sizeof(OutResponse->ErrorText)) != 0)
        {
            CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "provider returned error");
        }
        return -1;
    }

    if (AiJsonObjGet(Json, JsonLen, "choices", &Choices) != 0)
    {
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "missing choices array");
        return -1;
    }
    if (AiJsonArrayGet(Choices.Start, Choices.Len, 0, &Choice0) != 0)
    {
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "empty choices array");
        return -1;
    }
    if (AiJsonObjGet(Choice0.Start, Choice0.Len, "message", &Message) != 0)
    {
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "missing message object");
        return -1;
    }

    AiJsonObjGetString(Message.Start, Message.Len, "content",
                       OutResponse->Content, sizeof(OutResponse->Content));

    if (AiJsonObjGet(Message.Start, Message.Len, "tool_calls", &ToolCalls) == 0)
    {
        size_t Count = AiJsonArrayCount(ToolCalls.Start, ToolCalls.Len);
        size_t i;

        if (Count > AI_MAX_TOOL_CALLS)
            Count = AI_MAX_TOOL_CALLS;

        for (i = 0; i < Count; i++)
        {
            AI_JSON_SPAN CallSpan;
            AI_JSON_SPAN FuncSpan;
            AI_JSON_SPAN ArgsSpan;
            AI_TOOL_CALL *Dst = &OutResponse->ToolCalls[OutResponse->ToolCallCount];

            if (AiJsonArrayGet(ToolCalls.Start, ToolCalls.Len, i, &CallSpan) != 0)
                continue;
            memset(Dst, 0, sizeof(*Dst));

            if (AiJsonObjGetString(CallSpan.Start, CallSpan.Len, "id", Dst->Id, sizeof(Dst->Id)) != 0)
                snprintf(Dst->Id, sizeof(Dst->Id), "call_%u", (unsigned int)i);

            if (AiJsonObjGet(CallSpan.Start, CallSpan.Len, "function", &FuncSpan) != 0)
                continue;
            if (AiJsonObjGetString(FuncSpan.Start, FuncSpan.Len, "name", Dst->Name, sizeof(Dst->Name)) != 0)
                continue;

            if (AiJsonObjGet(FuncSpan.Start, FuncSpan.Len, "arguments", &ArgsSpan) == 0)
            {
                if (ArgsSpan.Len >= 2 && ArgsSpan.Start[0] == '"')
                {
                    AiJsonUnescapeString(ArgsSpan.Start, ArgsSpan.Len,
                                         Dst->ArgumentsJson, sizeof(Dst->ArgumentsJson));
                }
                else
                {
                    size_t CopyLen = ArgsSpan.Len;
                    if (CopyLen >= sizeof(Dst->ArgumentsJson))
                        CopyLen = sizeof(Dst->ArgumentsJson) - 1;
                    memcpy(Dst->ArgumentsJson, ArgsSpan.Start, CopyLen);
                    Dst->ArgumentsJson[CopyLen] = '\0';
                }
            }
            else
            {
                CopyStrSafe(Dst->ArgumentsJson, sizeof(Dst->ArgumentsJson), "{}");
            }

            OutResponse->ToolCallCount++;
        }
    }

    return 0;
}

char *AiOpenAiBuildEmbedRequest(
    const char *Model,
    const char *Text)
{
    AI_JSON_BUF Buf;

    if (Model == NULL || Text == NULL)
        return NULL;

    AiJsonBufInit(&Buf, 256);
    AiJsonBufAppendStr(&Buf, "{\"model\":");
    AiJsonBufAppendJsonString(&Buf, Model);
    AiJsonBufAppendStr(&Buf, ",\"input\":");
    AiJsonBufAppendJsonString(&Buf, Text);
    AiJsonBufAppendChar(&Buf, '}');

    if (Buf.Failed)
    {
        AiJsonBufFree(&Buf);
        return NULL;
    }
    return Buf.Data;
}

int AiOpenAiParseEmbedResponse(
    const char *Json,
    size_t JsonLen,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim)
{
    AI_JSON_SPAN DataArr;
    AI_JSON_SPAN Elem0;
    AI_JSON_SPAN EmbSpan;

    if (Json == NULL || OutVec == NULL || OutDim == NULL || MaxDim == 0)
        return -1;
    *OutDim = 0;

    if (AiJsonObjGet(Json, JsonLen, "data", &DataArr) != 0)
        return -1;
    if (AiJsonArrayGet(DataArr.Start, DataArr.Len, 0, &Elem0) != 0)
        return -1;
    if (AiJsonObjGet(Elem0.Start, Elem0.Len, "embedding", &EmbSpan) != 0)
        return -1;
    return AiJsonParseFloatArray(EmbSpan.Start, EmbSpan.Len, OutVec, MaxDim, OutDim);
}
