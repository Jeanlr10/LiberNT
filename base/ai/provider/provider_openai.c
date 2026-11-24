/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     OpenAI-compatible HTTP/HTTPS inference provider over WinHTTP
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winhttp.h>

#include "ai_protocol.h"
#include "ai_provider.h"

#define AI_MAX_HTTP_RESPONSE (256U * 1024U)

typedef struct _AI_OPENAI_PROVIDER
{
    AI_PROVIDER Base;
    AI_PROVIDER_CONFIG Cfg;
} AI_OPENAI_PROVIDER;

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

static void ReadRegUtf8(HKEY Key, PCWSTR ValueName, char *Out, size_t OutCap)
{
    WCHAR Wide[512];
    DWORD Type = 0;
    DWORD Size = sizeof(Wide) - sizeof(WCHAR);

    if (RegQueryValueExW(Key, ValueName, NULL, &Type, (LPBYTE)Wide, &Size) != ERROR_SUCCESS)
        return;
    if (Type != REG_SZ && Type != REG_EXPAND_SZ)
        return;
    Wide[Size / sizeof(WCHAR)] = UNICODE_NULL;
    if (Wide[0] == UNICODE_NULL)
        return;
    WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Out, (int)OutCap, NULL, NULL);
    Out[OutCap - 1] = '\0';
}

static int WriteRegUtf8(HKEY Key, PCWSTR ValueName, const char *ValueUtf8)
{
    WCHAR Wide[512];
    int Len;

    if (ValueUtf8 == NULL)
        ValueUtf8 = "";
    Len = MultiByteToWideChar(CP_UTF8, 0, ValueUtf8, -1, Wide, ARRAYSIZE(Wide));
    if (Len <= 0)
        return -1;
    if (RegSetValueExW(Key, ValueName, 0, REG_SZ, (const BYTE *)Wide, (DWORD)Len * sizeof(WCHAR)) != ERROR_SUCCESS)
        return -1;
    return 0;
}

void AiProviderDefaultConfig(AI_PROVIDER_CONFIG *Cfg)
{
    if (Cfg == NULL)
        return;
    memset(Cfg, 0, sizeof(*Cfg));
    CopyStrSafe(Cfg->ProviderType, sizeof(Cfg->ProviderType), AI_PROVIDER_REMOTE);
    CopyStrSafe(Cfg->Endpoint, sizeof(Cfg->Endpoint), AI_DEFAULT_ENDPOINT);
    CopyStrSafe(Cfg->Model, sizeof(Cfg->Model), AI_DEFAULT_MODEL);
    CopyStrSafe(Cfg->EmbedModel, sizeof(Cfg->EmbedModel), AI_DEFAULT_EMBED_MODEL);
    CopyStrSafe(Cfg->LocalModelPath, sizeof(Cfg->LocalModelPath), AI_DEFAULT_LOCAL_MODEL);
    CopyStrSafe(Cfg->BackendInfo, sizeof(Cfg->BackendInfo), "winhttp-openai");
    Cfg->TimeoutMs = AI_DEFAULT_TIMEOUT_MS;
    Cfg->MaxTokens = AI_DEFAULT_MAX_TOKENS;
    Cfg->Threads = 4U;
}

