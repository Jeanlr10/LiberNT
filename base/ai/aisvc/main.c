/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Session 0 NT service and ALPC broker for LiberNT semantic search and AI subsystem
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
#include <winsvc.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ndk/lpcfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>

#include "ai_protocol.h"
#include "ai_store.h"
#include "indexer.h"

typedef struct _AI_ALPC_PACKET
{
    PORT_MESSAGE Header;
    AI_MSG_PAYLOAD Payload;
} AI_ALPC_PACKET;

C_ASSERT(sizeof(AI_ALPC_PACKET) <= 4096);

typedef struct _AI_CLIENT_SLOT
{
    HANDLE ServerPort;
    BOOLEAN Active;
} AI_CLIENT_SLOT;

#define AI_MAX_CLIENTS 64

static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static SERVICE_STATUS g_ServiceStatus;
static volatile LONG g_Stopping = 0;
static HANDLE g_StopEvent = NULL;
static HANDLE g_ConnectionPort = NULL;
static AI_STORE *g_Store = NULL;
static AI_INDEXER *g_Indexer = NULL;
static AI_CLIENT_SLOT g_Clients[AI_MAX_CLIENTS];

static VOID ReportServiceStatus(DWORD CurrentState, DWORD Win32ExitCode, DWORD WaitHint)
{
    static DWORD CheckPoint = 1;

    if (g_StatusHandle == NULL)
        return;

    g_ServiceStatus.dwCurrentState = CurrentState;
    g_ServiceStatus.dwWin32ExitCode = Win32ExitCode;
    g_ServiceStatus.dwWaitHint = WaitHint;

    if (CurrentState == SERVICE_START_PENDING)
        g_ServiceStatus.dwControlsAccepted = 0;
    else
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    if (CurrentState == SERVICE_RUNNING || CurrentState == SERVICE_STOPPED)
        g_ServiceStatus.dwCheckPoint = 0;
    else
        g_ServiceStatus.dwCheckPoint = CheckPoint++;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

static VOID EnsureDataDir(VOID)
{
    CreateDirectoryW(L"C:\\ProgramData", NULL);
    CreateDirectoryW(L"C:\\ProgramData\\LiberNT", NULL);
    CreateDirectoryW(L"C:\\ProgramData\\LiberNT\\AI", NULL);
}

static AI_CLIENT_SLOT *AllocClientSlot(VOID)
{
    DWORD i;

    for (i = 0; i < AI_MAX_CLIENTS; i++)
    {
        if (!g_Clients[i].Active)
        {
            g_Clients[i].Active = TRUE;
            g_Clients[i].ServerPort = NULL;
            return &g_Clients[i];
        }
    }
    return NULL;
}

static VOID FreeClientSlot(AI_CLIENT_SLOT *Slot)
{
    if (Slot == NULL || Slot < &g_Clients[0] || Slot >= &g_Clients[AI_MAX_CLIENTS])
        return;

    if (Slot->ServerPort != NULL)
    {
        NtAlpcDisconnectPort(Slot->ServerPort, 0);
        NtClose(Slot->ServerPort);
        Slot->ServerPort = NULL;
    }
    Slot->Active = FALSE;
}

static VOID CloseAllClients(VOID)
{
    DWORD i;

    for (i = 0; i < AI_MAX_CLIENTS; i++)
    {
        if (g_Clients[i].Active)
            FreeClientSlot(&g_Clients[i]);
    }
}

static VOID HandleRequest(const AI_MSG_PAYLOAD *Req, AI_MSG_PAYLOAD *Resp)
{
    RtlZeroMemory(Resp, sizeof(*Resp));
    Resp->Version = AI_PROTOCOL_VERSION;
    Resp->Opcode = Req->Opcode;
    Resp->Status = AiStatusOk;

    if (Req->Version != AI_PROTOCOL_VERSION)
    {
        Resp->Status = AiStatusInvalidArg;
        return;
    }

    switch (Req->Opcode)
    {
        case AiOpPing:
            Resp->Status = AiStatusOk;
            break;

        case AiOpGetStatus:
        {
            AI_STORE_STATS Stats;

            RtlZeroMemory(&Stats, sizeof(Stats));
            if (g_Store == NULL || AiStoreGetStats(g_Store, &Stats) != 0)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            Resp->Data.StatusInfo.ProtocolVersion = AI_PROTOCOL_VERSION;
            Resp->Data.StatusInfo.ServiceReady = 1;
            Resp->Data.StatusInfo.IndexingActive = (g_Indexer && AiIndexerIsBusy(g_Indexer)) ? 1 : 0;
            Resp->Data.StatusInfo.WatchedRoots = g_Indexer ? AiIndexerGetRootCount(g_Indexer) : 0;
            Resp->Data.StatusInfo.FileCount = Stats.FileCount;
            Resp->Data.StatusInfo.DirCount = Stats.DirCount;
            Resp->Data.StatusInfo.DescribedDirCount = Stats.DescribedDirCount;
            Resp->Data.StatusInfo.EmbeddingCount = Stats.EmbeddingCount;
            break;
        }

        case AiOpSearch:
        {
            uint32_t Limit = Req->Data.SearchReq.MaxResults;
            uint32_t Count = 0;
            CHAR Query[AI_MAX_QUERY_UTF8];

            if (Limit == 0 || Limit > AI_MAX_SEARCH_RESULTS)
                Limit = AI_MAX_SEARCH_RESULTS;
            memcpy(Query, Req->Data.SearchReq.Query, sizeof(Query));
            Query[sizeof(Query) - 1] = '\0';

            if (g_Store == NULL ||
                AiStoreSearch(g_Store, Query, NULL, 0, Resp->Data.SearchResp.Hits, Limit, &Count) != 0)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            Resp->Count = Count;
            break;
        }

        case AiOpIndexNow:
        {
            DWORD Indexed = 0;

            if (g_Indexer == NULL)
            {
                Resp->Status = AiStatusInternal;
                break;
            }
            Indexed = AiIndexerCrawlNow(g_Indexer);
            Resp->Data.IndexResp.IndexedCount = Indexed;
            if (g_Store != NULL)
                AiStoreLogAudit(g_Store, 0, "index_now", "all_roots", "ok", "manual_crawl");
            break;
        }

        case AiOpSetFolderDesc:
        {
            CHAR Path[AI_MAX_PATH_UTF8];
            CHAR Desc[AI_MAX_DESC_UTF8];
            CHAR Source[AI_MAX_SOURCE_UTF8];
            CHAR Tags[AI_MAX_TAGS_UTF8];
            int Rc;

            memcpy(Path, Req->Data.FolderDesc.Path, sizeof(Path));
            Path[sizeof(Path) - 1] = '\0';
            memcpy(Desc, Req->Data.FolderDesc.Description, sizeof(Desc));
            Desc[sizeof(Desc) - 1] = '\0';
            memcpy(Source, Req->Data.FolderDesc.Source, sizeof(Source));
            Source[sizeof(Source) - 1] = '\0';
            memcpy(Tags, Req->Data.FolderDesc.Tags, sizeof(Tags));
            Tags[sizeof(Tags) - 1] = '\0';

            if (g_Store == NULL)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            Rc = AiStoreSetFolderDescription(g_Store, Path, Desc, Source[0] ? Source : "user", Tags);
            if (Rc != 0)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            AiStoreLogAudit(g_Store, 1, "set_folder_desc", Path, "ok", Source[0] ? Source : "user");
            break;
        }

        case AiOpGetFolderDesc:
        {
            CHAR Path[AI_MAX_PATH_UTF8];
            int Rc;

            memcpy(Path, Req->Data.FolderDesc.Path, sizeof(Path));
            Path[sizeof(Path) - 1] = '\0';

            if (g_Store == NULL)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            Rc = AiStoreGetFolderDescription(
                g_Store,
                Path,
                Resp->Data.FolderDesc.Description,
                sizeof(Resp->Data.FolderDesc.Description),
                Resp->Data.FolderDesc.Source,
                sizeof(Resp->Data.FolderDesc.Source));
            if (Rc != 0)
                Resp->Status = AiStatusNotFound;
            break;
        }

        case AiOpClearFolderDesc:
        {
            CHAR Path[AI_MAX_PATH_UTF8];
            int Rc;

            memcpy(Path, Req->Data.FolderDesc.Path, sizeof(Path));
            Path[sizeof(Path) - 1] = '\0';

            if (g_Store == NULL)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            Rc = AiStoreClearFolderDescription(g_Store, Path);
            if (Rc != 0)
            {
                Resp->Status = AiStatusDbError;
                break;
            }
            AiStoreLogAudit(g_Store, 1, "clear_folder_desc", Path, "ok", "");
            break;
        }

        default:
            Resp->Status = AiStatusInvalidArg;
            break;
    }
}

static NTSTATUS CreateAiAlpcPort(PHANDLE OutPort)
{
    UNICODE_STRING PortName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    ALPC_PORT_ATTRIBUTES PortAttributes;
    SECURITY_DESCRIPTOR Sd;

    RtlInitUnicodeString(&PortName, AI_PORT_NAME);

    if (!InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&Sd, TRUE, NULL, FALSE))
    {
        return STATUS_UNSUCCESSFUL;
    }

    InitializeObjectAttributes(&ObjectAttributes, &PortName, OBJ_CASE_INSENSITIVE, NULL, &Sd);

    RtlZeroMemory(&PortAttributes, sizeof(PortAttributes));
    PortAttributes.Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
    PortAttributes.SecurityQos.Length = sizeof(PortAttributes.SecurityQos);
    PortAttributes.SecurityQos.ImpersonationLevel = SecurityImpersonation;
    PortAttributes.SecurityQos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    PortAttributes.SecurityQos.EffectiveOnly = TRUE;
    PortAttributes.MaxMessageLength = sizeof(AI_ALPC_PACKET);

    return NtAlpcCreatePort(OutPort, &ObjectAttributes, &PortAttributes);
}

