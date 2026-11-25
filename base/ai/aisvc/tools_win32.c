/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Win32/NTFS OS tool backend for aisvc agent loop
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ndk/lpcfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>

#include "ai_json.h"
#include "libai.h"
#include "tools_win32.h"

typedef struct _AI_DESKTOP_ALPC_PACKET
{
    PORT_MESSAGE Header;
    AI_MSG_PAYLOAD Payload;
} AI_DESKTOP_ALPC_PACKET;

static BOOL Utf8ToWide(const char *Utf8, PWSTR OutW, DWORD OutChars)
{
    if (Utf8 == NULL || OutW == NULL || OutChars == 0)
        return FALSE;
    return MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, OutW, (int)OutChars) > 0;
}

static BOOL WideToUtf8(PCWSTR Wide, char *OutUtf8, size_t OutCap)
{
    if (Wide == NULL || OutUtf8 == NULL || OutCap == 0)
        return FALSE;
    if (WideCharToMultiByte(CP_UTF8, 0, Wide, -1, OutUtf8, (int)OutCap, NULL, NULL) <= 0)
    {
        OutUtf8[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static int Win32ReadFileSnippet(
    void *UserCtx,
    const char *PathUtf8,
    size_t MaxBytes,
    char *OutBuf,
    size_t OutCap)
{
    WCHAR PathW[MAX_PATH];
    HANDLE File;
    DWORD ToRead;
    DWORD BytesRead = 0;
    BOOL Ok;

    UNREFERENCED_PARAMETER(UserCtx);

    if (!Utf8ToWide(PathUtf8, PathW, ARRAYSIZE(PathW)) || OutBuf == NULL || OutCap < 2)
        return -1;

    File = CreateFileW(
        PathW,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (File == INVALID_HANDLE_VALUE)
        return -1;

    ToRead = (DWORD)((MaxBytes < OutCap - 1) ? MaxBytes : (OutCap - 1));
    Ok = ReadFile(File, OutBuf, ToRead, &BytesRead, NULL);
    CloseHandle(File);
    if (!Ok)
        return -1;

    OutBuf[BytesRead] = '\0';
    return 0;
}

static int Win32ListDirectory(
    void *UserCtx,
    const char *DirPathUtf8,
    char *OutBuf,
    size_t OutCap)
{
    WCHAR DirW[MAX_PATH];
    WCHAR PatternW[MAX_PATH + 4];
    WIN32_FIND_DATAW FindData;
    HANDLE Find;
    size_t Pos = 0;
    DWORD Count = 0;

    UNREFERENCED_PARAMETER(UserCtx);

    if (!Utf8ToWide(DirPathUtf8, DirW, ARRAYSIZE(DirW)) || OutBuf == NULL || OutCap < 16)
        return -1;
    OutBuf[0] = '\0';

    _snwprintf(PatternW, ARRAYSIZE(PatternW), L"%s\\*", DirW);
    PatternW[ARRAYSIZE(PatternW) - 1] = UNICODE_NULL;

    Find = FindFirstFileW(PatternW, &FindData);
    if (Find == INVALID_HANDLE_VALUE)
        return -1;

    do
    {
        char NameUtf8[260];
        WCHAR ChildW[MAX_PATH];
        char Desc[160];
        int n;

        if (wcscmp(FindData.cFileName, L".") == 0 || wcscmp(FindData.cFileName, L"..") == 0)
            continue;
        if (!WideToUtf8(FindData.cFileName, NameUtf8, sizeof(NameUtf8)))
            continue;

        Desc[0] = '\0';
        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            _snwprintf(ChildW, ARRAYSIZE(ChildW), L"%s\\%s", DirW, FindData.cFileName);
            ChildW[ARRAYSIZE(ChildW) - 1] = UNICODE_NULL;
            AiAdsReadFolderDescription(ChildW, Desc, sizeof(Desc), NULL, 0, NULL, 0);
        }

        if (Desc[0] != '\0')
        {
            n = _snprintf(OutBuf + Pos, OutCap - Pos, "[DIR] %s -- %s\n", NameUtf8, Desc);
        }
        else if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            n = _snprintf(OutBuf + Pos, OutCap - Pos, "[DIR] %s\n", NameUtf8);
        }
        else
        {
            n = _snprintf(OutBuf + Pos, OutCap - Pos, "[FILE] %s (%lu bytes)\n",
                          NameUtf8, (unsigned long)FindData.nFileSizeLow);
        }

        if (n < 0 || Pos + (size_t)n >= OutCap)
            break;
        Pos += (size_t)n;
        Count++;
    } while (Count < 32 && FindNextFileW(Find, &FindData));

    FindClose(Find);
    if (Pos == 0)
        _snprintf(OutBuf, OutCap, "(empty)");
    return 0;
}

static int Win32WriteFileUtf8(
    void *UserCtx,
    const char *PathUtf8,
    const char *ContentUtf8,
    char *ErrOut,
    size_t ErrCap)
{
    AI_WIN32_TOOL_CTX *Ctx = (AI_WIN32_TOOL_CTX *)UserCtx;
    WCHAR PathW[MAX_PATH];
    HANDLE File;
    DWORD Len;
    DWORD Written = 0;
    BOOL Ok;

    if (!Utf8ToWide(PathUtf8, PathW, ARRAYSIZE(PathW)) || ContentUtf8 == NULL)
    {
        if (ErrOut && ErrCap)
            _snprintf(ErrOut, ErrCap, "invalid path");
        return -1;
    }

    File = CreateFileW(
        PathW,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        if (ErrOut && ErrCap)
            _snprintf(ErrOut, ErrCap, "CreateFileW failed (%lu)", (unsigned long)GetLastError());
        return -1;
    }

    Len = (DWORD)strlen(ContentUtf8);
    Ok = WriteFile(File, ContentUtf8, Len, &Written, NULL);
    CloseHandle(File);
    if (!Ok || Written != Len)
    {
        if (ErrOut && ErrCap)
            _snprintf(ErrOut, ErrCap, "WriteFile failed (%lu)", (unsigned long)GetLastError());
        return -1;
    }

    if (Ctx != NULL && Ctx->Indexer != NULL)
        AiIndexerIndexSinglePath(Ctx->Indexer, PathW);
    return 0;
}

static int Win32DeleteFileUtf8(
    void *UserCtx,
    const char *PathUtf8,
    char *ErrOut,
    size_t ErrCap)
{
    AI_WIN32_TOOL_CTX *Ctx = (AI_WIN32_TOOL_CTX *)UserCtx;
    WCHAR PathW[MAX_PATH];

    if (!Utf8ToWide(PathUtf8, PathW, ARRAYSIZE(PathW)))
    {
        if (ErrOut && ErrCap)
            _snprintf(ErrOut, ErrCap, "invalid path");
        return -1;
    }

    if (!DeleteFileW(PathW))
    {
        if (ErrOut && ErrCap)
            _snprintf(ErrOut, ErrCap, "DeleteFileW failed (%lu)", (unsigned long)GetLastError());
        return -1;
    }

    if (Ctx != NULL && Ctx->Store != NULL)
        AiStoreDeletePath(Ctx->Store, PathUtf8);
    return 0;
}

static int Win32RunCommandUtf8(
    void *UserCtx,
    const char *CommandUtf8,
    const char *CwdUtf8,
    uint32_t TimeoutMs,
    char *OutBuf,
    size_t OutCap,
    int *OutExitCode)
{
    SECURITY_ATTRIBUTES Sa;
    HANDLE ReadPipe = NULL;
    HANDLE WritePipe = NULL;
    HANDLE Job = NULL;
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    WCHAR CmdW[2048];
    WCHAR FullCmdW[2100];
    WCHAR CwdW[MAX_PATH];
    PCWSTR UseCwd = NULL;
    DWORD TotalRead = 0;
    DWORD ExitCode = 0;
    ULONGLONG StartMs;
    ULONGLONG LimitMs;
    BOOL Truncated = FALSE;
    BOOL TimedOut = FALSE;

    UNREFERENCED_PARAMETER(UserCtx);

    if (!Utf8ToWide(CommandUtf8, CmdW, ARRAYSIZE(CmdW)) || OutBuf == NULL || OutCap < 32)
        return -1;
    OutBuf[0] = '\0';
    if (OutExitCode)
        *OutExitCode = -1;

    if (CwdUtf8 != NULL && CwdUtf8[0] != '\0')
    {
        if (!Utf8ToWide(CwdUtf8, CwdW, ARRAYSIZE(CwdW)))
            return -1;
        UseCwd = CwdW;
    }

    if (TimeoutMs == 0)
        TimeoutMs = 15000U;
    if (TimeoutMs < 1000U)
        TimeoutMs = 1000U;
    if (TimeoutMs > 60000U)
        TimeoutMs = 60000U;
    LimitMs = (ULONGLONG)TimeoutMs;

    _snwprintf(FullCmdW, ARRAYSIZE(FullCmdW), L"cmd.exe /c %s", CmdW);
    FullCmdW[ARRAYSIZE(FullCmdW) - 1] = UNICODE_NULL;

    memset(&Sa, 0, sizeof(Sa));
    Sa.nLength = sizeof(Sa);
    Sa.bInheritHandle = TRUE;

    if (!CreatePipe(&ReadPipe, &WritePipe, &Sa, 0))
        return -1;
    SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0);

    memset(&Si, 0, sizeof(Si));
    Si.cb = sizeof(Si);
    Si.dwFlags = STARTF_USESTDHANDLES;
    Si.hStdOutput = WritePipe;
    Si.hStdError = WritePipe;
    Si.hStdInput = INVALID_HANDLE_VALUE;

    memset(&Pi, 0, sizeof(Pi));
    if (!CreateProcessW(NULL, FullCmdW, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, UseCwd, &Si, &Pi))
    {
        CloseHandle(ReadPipe);
        CloseHandle(WritePipe);
        return -1;
    }

    CloseHandle(WritePipe);
    WritePipe = NULL;

    Job = CreateJobObjectW(NULL, NULL);
    if (Job != NULL)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Jeli;
        memset(&Jeli, 0, sizeof(Jeli));
        Jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Jeli, sizeof(Jeli));
        AssignProcessToJobObject(Job, Pi.hProcess);
    }

    ResumeThread(Pi.hThread);
    StartMs = GetTickCount64();

    for (;;)
    {
        DWORD Avail = 0;
        BOOL PeekOk = PeekNamedPipe(ReadPipe, NULL, 0, NULL, &Avail, NULL);

        if (PeekOk && Avail > 0)
        {
            char Discard[256];
            DWORD Chunk = 0;
            DWORD Space = (TotalRead < OutCap - 24) ? (DWORD)(OutCap - 24 - TotalRead) : 0;

            if (Space > 0)
            {
                DWORD ToRead = (Avail < Space) ? Avail : Space;
                if (ReadFile(ReadPipe, OutBuf + TotalRead, ToRead, &Chunk, NULL) && Chunk > 0)
                    TotalRead += Chunk;
                else
                    break;
            }
            else
            {
                DWORD ToRead = (Avail < sizeof(Discard)) ? Avail : (DWORD)sizeof(Discard);
                Truncated = TRUE;
                if (!ReadFile(ReadPipe, Discard, ToRead, &Chunk, NULL) || Chunk == 0)
                    break;
            }
            continue;
        }

        if (WaitForSingleObject(Pi.hProcess, 25) == WAIT_OBJECT_0)
        {
            /* Drain any final buffered bytes after process exit */
            while (PeekNamedPipe(ReadPipe, NULL, 0, NULL, &Avail, NULL) && Avail > 0)
            {
                DWORD Chunk = 0;
                DWORD Space = (TotalRead < OutCap - 24) ? (DWORD)(OutCap - 24 - TotalRead) : 0;
                if (Space == 0)
                {
                    Truncated = TRUE;
                    break;
                }
                if (!ReadFile(ReadPipe, OutBuf + TotalRead, (Avail < Space) ? Avail : Space, &Chunk, NULL) ||
                    Chunk == 0)
                {
                    break;
                }
                TotalRead += Chunk;
            }
            break;
        }

        if (!PeekOk)
            break;

        if ((GetTickCount64() - StartMs) >= LimitMs)
        {
            TimedOut = TRUE;
            if (Job != NULL)
                TerminateJobObject(Job, 124);
            TerminateProcess(Pi.hProcess, 124);
            WaitForSingleObject(Pi.hProcess, 1000);
            break;
        }
    }

    OutBuf[TotalRead] = '\0';
    if (Truncated && TotalRead + 18 < OutCap)
        strcat(OutBuf, "\n...[truncated]");
    if (TimedOut && strlen(OutBuf) + 20 < OutCap)
        strcat(OutBuf, "\n...[timed out]");

    GetExitCodeProcess(Pi.hProcess, &ExitCode);
    if (OutExitCode)
        *OutExitCode = TimedOut ? 124 : (int)ExitCode;

    if (Job != NULL)
        CloseHandle(Job);
    CloseHandle(Pi.hThread);
    CloseHandle(Pi.hProcess);
    CloseHandle(ReadPipe);
    return 0;
}