void AiProviderLoadConfigFromRegistry(AI_PROVIDER_CONFIG *Cfg)
{
    HKEY Key = NULL;
    DWORD Val = 0;
    DWORD Type = 0;
    DWORD Size = sizeof(Val);

    if (Cfg == NULL)
        return;
    AiProviderDefaultConfig(Cfg);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AI_REG_KEY, 0, KEY_READ, &Key) == ERROR_SUCCESS)
    {
        ReadRegUtf8(Key, L"ProviderType", Cfg->ProviderType, sizeof(Cfg->ProviderType));
        ReadRegUtf8(Key, L"Endpoint", Cfg->Endpoint, sizeof(Cfg->Endpoint));
        ReadRegUtf8(Key, L"Model", Cfg->Model, sizeof(Cfg->Model));
        ReadRegUtf8(Key, L"EmbedModel", Cfg->EmbedModel, sizeof(Cfg->EmbedModel));
        ReadRegUtf8(Key, L"ApiKey", Cfg->ApiKey, sizeof(Cfg->ApiKey));
        ReadRegUtf8(Key, L"LocalModelPath", Cfg->LocalModelPath, sizeof(Cfg->LocalModelPath));
        Size = sizeof(Val);
        if (RegQueryValueExW(Key, L"TimeoutMs", NULL, &Type, (LPBYTE)&Val, &Size) == ERROR_SUCCESS &&
            Type == REG_DWORD && Val >= 1000 && Val <= 120000)
        {
            Cfg->TimeoutMs = Val;
        }
        Size = sizeof(Val);
        if (RegQueryValueExW(Key, L"MaxTokens", NULL, &Type, (LPBYTE)&Val, &Size) == ERROR_SUCCESS &&
            Type == REG_DWORD && Val >= 16 && Val <= 2048)
        {
            Cfg->MaxTokens = Val;
        }
        Size = sizeof(Val);
        if (RegQueryValueExW(Key, L"Threads", NULL, &Type, (LPBYTE)&Val, &Size) == ERROR_SUCCESS &&
            Type == REG_DWORD && Val >= 1 && Val <= 64)
        {
            Cfg->Threads = Val;
        }
        RegCloseKey(Key);
    }
}

int AiProviderSaveConfigToRegistry(const AI_PROVIDER_CONFIG *Cfg)
{
    HKEY Key = NULL;
    DWORD Disp = 0;
    DWORD Timeout;
    DWORD MaxTok;
    DWORD Threads;

    if (Cfg == NULL)
        return -1;

    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            AI_REG_KEY,
            0,
            NULL,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            NULL,
            &Key,
            &Disp) != ERROR_SUCCESS)
    {
        return -1;
    }

    WriteRegUtf8(Key, L"ProviderType", Cfg->ProviderType[0] ? Cfg->ProviderType : AI_PROVIDER_REMOTE);
    WriteRegUtf8(Key, L"Endpoint", Cfg->Endpoint);
    WriteRegUtf8(Key, L"Model", Cfg->Model);
    WriteRegUtf8(Key, L"EmbedModel", Cfg->EmbedModel);
    WriteRegUtf8(Key, L"ApiKey", Cfg->ApiKey);
    WriteRegUtf8(Key, L"LocalModelPath", Cfg->LocalModelPath);
    Timeout = Cfg->TimeoutMs ? Cfg->TimeoutMs : AI_DEFAULT_TIMEOUT_MS;
    MaxTok = Cfg->MaxTokens ? Cfg->MaxTokens : AI_DEFAULT_MAX_TOKENS;
    Threads = Cfg->Threads ? Cfg->Threads : 4U;
    RegSetValueExW(Key, L"TimeoutMs", 0, REG_DWORD, (const BYTE *)&Timeout, sizeof(Timeout));
    RegSetValueExW(Key, L"MaxTokens", 0, REG_DWORD, (const BYTE *)&MaxTok, sizeof(MaxTok));
    RegSetValueExW(Key, L"Threads", 0, REG_DWORD, (const BYTE *)&Threads, sizeof(Threads));
    RegCloseKey(Key);
    return 0;
}