static DWORD RunAiServer(BOOL ConsoleMode)
{
    NTSTATUS Status;
    AI_ALPC_PACKET Message;
    AI_ALPC_PACKET Reply;
    BYTE AttrBuf[sizeof(ALPC_MESSAGE_ATTRIBUTES) + sizeof(ALPC_CONTEXT_ATTR) + 64];
    PALPC_MESSAGE_ATTRIBUTES RecvAttrs = (PALPC_MESSAGE_ATTRIBUTES)AttrBuf;
    SIZE_T AttrSize = 0;

    EnsureDataDir();

    if (AiStoreOpen("C:\\ProgramData\\LiberNT\\AI\\index.db", &g_Store) != 0)
    {
        OutputDebugStringA("aisvc: failed to open SQLite store\n");
        return ERROR_GEN_FAILURE;
    }

    if (!AiIndexerStart(g_Store, &g_Indexer))
    {
        OutputDebugStringA("aisvc: failed to start NTFS indexer\n");
        AiStoreClose(g_Store);
        g_Store = NULL;
        return ERROR_GEN_FAILURE;
    }

    Status = CreateAiAlpcPort(&g_ConnectionPort);
    if (!NT_SUCCESS(Status))
    {
        CHAR Dbg[128];
        _snprintf(Dbg, sizeof(Dbg), "aisvc: NtAlpcCreatePort failed: 0x%08lx\n", (unsigned long)Status);
        OutputDebugStringA(Dbg);
        AiIndexerStop(g_Indexer);
        g_Indexer = NULL;
        AiStoreClose(g_Store);
        g_Store = NULL;
        return ERROR_GEN_FAILURE;
    }

    AiStoreLogAudit(g_Store, 0, "service_start", "AiSvc", "ok", ConsoleMode ? "console" : "scm");
    OutputDebugStringA("aisvc: ALPC broker ready on \\RPC Control\\LiberNtAiPort\n");

    if (!ConsoleMode)
        ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);

    while (!InterlockedCompareExchange(&g_Stopping, 0, 0))
    {
        SIZE_T BufLen = sizeof(Message);
        LARGE_INTEGER Timeout;
        UCHAR MsgType;
        PALPC_CONTEXT_ATTR ContextAttr = NULL;
        AI_CLIENT_SLOT *Slot = NULL;

        RtlZeroMemory(&Message, sizeof(Message));
        RtlZeroMemory(AttrBuf, sizeof(AttrBuf));
        AlpcInitializeMessageAttribute(ALPC_MESSAGE_CONTEXT_ATTRIBUTE, RecvAttrs, sizeof(AttrBuf), &AttrSize);

        Timeout.QuadPart = -(LONGLONG)500 * 10000;
        Status = NtAlpcSendWaitReceivePort(
            g_ConnectionPort,
            0,
            NULL,
            NULL,
            &Message.Header,
            &BufLen,
            RecvAttrs,
            &Timeout);
        if (Status == STATUS_TIMEOUT)
            continue;
        if (!NT_SUCCESS(Status))
            break;

        MsgType = (UCHAR)(Message.Header.u2.s2.Type & 0xFF);
        if (RecvAttrs->ValidAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE)
            ContextAttr = AlpcGetMessageAttribute(RecvAttrs, ALPC_MESSAGE_CONTEXT_ATTRIBUTE);
        if (ContextAttr != NULL)
            Slot = (AI_CLIENT_SLOT *)ContextAttr->PortContext;

        if (MsgType == LPC_CONNECTION_REQUEST)
        {
            AI_CLIENT_SLOT *NewSlot = AllocClientSlot();
            BOOLEAN Accept = (NewSlot != NULL && Message.Payload.Version == AI_PROTOCOL_VERSION);
            HANDLE ServerPort = NULL;

            Status = NtAlpcAcceptConnectPort(
                &ServerPort,
                g_ConnectionPort,
                0,
                NULL,
                NULL,
                Accept ? NewSlot : NULL,
                &Message.Header,
                NULL,
                Accept);
            if (Accept && NT_SUCCESS(Status))
            {
                NewSlot->ServerPort = ServerPort;
            }
            else if (NewSlot != NULL)
            {
                FreeClientSlot(NewSlot);
            }
        }
        else if (MsgType == LPC_REQUEST)
        {
            RtlZeroMemory(&Reply, sizeof(Reply));
            Reply.Header = Message.Header;
            Reply.Header.u1.s1.DataLength = sizeof(Reply.Payload);
            Reply.Header.u1.s1.TotalLength = sizeof(Reply);

            HandleRequest(&Message.Payload, &Reply.Payload);

            if (Slot != NULL && Slot->Active && Slot->ServerPort != NULL)
            {
                NtAlpcSendWaitReceivePort(
                    Slot->ServerPort,
                    ALPC_MSGFLG_REPLY_MESSAGE,
                    &Reply.Header,
                    NULL,
                    NULL,
                    NULL,
                    NULL,
                    NULL);
            }
        }
        else if (MsgType == LPC_PORT_CLOSED || MsgType == LPC_CLIENT_DIED)
        {
            if (Slot != NULL)
                FreeClientSlot(Slot);
        }
    }

    CloseAllClients();
    if (g_ConnectionPort != NULL)
    {
        NtClose(g_ConnectionPort);
        g_ConnectionPort = NULL;
    }
    if (g_Indexer != NULL)
    {
        AiIndexerStop(g_Indexer);
        g_Indexer = NULL;
    }
    if (g_Store != NULL)
    {
        AiStoreLogAudit(g_Store, 0, "service_stop", "AiSvc", "ok", "");
        AiStoreClose(g_Store);
        g_Store = NULL;
    }

    return ERROR_SUCCESS;
}

