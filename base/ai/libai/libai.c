/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ALPC client SDK and NTFS Alternate Data Stream folder description helpers
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#define LIBAI_EXPORTS
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ndk/lpcfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>

#include "libai.h"

typedef struct _AI_ALPC_PACKET
{
    PORT_MESSAGE Header;
    AI_MSG_PAYLOAD Payload;
} AI_ALPC_PACKET;

static VOID CopySafeStr(PSTR Dst, DWORD Cap, PCSTR Src)
{
    SIZE_T Len;

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

static BOOL NormalizeFolderPath(PCWSTR InputPath, PWSTR OutPath, DWORD OutChars)
{
    DWORD Len;

    if (InputPath == NULL || OutPath == NULL || OutChars < 4)
        return FALSE;

    Len = GetFullPathNameW(InputPath, OutChars, OutPath, NULL);
    if (Len == 0 || Len >= OutChars)
        return FALSE;

    while (Len > 3 && (OutPath[Len - 1] == L'\\' || OutPath[Len - 1] == L'/'))
    {
        OutPath[Len - 1] = UNICODE_NULL;
        Len--;
    }
    return TRUE;
}

static BOOL WideToUtf8(PCWSTR Wide, PSTR Utf8, DWORD Utf8Cap)
{
    int Written;

    if (Wide == NULL || Utf8 == NULL || Utf8Cap == 0)
        return FALSE;
    Written = WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Utf8, (int)Utf8Cap, NULL, NULL);
    if (Written <= 0)
    {
        Utf8[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static BOOL BuildAdsPath(PCWSTR FolderPath, PWSTR OutPath, DWORD OutChars)
{
    WCHAR FullPath[MAX_PATH];
    SIZE_T PathLen;
    SIZE_T SuffixLen;

    if (!NormalizeFolderPath(FolderPath, FullPath, ARRAYSIZE(FullPath)))
        return FALSE;

    PathLen = wcslen(FullPath);
    SuffixLen = wcslen(AI_DESC_STREAM_SUFFIX);
    if (PathLen + SuffixLen + 1 >= OutChars)
        return FALSE;

    memcpy(OutPath, FullPath, PathLen * sizeof(WCHAR));
    memcpy(OutPath + PathLen, AI_DESC_STREAM_SUFFIX, (SuffixLen + 1) * sizeof(WCHAR));
    return TRUE;
}

static SIZE_T AppendJsonEscaped(PSTR Buf, SIZE_T Cap, SIZE_T Pos, PCSTR Value)
{
    PCSTR P;

    if (Value == NULL)
        return Pos;
    for (P = Value; *P != '\0' && Pos + 2 < Cap; P++)
    {
        unsigned char Ch = (unsigned char)*P;

        if (Ch == '"' || Ch == '\\')
        {
            Buf[Pos++] = '\\';
            Buf[Pos++] = (CHAR)Ch;
        }
        else if (Ch == '\n')
        {
            Buf[Pos++] = '\\';
            Buf[Pos++] = 'n';
        }
        else if (Ch == '\r')
        {
            Buf[Pos++] = '\\';
            Buf[Pos++] = 'r';
        }
        else if (Ch == '\t')
        {
            Buf[Pos++] = '\\';
            Buf[Pos++] = 't';
        }
        else if (Ch >= 0x20)
        {
            Buf[Pos++] = (CHAR)Ch;
        }
    }
    Buf[Pos] = '\0';
    return Pos;
}

static BOOL ExtractJsonString(PCSTR Json, PCSTR Key, PSTR Out, DWORD OutCap)
{
    CHAR Pattern[64];
    PCSTR Found;
    PCSTR P;
    DWORD Pos = 0;

    if (Json == NULL || Key == NULL || Out == NULL || OutCap == 0)
        return FALSE;
    Out[0] = '\0';

    _snprintf(Pattern, sizeof(Pattern), "\"%s\":\"", Key);
    Pattern[sizeof(Pattern) - 1] = '\0';
    Found = strstr(Json, Pattern);
    if (Found == NULL)
        return FALSE;

    P = Found + strlen(Pattern);
    while (*P != '\0' && *P != '"' && Pos + 1 < OutCap)
    {
        if (*P == '\\' && P[1] != '\0')
        {
            P++;
            if (*P == 'n')
                Out[Pos++] = '\n';
            else if (*P == 'r')
                Out[Pos++] = '\r';
            else if (*P == 't')
                Out[Pos++] = '\t';
            else
                Out[Pos++] = *P;
        }
        else
        {
            Out[Pos++] = *P;
        }
        P++;
    }
    Out[Pos] = '\0';
    return TRUE;
}

static DWORD SendAlpcRequest(HANDLE Port, const AI_MSG_PAYLOAD *Req, AI_MSG_PAYLOAD *Resp)
{
    AI_ALPC_PACKET SendPkt;
    AI_ALPC_PACKET RecvPkt;
    SIZE_T RecvLen;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (Port == NULL || Req == NULL || Resp == NULL)
        return AiStatusInvalidArg;

    RtlZeroMemory(&SendPkt, sizeof(SendPkt));
    RtlZeroMemory(&RecvPkt, sizeof(RecvPkt));
    SendPkt.Header.u1.s1.DataLength = sizeof(SendPkt.Payload);
    SendPkt.Header.u1.s1.TotalLength = sizeof(SendPkt);
    SendPkt.Payload = *Req;
    SendPkt.Payload.Version = AI_PROTOCOL_VERSION;

    RecvLen = sizeof(RecvPkt);
    Timeout.QuadPart = -(LONGLONG)15000 * 10000;
    Status = NtAlpcSendWaitReceivePort(
        Port,
        ALPC_MSGFLG_SYNC_REQUEST,
        &SendPkt.Header,
        NULL,
        &RecvPkt.Header,
        &RecvLen,
        NULL,
        &Timeout);
    if (!NT_SUCCESS(Status))
        return AiStatusIoError;

    *Resp = RecvPkt.Payload;
    return Resp->Status;
}

LIBAI_API
DWORD
WINAPI
AiClientConnect(
    _Out_ PHANDLE OutPort)
{
    UNICODE_STRING PortName;
    ALPC_PORT_ATTRIBUTES Attributes;
    AI_ALPC_PACKET ConnPkt;
    SIZE_T ConnLen;
    LARGE_INTEGER Timeout;
    HANDLE Port = NULL;
    NTSTATUS Status;

    if (OutPort == NULL)
        return AiStatusInvalidArg;
    *OutPort = NULL;

    RtlInitUnicodeString(&PortName, AI_PORT_NAME);
    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.SecurityQos.Length = sizeof(Attributes.SecurityQos);
    Attributes.SecurityQos.ImpersonationLevel = SecurityImpersonation;
    Attributes.SecurityQos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Attributes.SecurityQos.EffectiveOnly = TRUE;
    Attributes.MaxMessageLength = sizeof(AI_ALPC_PACKET);

    RtlZeroMemory(&ConnPkt, sizeof(ConnPkt));
    ConnPkt.Header.u1.s1.DataLength = sizeof(ConnPkt.Payload);
    ConnPkt.Header.u1.s1.TotalLength = sizeof(ConnPkt);
    ConnPkt.Payload.Version = AI_PROTOCOL_VERSION;
    ConnPkt.Payload.Opcode = AiOpPing;

    ConnLen = sizeof(ConnPkt);
    Timeout.QuadPart = -(LONGLONG)5000 * 10000;
    Status = NtAlpcConnectPort(
        &Port,
        &PortName,
        NULL,
        &Attributes,
        ALPC_SYNC_CONNECTION,
        NULL,
        &ConnPkt.Header,
        &ConnLen,
        NULL,
        NULL,
        &Timeout);
    if (!NT_SUCCESS(Status))
        return AiStatusIoError;

    *OutPort = Port;
    return AiStatusOk;
}

LIBAI_API
VOID
WINAPI
AiClientClose(
    _In_ HANDLE Port)
{
    if (Port != NULL)
    {
        NtAlpcDisconnectPort(Port, 0);
        NtClose(Port);
    }
}

LIBAI_API
DWORD
WINAPI
AiClientGetStatus(
    _In_ HANDLE Port,
    _Out_ AI_STATUS_INFO *OutInfo)
{
    AI_MSG_PAYLOAD Req;
    AI_MSG_PAYLOAD Resp;
    DWORD Result;

    if (Port == NULL || OutInfo == NULL)
        return AiStatusInvalidArg;

    RtlZeroMemory(&Req, sizeof(Req));
    Req.Opcode = AiOpGetStatus;
    Result = SendAlpcRequest(Port, &Req, &Resp);
    if (Result == AiStatusOk)
        *OutInfo = Resp.Data.StatusInfo;
    return Result;
}

LIBAI_API
DWORD
WINAPI
AiClientSearch(
    _In_ HANDLE Port,
    _In_ PCSTR QueryUtf8,
    _In_ DWORD MaxResults,
    _Out_ AI_SEARCH_HIT *OutHits,
    _Out_ PDWORD OutCount)
{
    AI_MSG_PAYLOAD Req;
    AI_MSG_PAYLOAD Resp;
    DWORD Limit;
    DWORD Result;

    if (Port == NULL || QueryUtf8 == NULL || OutHits == NULL || OutCount == NULL || MaxResults == 0)
        return AiStatusInvalidArg;

    *OutCount = 0;
    Limit = (MaxResults > AI_MAX_SEARCH_RESULTS) ? AI_MAX_SEARCH_RESULTS : MaxResults;

    RtlZeroMemory(&Req, sizeof(Req));
    Req.Opcode = AiOpSearch;
    Req.Data.SearchReq.MaxResults = Limit;
    CopySafeStr(Req.Data.SearchReq.Query, sizeof(Req.Data.SearchReq.Query), QueryUtf8);

    Result = SendAlpcRequest(Port, &Req, &Resp);
    if (Result != AiStatusOk)
        return Result;

    if (Resp.Count > Limit)
        Resp.Count = Limit;
    if (Resp.Count > 0)
        memcpy(OutHits, Resp.Data.SearchResp.Hits, Resp.Count * sizeof(AI_SEARCH_HIT));
    *OutCount = Resp.Count;
    return AiStatusOk;
}

LIBAI_API
DWORD
WINAPI
AiClientIndexNow(
    _In_ HANDLE Port,
    _Out_opt_ PDWORD OutIndexedCount)
{
    AI_MSG_PAYLOAD Req;
    AI_MSG_PAYLOAD Resp;
    DWORD Result;

    if (Port == NULL)
        return AiStatusInvalidArg;

    RtlZeroMemory(&Req, sizeof(Req));
    Req.Opcode = AiOpIndexNow;
    Result = SendAlpcRequest(Port, &Req, &Resp);
    if (Result == AiStatusOk && OutIndexedCount != NULL)
        *OutIndexedCount = Resp.Data.IndexResp.IndexedCount;
    return Result;
}

LIBAI_API
DWORD
WINAPI
AiAdsWriteFolderDescription(
    _In_ PCWSTR FolderPath,
    _In_ PCSTR DescUtf8,
    _In_opt_ PCSTR SourceUtf8,
    _In_opt_ PCSTR TagsUtf8)
{
    WCHAR AdsPath[MAX_PATH + 64];
    CHAR JsonBuf[1536];
    SIZE_T Pos = 0;
    DWORD Attrs;
    HANDLE File;
    DWORD Written = 0;
    BOOL Ok;

    if (FolderPath == NULL || DescUtf8 == NULL)
        return AiStatusInvalidArg;

    Attrs = GetFileAttributesW(FolderPath);
    if (Attrs == INVALID_FILE_ATTRIBUTES || !(Attrs & FILE_ATTRIBUTE_DIRECTORY))
        return AiStatusNotFound;

    if (!BuildAdsPath(FolderPath, AdsPath, ARRAYSIZE(AdsPath)))
        return AiStatusInvalidArg;

    Pos += (SIZE_T)_snprintf(JsonBuf + Pos, sizeof(JsonBuf) - Pos, "{\"v\":1,\"source\":\"");
    Pos = AppendJsonEscaped(JsonBuf, sizeof(JsonBuf), Pos, (SourceUtf8 && SourceUtf8[0]) ? SourceUtf8 : "user");
    Pos += (SIZE_T)_snprintf(JsonBuf + Pos, sizeof(JsonBuf) - Pos, "\",\"tags\":\"");
    Pos = AppendJsonEscaped(JsonBuf, sizeof(JsonBuf), Pos, TagsUtf8 ? TagsUtf8 : "");
    Pos += (SIZE_T)_snprintf(JsonBuf + Pos, sizeof(JsonBuf) - Pos, "\",\"text\":\"");
    Pos = AppendJsonEscaped(JsonBuf, sizeof(JsonBuf), Pos, DescUtf8);
    if (Pos + 3 >= sizeof(JsonBuf))
        return AiStatusInvalidArg;
    JsonBuf[Pos++] = '"';
    JsonBuf[Pos++] = '}';
    JsonBuf[Pos] = '\0';

    File = CreateFileW(
        AdsPath,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (File == INVALID_HANDLE_VALUE)
        return AiStatusIoError;

    Ok = WriteFile(File, JsonBuf, (DWORD)Pos, &Written, NULL);
    CloseHandle(File);
    return (Ok && Written == (DWORD)Pos) ? AiStatusOk : AiStatusIoError;
}

LIBAI_API
DWORD
WINAPI
AiAdsReadFolderDescription(
    _In_ PCWSTR FolderPath,
    _Out_ PSTR DescUtf8,
    _In_ DWORD DescCap,
    _Out_opt_ PSTR SourceUtf8,
    _In_ DWORD SourceCap,
    _Out_opt_ PSTR TagsUtf8,
    _In_ DWORD TagsCap)
{
    WCHAR AdsPath[MAX_PATH + 64];
    CHAR JsonBuf[1536];
    HANDLE File;
    DWORD BytesRead = 0;
    BOOL Ok;

    if (FolderPath == NULL || DescUtf8 == NULL || DescCap == 0)
        return AiStatusInvalidArg;

    DescUtf8[0] = '\0';
    CopySafeStr(SourceUtf8, SourceCap, "");
    CopySafeStr(TagsUtf8, TagsCap, "");

    if (!BuildAdsPath(FolderPath, AdsPath, ARRAYSIZE(AdsPath)))
        return AiStatusInvalidArg;

    File = CreateFileW(
        AdsPath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (File == INVALID_HANDLE_VALUE)
        return AiStatusNotFound;

    Ok = ReadFile(File, JsonBuf, sizeof(JsonBuf) - 1, &BytesRead, NULL);
    CloseHandle(File);
    if (!Ok || BytesRead == 0)
        return AiStatusNotFound;
    JsonBuf[BytesRead] = '\0';

    if (!ExtractJsonString(JsonBuf, "text", DescUtf8, DescCap))
    {
        CopySafeStr(DescUtf8, DescCap, JsonBuf);
        CopySafeStr(SourceUtf8, SourceCap, "user");
        return AiStatusOk;
    }
    if (SourceUtf8 != NULL && SourceCap > 0)
        ExtractJsonString(JsonBuf, "source", SourceUtf8, SourceCap);
    if (TagsUtf8 != NULL && TagsCap > 0)
        ExtractJsonString(JsonBuf, "tags", TagsUtf8, TagsCap);
    return AiStatusOk;
}

LIBAI_API
DWORD
WINAPI
AiAdsClearFolderDescription(
    _In_ PCWSTR FolderPath)
{
    WCHAR AdsPath[MAX_PATH + 64];
    HANDLE File;
    DWORD Err;

    if (FolderPath == NULL)
        return AiStatusInvalidArg;
    if (!BuildAdsPath(FolderPath, AdsPath, ARRAYSIZE(AdsPath)))
        return AiStatusInvalidArg;

    File = CreateFileW(
        AdsPath,
        DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
        return AiStatusOk;
    }

    Err = GetLastError();
    if (Err == ERROR_FILE_NOT_FOUND || Err == ERROR_PATH_NOT_FOUND)
        return AiStatusOk;

    File = CreateFileW(
        AdsPath,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        TRUNCATE_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(File);
        return AiStatusOk;
    }

    Err = GetLastError();
    if (Err == ERROR_FILE_NOT_FOUND || Err == ERROR_PATH_NOT_FOUND)
        return AiStatusOk;

    return AiStatusIoError;
}

LIBAI_API
DWORD
WINAPI
AiClientSetFolderDescription(
    _In_opt_ HANDLE Port,
    _In_ PCWSTR FolderPath,
    _In_ PCSTR DescUtf8,
    _In_opt_ PCSTR SourceUtf8,
    _In_opt_ PCSTR TagsUtf8)
{
    WCHAR FullPath[MAX_PATH];
    AI_MSG_PAYLOAD Req;
    AI_MSG_PAYLOAD Resp;
    DWORD AdsResult;

    if (FolderPath == NULL || DescUtf8 == NULL)
        return AiStatusInvalidArg;

    AdsResult = AiAdsWriteFolderDescription(FolderPath, DescUtf8, SourceUtf8, TagsUtf8);
    if (AdsResult != AiStatusOk)
        return AdsResult;

    if (Port == NULL)
        return AiStatusOk;

    if (!NormalizeFolderPath(FolderPath, FullPath, ARRAYSIZE(FullPath)))
        return AiStatusInvalidArg;

    RtlZeroMemory(&Req, sizeof(Req));
    Req.Opcode = AiOpSetFolderDesc;
    WideToUtf8(FullPath, Req.Data.FolderDesc.Path, sizeof(Req.Data.FolderDesc.Path));
    CopySafeStr(Req.Data.FolderDesc.Description, sizeof(Req.Data.FolderDesc.Description), DescUtf8);
    CopySafeStr(Req.Data.FolderDesc.Source, sizeof(Req.Data.FolderDesc.Source), (SourceUtf8 && SourceUtf8[0]) ? SourceUtf8 : "user");
    CopySafeStr(Req.Data.FolderDesc.Tags, sizeof(Req.Data.FolderDesc.Tags), TagsUtf8 ? TagsUtf8 : "");

    return SendAlpcRequest(Port, &Req, &Resp);
}

LIBAI_API
DWORD
WINAPI
AiClientGetFolderDescription(
    _In_opt_ HANDLE Port,
    _In_ PCWSTR FolderPath,
    _Out_ PSTR DescUtf8,
    _In_ DWORD DescCap,
    _Out_opt_ PSTR SourceUtf8,
    _In_ DWORD SourceCap)
{
    WCHAR FullPath[MAX_PATH];
    AI_MSG_PAYLOAD Req;
    AI_MSG_PAYLOAD Resp;
    DWORD Result;

    if (FolderPath == NULL || DescUtf8 == NULL || DescCap == 0)
        return AiStatusInvalidArg;

    Result = AiAdsReadFolderDescription(FolderPath, DescUtf8, DescCap, SourceUtf8, SourceCap, NULL, 0);
    if (Result == AiStatusOk || Port == NULL)
        return Result;

    if (!NormalizeFolderPath(FolderPath, FullPath, ARRAYSIZE(FullPath)))
        return AiStatusInvalidArg;

    RtlZeroMemory(&Req, sizeof(Req));
    Req.Opcode = AiOpGetFolderDesc;
    WideToUtf8(FullPath, Req.Data.FolderDesc.Path, sizeof(Req.Data.FolderDesc.Path));

    Result = SendAlpcRequest(Port, &Req, &Resp);
    if (Result == AiStatusOk)
    {
        CopySafeStr(DescUtf8, DescCap, Resp.Data.FolderDesc.Description);
        CopySafeStr(SourceUtf8, SourceCap, Resp.Data.FolderDesc.Source);
    }
    return Result;
}

LIBAI_API
DWORD
WINAPI
AiClientClearFolderDescription(
    _In_opt_ HANDLE Port,
    _In_ PCWSTR FolderPath)
{
    WCHAR FullPath[MAX_PATH];
    AI_MSG_PAYLOAD Req;
    AI_MSG_PAYLOAD Resp;
    DWORD AdsResult;

    if (FolderPath == NULL)
        return AiStatusInvalidArg;

    AdsResult = AiAdsClearFolderDescription(FolderPath);
    if (AdsResult != AiStatusOk)
        return AdsResult;

    if (Port == NULL)
        return AiStatusOk;

    if (!NormalizeFolderPath(FolderPath, FullPath, ARRAYSIZE(FullPath)))
        return AiStatusInvalidArg;

    RtlZeroMemory(&Req, sizeof(Req));
    Req.Opcode = AiOpClearFolderDesc;
    WideToUtf8(FullPath, Req.Data.FolderDesc.Path, sizeof(Req.Data.FolderDesc.Path));

    return SendAlpcRequest(Port, &Req, &Resp);
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Reserved);
    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);
    return TRUE;
}