static int Win32WriteFolderAds(
    void *UserCtx,
    const char *DirPathUtf8,
    const char *DescUtf8,
    const char *SourceUtf8,
    const char *TagsUtf8)
{
    WCHAR DirW[MAX_PATH];

    UNREFERENCED_PARAMETER(UserCtx);

    if (!Utf8ToWide(DirPathUtf8, DirW, ARRAYSIZE(DirW)))
        return -1;
    return (AiAdsWriteFolderDescription(DirW, DescUtf8, SourceUtf8, TagsUtf8) == AiStatusOk) ? 0 : -1;
}

static int Win32GetSystemSummary(
    void *UserCtx,
    char *OutBuf,
    size_t OutCap)
{
    AI_WIN32_TOOL_CTX *Ctx = (AI_WIN32_TOOL_CTX *)UserCtx;
    AI_STORE_STATS Stats;
    MEMORYSTATUSEX Mem;
    ULONGLONG UptimeSec = GetTickCount64() / 1000ULL;

    memset(&Stats, 0, sizeof(Stats));
    if (Ctx != NULL && Ctx->Store != NULL)
        AiStoreGetStats(Ctx->Store, &Stats);

    memset(&Mem, 0, sizeof(Mem));
    Mem.dwLength = sizeof(Mem);
    GlobalMemoryStatusEx(&Mem);

    _snprintf(OutBuf, OutCap,
              "{\"os\":\"LiberNT\",\"uptime_sec\":%llu,\"mem_total_mb\":%llu,\"mem_avail_mb\":%llu,"
              "\"files\":%llu,\"dirs\":%llu,\"described_dirs\":%llu,\"embeddings\":%llu,\"audits\":%llu}",
              (unsigned long long)UptimeSec,
              (unsigned long long)(Mem.ullTotalPhys / (1024ULL * 1024ULL)),
              (unsigned long long)(Mem.ullAvailPhys / (1024ULL * 1024ULL)),
              (unsigned long long)Stats.FileCount,
              (unsigned long long)Stats.DirCount,
              (unsigned long long)Stats.DescribedDirCount,
              (unsigned long long)Stats.EmbeddingCount,
              (unsigned long long)Stats.AuditCount);
    OutBuf[OutCap - 1] = '\0';
    return 0;
}