static DWORD WINAPI ServiceCtrlHandler(DWORD Control, DWORD EventType, PVOID EventData, PVOID Context)
{
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Context);

    switch (Control)
    {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            InterlockedExchange(&g_Stopping, 1);
            if (g_StopEvent != NULL)
                SetEvent(g_StopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static VOID WINAPI ServiceMain(DWORD Argc, PWSTR *Argv)
{
    DWORD ExitCode;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    RtlZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    g_StatusHandle = RegisterServiceCtrlHandlerExW(AI_SERVICE_NAME, ServiceCtrlHandler, NULL);
    if (g_StatusHandle == NULL)
        return;

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    ExitCode = RunAiServer(FALSE);

    if (g_StopEvent != NULL)
    {
        CloseHandle(g_StopEvent);
        g_StopEvent = NULL;
    }
    ReportServiceStatus(SERVICE_STOPPED, ExitCode, 0);
}

int wmain(int argc, wchar_t **argv)
{
    static const SERVICE_TABLE_ENTRYW DispatchTable[] =
    {
        { (LPWSTR)AI_SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (argc > 1 && _wcsicmp(argv[1], L"--console") == 0)
        return (int)RunAiServer(TRUE);

    if (!StartServiceCtrlDispatcherW(DispatchTable))
    {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            return (int)RunAiServer(TRUE);
        return 1;
    }
    return 0;
}
