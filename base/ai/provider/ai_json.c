/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Portable bounded C11 JSON builder and parser for OpenAI-compatible payloads
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_json.h"

#define AI_JSON_MAX_DEPTH 64

static int JsonBufReserve(AI_JSON_BUF *Buf, size_t Extra)
{
    size_t Needed;
    size_t NewCap;
    char *NewData;

    if (Buf == NULL || Buf->Failed)
        return -1;
    if (Extra > (size_t)-1 - Buf->Len - 1)
    {
        Buf->Failed = 1;
        return -1;
    }
    Needed = Buf->Len + Extra + 1;
    if (Needed <= Buf->Cap)
        return 0;

    NewCap = Buf->Cap ? Buf->Cap : 256;
    while (NewCap < Needed)
    {
        if (NewCap > (size_t)-1 / 2)
        {
            NewCap = Needed;
            break;
        }
        NewCap *= 2;
    }

    NewData = (char *)realloc(Buf->Data, NewCap);
    if (NewData == NULL)
    {
        Buf->Failed = 1;
        return -1;
    }
    Buf->Data = NewData;
    Buf->Cap = NewCap;
    return 0;
}

void AiJsonBufInit(AI_JSON_BUF *Buf, size_t InitialCap)
{
    if (Buf == NULL)
        return;
    Buf->Data = NULL;
    Buf->Len = 0;
    Buf->Cap = 0;
    Buf->Failed = 0;
    if (InitialCap > 0 && JsonBufReserve(Buf, InitialCap) == 0)
        Buf->Data[0] = '\0';
}

void AiJsonBufFree(AI_JSON_BUF *Buf)
{
    if (Buf == NULL)
        return;
    free(Buf->Data);
    Buf->Data = NULL;
    Buf->Len = 0;
    Buf->Cap = 0;
    Buf->Failed = 0;
}

int AiJsonBufAppendRaw(AI_JSON_BUF *Buf, const char *Text, size_t Len)
{
    if (Buf == NULL || (Text == NULL && Len > 0))
        return -1;
    if (JsonBufReserve(Buf, Len) != 0)
        return -1;
    if (Len > 0)
        memcpy(Buf->Data + Buf->Len, Text, Len);
    Buf->Len += Len;
    Buf->Data[Buf->Len] = '\0';
    return 0;
}

int AiJsonBufAppendStr(AI_JSON_BUF *Buf, const char *Text)
{
    if (Text == NULL)
        return 0;
    return AiJsonBufAppendRaw(Buf, Text, strlen(Text));
}

int AiJsonBufAppendChar(AI_JSON_BUF *Buf, char Ch)
{
    return AiJsonBufAppendRaw(Buf, &Ch, 1);
}

int AiJsonBufAppendJsonString(AI_JSON_BUF *Buf, const char *Text)
{
    const unsigned char *P;

    if (AiJsonBufAppendChar(Buf, '"') != 0)
        return -1;
    if (Text != NULL)
    {
        for (P = (const unsigned char *)Text; *P != '\0'; P++)
        {
            unsigned char Ch = *P;
            char Esc[8];

            if (Ch == '"')
            {
                if (AiJsonBufAppendRaw(Buf, "\\\"", 2) != 0)
                    return -1;
            }
            else if (Ch == '\\')
            {
                if (AiJsonBufAppendRaw(Buf, "\\\\", 2) != 0)
                    return -1;
            }
            else if (Ch == '\b')
            {
                if (AiJsonBufAppendRaw(Buf, "\\b", 2) != 0)
                    return -1;
            }
            else if (Ch == '\f')
            {
                if (AiJsonBufAppendRaw(Buf, "\\f", 2) != 0)
                    return -1;
            }
            else if (Ch == '\n')
            {
                if (AiJsonBufAppendRaw(Buf, "\\n", 2) != 0)
                    return -1;
            }
            else if (Ch == '\r')
            {
                if (AiJsonBufAppendRaw(Buf, "\\r", 2) != 0)
                    return -1;
            }
            else if (Ch == '\t')
            {
                if (AiJsonBufAppendRaw(Buf, "\\t", 2) != 0)
                    return -1;
            }
            else if (Ch < 0x20)
            {
                int n = snprintf(Esc, sizeof(Esc), "\\u%04x", (unsigned int)Ch);
                if (n < 0 || AiJsonBufAppendRaw(Buf, Esc, (size_t)n) != 0)
                    return -1;
            }
            else
            {
                if (AiJsonBufAppendChar(Buf, (char)Ch) != 0)
                    return -1;
            }
        }
    }
    return AiJsonBufAppendChar(Buf, '"');
}

