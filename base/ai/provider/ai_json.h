/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Portable bounded C11 JSON builder and parser for OpenAI-compatible payloads
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _AI_JSON_BUF
{
    char *Data;
    size_t Len;
    size_t Cap;
    int Failed;
} AI_JSON_BUF;

void AiJsonBufInit(AI_JSON_BUF *Buf, size_t InitialCap);
void AiJsonBufFree(AI_JSON_BUF *Buf);
int AiJsonBufAppendRaw(AI_JSON_BUF *Buf, const char *Text, size_t Len);
int AiJsonBufAppendStr(AI_JSON_BUF *Buf, const char *Text);
int AiJsonBufAppendChar(AI_JSON_BUF *Buf, char Ch);
int AiJsonBufAppendJsonString(AI_JSON_BUF *Buf, const char *Text);
int AiJsonBufAppendInt64(AI_JSON_BUF *Buf, int64_t Val);

typedef struct _AI_JSON_SPAN
{
    const char *Start;
    size_t Len;
} AI_JSON_SPAN;

const char *AiJsonSkipWs(const char *P, const char *End);
int AiJsonSkipValue(const char **Cursor, const char *End, AI_JSON_SPAN *OutSpan);

int AiJsonObjGet(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    AI_JSON_SPAN *OutValue);

int AiJsonArrayGet(
    const char *ArrJson,
    size_t ArrLen,
    size_t Index,
    AI_JSON_SPAN *OutElem);

size_t AiJsonArrayCount(const char *ArrJson, size_t ArrLen);

int AiJsonUnescapeString(
    const char *QuotedJson,
    size_t QuotedLen,
    char *Out,
    size_t OutCap);

int AiJsonObjGetString(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    char *Out,
    size_t OutCap);

int AiJsonObjGetInt64(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    int64_t *OutVal);

int AiJsonObjGetBool(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    int *OutVal);

int AiJsonParseFloatArray(
    const char *ArrJson,
    size_t ArrLen,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim);

#ifdef __cplusplus
}
#endif