static int HttpPostJson(
    const AI_PROVIDER_CONFIG *Cfg,
    const char *PathSuffix,
    const char *BodyJson,
    char **OutResp,
    size_t *OutLen,
    int *OutStatus,
    char *ErrBuf,
    size_t ErrCap)
{
    char FullUrlUtf8[512];
    WCHAR FullUrlW[512];
    WCHAR HostW[256];
    WCHAR PathW[256];
    URL_COMPONENTSW Comps;
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    DWORD OpenFlags = 0;
    DWORD StatusCode = 0;
    DWORD StatusSize = sizeof(StatusCode);
    char *RespBuf = NULL;
    size_t RespCap = 4096;
    size_t RespLen = 0;
    int Result = -1;
    size_t BaseLen;

    if (OutResp)
        *OutResp = NULL;
    if (OutLen)
        *OutLen = 0;
    if (OutStatus)
        *OutStatus = 0;
    if (ErrBuf && ErrCap > 0)
        ErrBuf[0] = '\0';

    BaseLen = strlen(Cfg->Endpoint);
    while (BaseLen > 0 && Cfg->Endpoint[BaseLen - 1] == '/')
        BaseLen--;
    _snprintf(FullUrlUtf8, sizeof(FullUrlUtf8), "%.*s%s", (int)BaseLen, Cfg->Endpoint, PathSuffix);
    FullUrlUtf8[sizeof(FullUrlUtf8) - 1] = '\0';

    if (MultiByteToWideChar(CP_UTF8, 0, FullUrlUtf8, -1, FullUrlW, ARRAYSIZE(FullUrlW)) <= 0)
    {
        CopyStrSafe(ErrBuf, ErrCap, "invalid endpoint URL");
        return -1;
    }

    memset(&Comps, 0, sizeof(Comps));
    Comps.dwStructSize = sizeof(Comps);
    Comps.lpszHostName = HostW;
    Comps.dwHostNameLength = ARRAYSIZE(HostW);
    Comps.lpszUrlPath = PathW;
    Comps.dwUrlPathLength = ARRAYSIZE(PathW);

    if (!WinHttpCrackUrl(FullUrlW, 0, 0, &Comps))
    {
        _snprintf(ErrBuf, ErrCap, "WinHttpCrackUrl failed (%lu)", (unsigned long)GetLastError());
        return -1;
    }

    hSession = WinHttpOpen(
        L"LiberNT-AI/0.2",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (hSession == NULL)
    {
        _snprintf(ErrBuf, ErrCap, "WinHttpOpen failed (%lu)", (unsigned long)GetLastError());
        goto Cleanup;
    }

    WinHttpSetTimeouts(
        hSession,
        (int)(Cfg->TimeoutMs ? Cfg->TimeoutMs : AI_DEFAULT_TIMEOUT_MS),
        (int)(Cfg->TimeoutMs ? Cfg->TimeoutMs : AI_DEFAULT_TIMEOUT_MS),
        (int)(Cfg->TimeoutMs ? Cfg->TimeoutMs : AI_DEFAULT_TIMEOUT_MS),
        (int)(Cfg->TimeoutMs ? Cfg->TimeoutMs : AI_DEFAULT_TIMEOUT_MS));

    hConnect = WinHttpConnect(hSession, HostW, Comps.nPort, 0);
    if (hConnect == NULL)
    {
        _snprintf(ErrBuf, ErrCap, "WinHttpConnect failed (%lu)", (unsigned long)GetLastError());
        goto Cleanup;
    }

    if (Comps.nScheme == INTERNET_SCHEME_HTTPS)
        OpenFlags |= WINHTTP_FLAG_SECURE;

    hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        PathW[0] ? PathW : L"/",
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        OpenFlags);
    if (hRequest == NULL)
    {
        _snprintf(ErrBuf, ErrCap, "WinHttpOpenRequest failed (%lu)", (unsigned long)GetLastError());
        goto Cleanup;
    }

    WinHttpAddRequestHeaders(
        hRequest,
        L"Content-Type: application/json\r\n",
        (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (Cfg->ApiKey[0] != '\0')
    {
        char AuthUtf8[320];
        WCHAR AuthW[320];
        _snprintf(AuthUtf8, sizeof(AuthUtf8), "Authorization: Bearer %s\r\n", Cfg->ApiKey);
        AuthUtf8[sizeof(AuthUtf8) - 1] = '\0';
        if (MultiByteToWideChar(CP_UTF8, 0, AuthUtf8, -1, AuthW, ARRAYSIZE(AuthW)) > 0)
        {
            WinHttpAddRequestHeaders(
                hRequest,
                AuthW,
                (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
    }

    {
        DWORD BodyLen = (DWORD)strlen(BodyJson);
        if (!WinHttpSendRequest(
                hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                (LPVOID)BodyJson,
                BodyLen,
                BodyLen,
                0))
        {
            _snprintf(ErrBuf, ErrCap, "WinHttpSendRequest failed (%lu)", (unsigned long)GetLastError());
            goto Cleanup;
        }
    }

    if (!WinHttpReceiveResponse(hRequest, NULL))
    {
        _snprintf(ErrBuf, ErrCap, "WinHttpReceiveResponse failed (%lu)", (unsigned long)GetLastError());
        goto Cleanup;
    }

    if (WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &StatusCode,
            &StatusSize,
            WINHTTP_NO_HEADER_INDEX))
    {
        if (OutStatus)
            *OutStatus = (int)StatusCode;
    }

    RespBuf = (char *)malloc(RespCap);
    if (RespBuf == NULL)
    {
        CopyStrSafe(ErrBuf, ErrCap, "out of memory");
        goto Cleanup;
    }

    for (;;)
    {
        DWORD Avail = 0;
        DWORD ReadNow = 0;

        if (!WinHttpQueryDataAvailable(hRequest, &Avail))
            break;
        if (Avail == 0)
            break;

        if (RespLen + Avail + 1 > AI_MAX_HTTP_RESPONSE)
            Avail = (DWORD)(AI_MAX_HTTP_RESPONSE - RespLen - 1);
        if (Avail == 0)
            break;

        if (RespLen + Avail + 1 > RespCap)
        {
            size_t NewCap = RespCap * 2;
            char *Tmp;
            while (NewCap < RespLen + Avail + 1)
                NewCap *= 2;
            Tmp = (char *)realloc(RespBuf, NewCap);
            if (Tmp == NULL)
            {
                CopyStrSafe(ErrBuf, ErrCap, "out of memory");
                goto Cleanup;
            }
            RespBuf = Tmp;
            RespCap = NewCap;
        }

        if (!WinHttpReadData(hRequest, RespBuf + RespLen, Avail, &ReadNow) || ReadNow == 0)
            break;
        RespLen += ReadNow;
    }

    RespBuf[RespLen] = '\0';
    *OutResp = RespBuf;
    *OutLen = RespLen;
    RespBuf = NULL;
    Result = 0;

Cleanup:
    free(RespBuf);
    if (hRequest)
        WinHttpCloseHandle(hRequest);
    if (hConnect)
        WinHttpCloseHandle(hConnect);
    if (hSession)
        WinHttpCloseHandle(hSession);
    return Result;
}

static int OpenAiChatComplete(
    AI_PROVIDER *Self,
    const AI_CHAT_MESSAGE *Messages,
    uint32_t MessageCount,
    const char *ToolsJson,
    AI_CHAT_RESPONSE *OutResponse)
{
    AI_OPENAI_PROVIDER *Prov = (AI_OPENAI_PROVIDER *)Self;
    char *ReqBody = NULL;
    char *RespBody = NULL;
    size_t RespLen = 0;
    int HttpStatus = 0;
    int Rc = -1;

    if (Prov == NULL || OutResponse == NULL)
        return -1;
    memset(OutResponse, 0, sizeof(*OutResponse));

    ReqBody = AiOpenAiBuildChatRequest(Prov->Cfg.Model, Messages, MessageCount, ToolsJson);
    if (ReqBody == NULL)
    {
        CopyStrSafe(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "failed to build request JSON");
        return -1;
    }

    if (HttpPostJson(
            &Prov->Cfg,
            "/chat/completions",
            ReqBody,
            &RespBody,
            &RespLen,
            &HttpStatus,
            OutResponse->ErrorText,
            sizeof(OutResponse->ErrorText)) != 0)
    {
        free(ReqBody);
        return -1;
    }
    free(ReqBody);

    OutResponse->HttpStatus = HttpStatus;
    Rc = AiOpenAiParseChatResponse(RespBody, RespLen, OutResponse);
    free(RespBody);

    if (HttpStatus < 200 || HttpStatus >= 300)
    {
        if (OutResponse->ErrorText[0] == '\0')
            _snprintf(OutResponse->ErrorText, sizeof(OutResponse->ErrorText), "HTTP status %d", HttpStatus);
        return -1;
    }
    return Rc;
}

static int OpenAiEmbed(
    AI_PROVIDER *Self,
    const char *Text,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim)
{
    AI_OPENAI_PROVIDER *Prov = (AI_OPENAI_PROVIDER *)Self;
    char *ReqBody = NULL;
    char *RespBody = NULL;
    size_t RespLen = 0;
    int HttpStatus = 0;
    char ErrBuf[128];
    int Rc;

    if (Prov == NULL || Text == NULL || OutVec == NULL || OutDim == NULL)
        return -1;
    if (Prov->Cfg.EmbedModel[0] == '\0')
        return -1;

    ReqBody = AiOpenAiBuildEmbedRequest(Prov->Cfg.EmbedModel, Text);
    if (ReqBody == NULL)
        return -1;

    if (HttpPostJson(&Prov->Cfg, "/embeddings", ReqBody, &RespBody, &RespLen,
                     &HttpStatus, ErrBuf, sizeof(ErrBuf)) != 0)
    {
        free(ReqBody);
        return -1;
    }
    free(ReqBody);

    if (HttpStatus < 200 || HttpStatus >= 300)
    {
        free(RespBody);
        return -1;
    }

    Rc = AiOpenAiParseEmbedResponse(RespBody, RespLen, OutVec, MaxDim, OutDim);
    free(RespBody);
    return Rc;
}

static void OpenAiGetConfig(AI_PROVIDER *Self, AI_PROVIDER_CONFIG *OutConfig)
{
    AI_OPENAI_PROVIDER *Prov = (AI_OPENAI_PROVIDER *)Self;
    if (Prov == NULL || OutConfig == NULL)
        return;
    *OutConfig = Prov->Cfg;
}

static void OpenAiDestroy(AI_PROVIDER *Self)
{
    free(Self);
}

static const AI_PROVIDER_VTBL g_OpenAiVtbl =
{
    OpenAiChatComplete,
    OpenAiEmbed,
    OpenAiGetConfig,
    OpenAiDestroy
};

AI_PROVIDER *AiProviderCreateOpenAi(const AI_PROVIDER_CONFIG *Cfg)
{
    AI_OPENAI_PROVIDER *Prov;

    Prov = (AI_OPENAI_PROVIDER *)calloc(1, sizeof(*Prov));
    if (Prov == NULL)
        return NULL;

    Prov->Base.Vtbl = &g_OpenAiVtbl;
    if (Cfg != NULL)
        Prov->Cfg = *Cfg;
    else
        AiProviderLoadConfigFromRegistry(&Prov->Cfg);
    CopyStrSafe(Prov->Cfg.ProviderType, sizeof(Prov->Cfg.ProviderType), AI_PROVIDER_REMOTE);
    CopyStrSafe(Prov->Cfg.BackendInfo, sizeof(Prov->Cfg.BackendInfo), "winhttp-openai");
    if (Prov->Cfg.TimeoutMs == 0)
        Prov->Cfg.TimeoutMs = AI_DEFAULT_TIMEOUT_MS;
    return &Prov->Base;
}

AI_PROVIDER *AiProviderCreateFromConfig(const AI_PROVIDER_CONFIG *Cfg)
{
    AI_PROVIDER_CONFIG LocalCfg;

    if (Cfg == NULL)
    {
        AiProviderLoadConfigFromRegistry(&LocalCfg);
        Cfg = &LocalCfg;
    }
    if (_stricmp(Cfg->ProviderType, AI_PROVIDER_LOCAL) == 0)
        return AiProviderCreateLocal(Cfg);
    return AiProviderCreateOpenAi(Cfg);
}