int AiJsonBufAppendInt64(AI_JSON_BUF *Buf, int64_t Val)
{
    char Num[32];
    int n = snprintf(Num, sizeof(Num), "%lld", (long long)Val);
    if (n < 0)
        return -1;
    return AiJsonBufAppendRaw(Buf, Num, (size_t)n);
}

const char *AiJsonSkipWs(const char *P, const char *End)
{
    while (P < End && (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n'))
        P++;
    return P;
}

static int SkipString(const char **Cursor, const char *End)
{
    const char *P = *Cursor;

    if (P >= End || *P != '"')
        return -1;
    P++;
    while (P < End)
    {
        if (*P == '\\')
        {
            P++;
            if (P >= End)
                return -1;
            P++;
            continue;
        }
        if (*P == '"')
        {
            *Cursor = P + 1;
            return 0;
        }
        P++;
    }
    return -1;
}

static int SkipValueDepth(const char **Cursor, const char *End, AI_JSON_SPAN *OutSpan, int Depth)
{
    const char *P;
    const char *Start;

    if (Depth > AI_JSON_MAX_DEPTH)
        return -1;

    P = AiJsonSkipWs(*Cursor, End);
    if (P >= End)
        return -1;
    Start = P;

    if (*P == '"')
    {
        if (SkipString(&P, End) != 0)
            return -1;
    }
    else if (*P == '{')
    {
        P++;
        P = AiJsonSkipWs(P, End);
        if (P < End && *P == '}')
        {
            P++;
        }
        else
        {
            for (;;)
            {
                P = AiJsonSkipWs(P, End);
                if (SkipString(&P, End) != 0)
                    return -1;
                P = AiJsonSkipWs(P, End);
                if (P >= End || *P != ':')
                    return -1;
                P++;
                if (SkipValueDepth(&P, End, NULL, Depth + 1) != 0)
                    return -1;
                P = AiJsonSkipWs(P, End);
                if (P >= End)
                    return -1;
                if (*P == ',')
                {
                    P++;
                    continue;
                }
                if (*P == '}')
                {
                    P++;
                    break;
                }
                return -1;
            }
        }
    }
    else if (*P == '[')
    {
        P++;
        P = AiJsonSkipWs(P, End);
        if (P < End && *P == ']')
        {
            P++;
        }
        else
        {
            for (;;)
            {
                if (SkipValueDepth(&P, End, NULL, Depth + 1) != 0)
                    return -1;
                P = AiJsonSkipWs(P, End);
                if (P >= End)
                    return -1;
                if (*P == ',')
                {
                    P++;
                    continue;
                }
                if (*P == ']')
                {
                    P++;
                    break;
                }
                return -1;
            }
        }
    }
    else if (End - P >= 4 && memcmp(P, "true", 4) == 0)
    {
        P += 4;
    }
    else if (End - P >= 5 && memcmp(P, "false", 5) == 0)
    {
        P += 5;
    }
    else if (End - P >= 4 && memcmp(P, "null", 4) == 0)
    {
        P += 4;
    }
    else if (*P == '-' || (*P >= '0' && *P <= '9'))
    {
        if (*P == '-')
            P++;
        if (P >= End || !(*P >= '0' && *P <= '9'))
            return -1;
        while (P < End && (*P >= '0' && *P <= '9'))
            P++;
        if (P < End && *P == '.')
        {
            P++;
            if (P >= End || !(*P >= '0' && *P <= '9'))
                return -1;
            while (P < End && (*P >= '0' && *P <= '9'))
                P++;
        }
        if (P < End && (*P == 'e' || *P == 'E'))
        {
            P++;
            if (P < End && (*P == '+' || *P == '-'))
                P++;
            if (P >= End || !(*P >= '0' && *P <= '9'))
                return -1;
            while (P < End && (*P >= '0' && *P <= '9'))
                P++;
        }
    }
    else
    {
        return -1;
    }

    if (OutSpan != NULL)
    {
        OutSpan->Start = Start;
        OutSpan->Len = (size_t)(P - Start);
    }
    *Cursor = P;
    return 0;
}

int AiJsonSkipValue(const char **Cursor, const char *End, AI_JSON_SPAN *OutSpan)
{
    if (Cursor == NULL || *Cursor == NULL || End == NULL)
        return -1;
    return SkipValueDepth(Cursor, End, OutSpan, 0);
}

static int ParseHex4(const char *P, unsigned int *OutVal)
{
    unsigned int Val = 0;
    int i;

    for (i = 0; i < 4; i++)
    {
        unsigned char Ch = (unsigned char)P[i];
        Val <<= 4;
        if (Ch >= '0' && Ch <= '9')
            Val |= (unsigned int)(Ch - '0');
        else if (Ch >= 'a' && Ch <= 'f')
            Val |= (unsigned int)(Ch - 'a' + 10);
        else if (Ch >= 'A' && Ch <= 'F')
            Val |= (unsigned int)(Ch - 'A' + 10);
        else
            return -1;
    }
    *OutVal = Val;
    return 0;
}

static void AppendUtf8Codepoint(char *Out, size_t OutCap, size_t *Pos, unsigned int Cp)
{
    if (Cp < 0x80)
    {
        if (*Pos + 1 < OutCap)
            Out[(*Pos)++] = (char)Cp;
    }
    else if (Cp < 0x800)
    {
        if (*Pos + 2 < OutCap)
        {
            Out[(*Pos)++] = (char)(0xC0 | (Cp >> 6));
            Out[(*Pos)++] = (char)(0x80 | (Cp & 0x3F));
        }
    }
    else if (Cp < 0x10000)
    {
        if (*Pos + 3 < OutCap)
        {
            Out[(*Pos)++] = (char)(0xE0 | (Cp >> 12));
            Out[(*Pos)++] = (char)(0x80 | ((Cp >> 6) & 0x3F));
            Out[(*Pos)++] = (char)(0x80 | (Cp & 0x3F));
        }
    }
    else if (Cp <= 0x10FFFF)
    {
        if (*Pos + 4 < OutCap)
        {
            Out[(*Pos)++] = (char)(0xF0 | (Cp >> 18));
            Out[(*Pos)++] = (char)(0x80 | ((Cp >> 12) & 0x3F));
            Out[(*Pos)++] = (char)(0x80 | ((Cp >> 6) & 0x3F));
            Out[(*Pos)++] = (char)(0x80 | (Cp & 0x3F));
        }
    }
}

int AiJsonUnescapeString(
    const char *QuotedJson,
    size_t QuotedLen,
    char *Out,
    size_t OutCap)
{
    const char *P;
    const char *End;
    size_t Pos = 0;

    if (QuotedJson == NULL || Out == NULL || OutCap == 0)
        return -1;
    Out[0] = '\0';

    if (QuotedLen < 2 || QuotedJson[0] != '"' || QuotedJson[QuotedLen - 1] != '"')
        return -1;

    P = QuotedJson + 1;
    End = QuotedJson + QuotedLen - 1;
    while (P < End)
    {
        if (*P == '\\')
        {
            P++;
            if (P >= End)
                return -1;
            switch (*P)
            {
                case '"':
                case '\\':
                case '/':
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = *P;
                    P++;
                    break;
                case 'b':
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = '\b';
                    P++;
                    break;
                case 'f':
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = '\f';
                    P++;
                    break;
                case 'n':
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = '\n';
                    P++;
                    break;
                case 'r':
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = '\r';
                    P++;
                    break;
                case 't':
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = '\t';
                    P++;
                    break;
                case 'u':
                {
                    unsigned int Cp = 0;
                    P++;
                    if (End - P < 4 || ParseHex4(P, &Cp) != 0)
                        return -1;
                    P += 4;
                    if (Cp >= 0xD800 && Cp <= 0xDBFF && End - P >= 6 && P[0] == '\\' && P[1] == 'u')
                    {
                        unsigned int Low = 0;
                        if (ParseHex4(P + 2, &Low) == 0 && Low >= 0xDC00 && Low <= 0xDFFF)
                        {
                            Cp = 0x10000 + (((Cp - 0xD800) << 10) | (Low - 0xDC00));
                            P += 6;
                        }
                    }
                    AppendUtf8Codepoint(Out, OutCap, &Pos, Cp);
                    break;
                }
                default:
                    if (Pos + 1 < OutCap)
                        Out[Pos++] = *P;
                    P++;
                    break;
            }
        }
        else
        {
            if (Pos + 1 < OutCap)
                Out[Pos++] = *P;
            P++;
        }
    }
    Out[Pos] = '\0';
    return 0;
}

int AiJsonObjGet(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    AI_JSON_SPAN *OutValue)
{
    const char *P;
    const char *End;

    if (ObjJson == NULL || Key == NULL || OutValue == NULL)
        return -1;

    End = ObjJson + ObjLen;
    P = AiJsonSkipWs(ObjJson, End);
    if (P >= End || *P != '{')
        return -1;
    P++;
    P = AiJsonSkipWs(P, End);
    if (P < End && *P == '}')
        return -1;

    while (P < End)
    {
        const char *KeyStart;
        char KeyBuf[128];
        AI_JSON_SPAN ValSpan;

        P = AiJsonSkipWs(P, End);
        KeyStart = P;
        if (SkipString(&P, End) != 0)
            return -1;
        if (AiJsonUnescapeString(KeyStart, (size_t)(P - KeyStart), KeyBuf, sizeof(KeyBuf)) != 0)
            return -1;

        P = AiJsonSkipWs(P, End);
        if (P >= End || *P != ':')
            return -1;
        P++;

        if (AiJsonSkipValue(&P, End, &ValSpan) != 0)
            return -1;

        if (strcmp(KeyBuf, Key) == 0)
        {
            *OutValue = ValSpan;
            return 0;
        }

        P = AiJsonSkipWs(P, End);
        if (P >= End)
            return -1;
        if (*P == ',')
        {
            P++;
            continue;
        }
        if (*P == '}')
            return -1;
        return -1;
    }
    return -1;
}

int AiJsonArrayGet(
    const char *ArrJson,
    size_t ArrLen,
    size_t Index,
    AI_JSON_SPAN *OutElem)
{
    const char *P;
    const char *End;
    size_t Cur = 0;

    if (ArrJson == NULL || OutElem == NULL)
        return -1;

    End = ArrJson + ArrLen;
    P = AiJsonSkipWs(ArrJson, End);
    if (P >= End || *P != '[')
        return -1;
    P++;
    P = AiJsonSkipWs(P, End);
    if (P < End && *P == ']')
        return -1;

    while (P < End)
    {
        AI_JSON_SPAN Elem;
        if (AiJsonSkipValue(&P, End, &Elem) != 0)
            return -1;
        if (Cur == Index)
        {
            *OutElem = Elem;
            return 0;
        }
        Cur++;
        P = AiJsonSkipWs(P, End);
        if (P >= End)
            return -1;
        if (*P == ',')
        {
            P++;
            continue;
        }
        if (*P == ']')
            return -1;
        return -1;
    }
    return -1;
}

size_t AiJsonArrayCount(const char *ArrJson, size_t ArrLen)
{
    const char *P;
    const char *End;
    size_t Count = 0;

    if (ArrJson == NULL)
        return 0;

    End = ArrJson + ArrLen;
    P = AiJsonSkipWs(ArrJson, End);
    if (P >= End || *P != '[')
        return 0;
    P++;
    P = AiJsonSkipWs(P, End);
    if (P < End && *P == ']')
        return 0;

    while (P < End)
    {
        if (AiJsonSkipValue(&P, End, NULL) != 0)
            return 0;
        Count++;
        P = AiJsonSkipWs(P, End);
        if (P >= End)
            return 0;
        if (*P == ',')
        {
            P++;
            continue;
        }
        if (*P == ']')
            return Count;
        return 0;
    }
    return 0;
}

int AiJsonObjGetString(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    char *Out,
    size_t OutCap)
{
    AI_JSON_SPAN Span;

    if (Out == NULL || OutCap == 0)
        return -1;
    Out[0] = '\0';

    if (AiJsonObjGet(ObjJson, ObjLen, Key, &Span) != 0)
        return -1;
    if (Span.Len == 4 && memcmp(Span.Start, "null", 4) == 0)
        return -1;
    return AiJsonUnescapeString(Span.Start, Span.Len, Out, OutCap);
}

int AiJsonObjGetInt64(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    int64_t *OutVal)
{
    AI_JSON_SPAN Span;
    char NumBuf[64];
    char *EndPtr = NULL;
    long long Val;

    if (OutVal == NULL)
        return -1;
    if (AiJsonObjGet(ObjJson, ObjLen, Key, &Span) != 0)
        return -1;
    if (Span.Len == 0 || Span.Len >= sizeof(NumBuf))
        return -1;
    memcpy(NumBuf, Span.Start, Span.Len);
    NumBuf[Span.Len] = '\0';
    Val = strtoll(NumBuf, &EndPtr, 10);
    if (EndPtr == NumBuf)
        return -1;
    *OutVal = (int64_t)Val;
    return 0;
}

int AiJsonObjGetBool(
    const char *ObjJson,
    size_t ObjLen,
    const char *Key,
    int *OutVal)
{
    AI_JSON_SPAN Span;

    if (OutVal == NULL)
        return -1;
    if (AiJsonObjGet(ObjJson, ObjLen, Key, &Span) != 0)
        return -1;
    if (Span.Len == 4 && memcmp(Span.Start, "true", 4) == 0)
    {
        *OutVal = 1;
        return 0;
    }
    if (Span.Len == 5 && memcmp(Span.Start, "false", 5) == 0)
    {
        *OutVal = 0;
        return 0;
    }
    return -1;
}

int AiJsonParseFloatArray(
    const char *ArrJson,
    size_t ArrLen,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim)
{
    const char *P;
    const char *End;
    uint32_t Count = 0;

    if (ArrJson == NULL || OutVec == NULL || OutDim == NULL || MaxDim == 0)
        return -1;
    *OutDim = 0;

    End = ArrJson + ArrLen;
    P = AiJsonSkipWs(ArrJson, End);
    if (P >= End || *P != '[')
        return -1;
    P++;
    P = AiJsonSkipWs(P, End);
    if (P < End && *P == ']')
        return 0;

    while (P < End)
    {
        AI_JSON_SPAN Elem;
        char NumBuf[64];
        char *EndPtr = NULL;

        if (AiJsonSkipValue(&P, End, &Elem) != 0)
            return -1;
        if (Count < MaxDim)
        {
            if (Elem.Len == 0 || Elem.Len >= sizeof(NumBuf))
                return -1;
            memcpy(NumBuf, Elem.Start, Elem.Len);
            NumBuf[Elem.Len] = '\0';
            OutVec[Count] = (float)strtod(NumBuf, &EndPtr);
            if (EndPtr == NumBuf)
                return -1;
            Count++;
        }

        P = AiJsonSkipWs(P, End);
        if (P >= End)
            return -1;
        if (*P == ',')
        {
            P++;
            continue;
        }
        if (*P == ']')
        {
            *OutDim = Count;
            return 0;
        }
        return -1;
    }
    return -1;
}