static int Win32DesktopTool(
    void *UserCtx,
    const char *ToolName,
    const char *ArgsJson,
    char *OutJson,
    size_t OutCap)
{
    UNICODE_STRING PortName;
    ALPC_PORT_ATTRIBUTES Attrs;
    AI_DESKTOP_ALPC_PACKET ConnPkt;
    AI_DESKTOP_ALPC_PACKET SendPkt;
    AI_DESKTOP_ALPC_PACKET RecvPkt;
    SIZE_T Len;
    LARGE_INTEGER Timeout;
    HANDLE Port = NULL;
    NTSTATUS Status;
    size_t NameLen;
    size_t ArgsLen;

    UNREFERENCED_PARAMETER(UserCtx);

    if (ToolName == NULL || OutJson == NULL || OutCap < 16)
        return -1;
    OutJson[0] = '\0';

    RtlInitUnicodeString(&PortName, AI_DESKTOP_PORT_NAME);
    RtlZeroMemory(&Attrs, sizeof(Attrs));
    Attrs.SecurityQos.Length = sizeof(Attrs.SecurityQos);
    Attrs.SecurityQos.ImpersonationLevel = SecurityImpersonation;
    Attrs.SecurityQos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    Attrs.SecurityQos.EffectiveOnly = TRUE;
    Attrs.MaxMessageLength = sizeof(AI_DESKTOP_ALPC_PACKET);

    RtlZeroMemory(&ConnPkt, sizeof(ConnPkt));
    ConnPkt.Header.u1.s1.DataLength = sizeof(ConnPkt.Payload);
    ConnPkt.Header.u1.s1.TotalLength = sizeof(ConnPkt);
    ConnPkt.Payload.Version = AI_PROTOCOL_VERSION;
    ConnPkt.Payload.Opcode = AiOpPing;

    Len = sizeof(ConnPkt);
    Timeout.QuadPart = -(LONGLONG)3000 * 10000;
    Status = NtAlpcConnectPort(
        &Port,
        &PortName,
        NULL,
        &Attrs,
        ALPC_SYNC_CONNECTION,
        NULL,
        &ConnPkt.Header,
        &Len,
        NULL,
        NULL,
        &Timeout);
    if (!NT_SUCCESS(Status))
    {
        _snprintf(OutJson, OutCap, "{\"error\":\"aishell desktop bridge unavailable (0x%08lx)\"}",
                  (unsigned long)Status);
        OutJson[OutCap - 1] = '\0';
        return -1;
    }

    RtlZeroMemory(&SendPkt, sizeof(SendPkt));
    RtlZeroMemory(&RecvPkt, sizeof(RecvPkt));
    SendPkt.Header.u1.s1.DataLength = sizeof(SendPkt.Payload);
    SendPkt.Header.u1.s1.TotalLength = sizeof(SendPkt);
    SendPkt.Payload.Version = AI_PROTOCOL_VERSION;
    SendPkt.Payload.Opcode = AiOpDesktopTool;

    NameLen = strlen(ToolName);
    if (NameLen >= sizeof(SendPkt.Payload.Data.DesktopTool.ToolName))
        NameLen = sizeof(SendPkt.Payload.Data.DesktopTool.ToolName) - 1;
    memcpy(SendPkt.Payload.Data.DesktopTool.ToolName, ToolName, NameLen);

    if (ArgsJson != NULL)
    {
        ArgsLen = strlen(ArgsJson);
        if (ArgsLen >= sizeof(SendPkt.Payload.Data.DesktopTool.ArgsJson))
            ArgsLen = sizeof(SendPkt.Payload.Data.DesktopTool.ArgsJson) - 1;
        memcpy(SendPkt.Payload.Data.DesktopTool.ArgsJson, ArgsJson, ArgsLen);
    }

    Len = sizeof(RecvPkt);
    Timeout.QuadPart = -(LONGLONG)10000 * 10000;
    Status = NtAlpcSendWaitReceivePort(
        Port,
        ALPC_MSGFLG_SYNC_REQUEST,
        &SendPkt.Header,
        NULL,
        &RecvPkt.Header,
        &Len,
        NULL,
        &Timeout);

    NtAlpcDisconnectPort(Port, 0);
    NtClose(Port);

    if (!NT_SUCCESS(Status))
    {
        _snprintf(OutJson, OutCap, "{\"error\":\"aishell ALPC call failed (0x%08lx)\"}",
                  (unsigned long)Status);
        OutJson[OutCap - 1] = '\0';
        return -1;
    }

    RecvPkt.Payload.Data.DesktopTool.ResultJson[sizeof(RecvPkt.Payload.Data.DesktopTool.ResultJson) - 1] = '\0';
    _snprintf(OutJson, OutCap, "%s", RecvPkt.Payload.Data.DesktopTool.ResultJson);
    OutJson[OutCap - 1] = '\0';
    return (RecvPkt.Payload.Status == AiStatusOk) ? 0 : -1;
}

void AiWin32BuildAgentEnv(
    AI_WIN32_TOOL_CTX *Ctx,
    AI_AGENT_ENV *OutEnv)
{
    memset(OutEnv, 0, sizeof(*OutEnv));
    OutEnv->UserCtx = Ctx;
    OutEnv->ReadFileSnippet = Win32ReadFileSnippet;
    OutEnv->ListDirectory = Win32ListDirectory;
    OutEnv->WriteFileUtf8 = Win32WriteFileUtf8;
    OutEnv->DeleteFileUtf8 = Win32DeleteFileUtf8;
    OutEnv->RunCommandUtf8 = Win32RunCommandUtf8;
    OutEnv->WriteFolderAds = Win32WriteFolderAds;
    OutEnv->GetSystemSummary = Win32GetSystemSummary;
    OutEnv->DesktopTool = Win32DesktopTool;
}
