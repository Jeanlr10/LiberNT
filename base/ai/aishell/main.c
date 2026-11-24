/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Floating Win+Space DWM AI command bar, Tier 2 approval UI, desktop ALPC bridge, and folder description editor
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
#include <wingdi.h>
#include <winuser.h>
#include <winnls.h>
#include <shellapi.h>
#include <dwmapi.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ndk/lpcfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>

#include "ai_protocol.h"
#include "desktop_orch.h"
#include "libai.h"

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND          2
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW     2
#endif
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT          0x4000
#endif

#define AI_BAR_CLASS          L"LiberNtAiShellBar"
#define AI_DESC_CLASS         L"LiberNtAiDescEditor"

#define ID_HOTKEY_WIN_SPACE   1
#define ID_HOTKEY_ALT_SPACE   2
#define ID_TIMER_SEARCH       101

#define IDC_INPUT             201
#define IDC_BTN_ASK           202
#define IDC_BTN_DO            203
#define IDC_RESULTS           204
#define IDC_OUTPUT            205
#define IDC_APPROVAL_LABEL    206
#define IDC_BTN_APPROVE       207
#define IDC_BTN_DENY          208

#define IDC_DESC_EDIT         301
#define IDC_DESC_SAVE         302
#define IDC_DESC_AUTO         303
#define IDC_DESC_CLEAR        304
#define IDC_DESC_CLOSE        305
#define IDC_DESC_STATUS       306

#define WM_AI_TOGGLE_BAR      (WM_APP + 1)
#define WM_AI_AGENT_DONE      (WM_APP + 2)
#define WM_AI_SEARCH_DONE     (WM_APP + 3)

typedef struct _AI_ALPC_PACKET
{
    PORT_MESSAGE Header;
    AI_MSG_PAYLOAD Payload;
} AI_ALPC_PACKET;

C_ASSERT(sizeof(AI_ALPC_PACKET) <= 4096);

typedef struct _AI_BRIDGE_SLOT
{
    HANDLE ServerPort;
    BOOLEAN Active;
} AI_BRIDGE_SLOT;

#define AI_MAX_BRIDGE_CLIENTS 16

typedef struct _AI_AGENT_TASK
{
    DWORD Mode;
    BOOL IsApproval;
    DWORD ApprovalId;
    BOOL Approved;
    char PromptUtf8[AI_MAX_PROMPT_UTF8];
} AI_AGENT_TASK;

typedef struct _AI_SEARCH_RESULT_PACK
{
    DWORD Count;
    AI_SEARCH_HIT Hits[AI_MAX_SEARCH_RESULTS];
} AI_SEARCH_RESULT_PACK;

static HWND g_MainHwnd = NULL;
static HWND g_InputHwnd = NULL;
static HWND g_ResultsHwnd = NULL;
static HWND g_OutputHwnd = NULL;
static HWND g_ApprovalLabel = NULL;
static HWND g_ApproveBtn = NULL;
static HWND g_DenyBtn = NULL;
static WNDPROC g_OrigInputProc = NULL;
static HFONT g_UiFont = NULL;
static HFONT g_TitleFont = NULL;
static HBRUSH g_BgBrush = NULL;
static HBRUSH g_EditBrush = NULL;
static HBRUSH g_WarnBrush = NULL;
static DWORD g_PendingApprovalId = 0;
static AI_SEARCH_HIT g_LastHits[AI_MAX_SEARCH_RESULTS];
static DWORD g_LastHitCount = 0;
static volatile LONG g_BridgeStop = 0;
static HANDLE g_BridgePort = NULL;
static HANDLE g_BridgeThread = NULL;
static AI_BRIDGE_SLOT g_BridgeSlots[AI_MAX_BRIDGE_CLIENTS];

static BOOL WideToUtf8(PCWSTR Wide, char *Out, size_t Cap)
{
    if (Wide == NULL || Out == NULL || Cap == 0)
        return FALSE;
    if (WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Out, (int)Cap, NULL, NULL) <= 0)
    {
        Out[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static BOOL Utf8ToWide(const char *Utf8, PWSTR Out, DWORD Cap)
{
    if (Utf8 == NULL || Out == NULL || Cap == 0)
        return FALSE;
    if (MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, Out, (int)Cap) <= 0)
    {
        Out[0] = UNICODE_NULL;
        return FALSE;
    }
    return TRUE;
}

static void ApplyDwmStyling(HWND Hwnd)
{
    BOOL Dark = TRUE;
    DWORD CornerPref = DWMWCP_ROUND;
    DWORD Backdrop = DWMSBT_MAINWINDOW;

    DwmSetWindowAttribute(Hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &Dark, sizeof(Dark));
    DwmSetWindowAttribute(Hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &CornerPref, sizeof(CornerPref));
    DwmSetWindowAttribute(Hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &Backdrop, sizeof(Backdrop));
}

/* ==================== Session 1 Desktop ALPC Bridge ==================== */

static AI_BRIDGE_SLOT *AllocBridgeSlot(void)
{
    DWORD i;
    for (i = 0; i < AI_MAX_BRIDGE_CLIENTS; i++)
    {
        if (!g_BridgeSlots[i].Active)
        {
            g_BridgeSlots[i].Active = TRUE;
            g_BridgeSlots[i].ServerPort = NULL;
            return &g_BridgeSlots[i];
        }
    }
    return NULL;
}

static void FreeBridgeSlot(AI_BRIDGE_SLOT *Slot)
{
    if (Slot == NULL || Slot < &g_BridgeSlots[0] || Slot >= &g_BridgeSlots[AI_MAX_BRIDGE_CLIENTS])
        return;
    if (Slot->ServerPort != NULL)
    {
        NtAlpcDisconnectPort(Slot->ServerPort, 0);
        NtClose(Slot->ServerPort);
        Slot->ServerPort = NULL;
    }
    Slot->Active = FALSE;
}

static DWORD WINAPI DesktopBridgeThread(LPVOID Param)
{
    UNICODE_STRING PortName;
    OBJECT_ATTRIBUTES ObjAttrs;
    ALPC_PORT_ATTRIBUTES PortAttrs;
    SECURITY_DESCRIPTOR Sd;
    NTSTATUS Status;
    AI_ALPC_PACKET Msg;
    AI_ALPC_PACKET Reply;
    BYTE AttrBuf[sizeof(ALPC_MESSAGE_ATTRIBUTES) + sizeof(ALPC_CONTEXT_ATTR) + 64];
    PALPC_MESSAGE_ATTRIBUTES RecvAttrs = (PALPC_MESSAGE_ATTRIBUTES)AttrBuf;
    SIZE_T AttrSize = 0;
    DWORD i;

    UNREFERENCED_PARAMETER(Param);

    RtlInitUnicodeString(&PortName, AI_DESKTOP_PORT_NAME);
    if (!InitializeSecurityDescriptor(&Sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&Sd, TRUE, NULL, FALSE))
    {
        return 1;
    }

    InitializeObjectAttributes(&ObjAttrs, &PortName, OBJ_CASE_INSENSITIVE, NULL, &Sd);
    RtlZeroMemory(&PortAttrs, sizeof(PortAttrs));
    PortAttrs.Flags = ALPC_PORFLG_ALLOW_LPC_REQUESTS;
    PortAttrs.SecurityQos.Length = sizeof(PortAttrs.SecurityQos);
    PortAttrs.SecurityQos.ImpersonationLevel = SecurityImpersonation;
    PortAttrs.SecurityQos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    PortAttrs.SecurityQos.EffectiveOnly = TRUE;
    PortAttrs.MaxMessageLength = sizeof(AI_ALPC_PACKET);

    Status = NtAlpcCreatePort(&g_BridgePort, &ObjAttrs, &PortAttrs);
    if (!NT_SUCCESS(Status))
    {
        CHAR Dbg[128];
        _snprintf(Dbg, sizeof(Dbg), "aishell: NtAlpcCreatePort(DesktopPort) failed: 0x%08lx\n",
                  (unsigned long)Status);
        OutputDebugStringA(Dbg);
        return 1;
    }

    OutputDebugStringA("aishell: desktop ALPC bridge ready on \\RPC Control\\LiberNtAiDesktopPort\n");

    while (!InterlockedCompareExchange(&g_BridgeStop, 0, 0))
    {
        SIZE_T BufLen = sizeof(Msg);
        LARGE_INTEGER Timeout;
        UCHAR MsgType;
        PALPC_CONTEXT_ATTR CtxAttr = NULL;
        AI_BRIDGE_SLOT *Slot = NULL;

        RtlZeroMemory(&Msg, sizeof(Msg));
        RtlZeroMemory(AttrBuf, sizeof(AttrBuf));
        AlpcInitializeMessageAttribute(ALPC_MESSAGE_CONTEXT_ATTRIBUTE, RecvAttrs, sizeof(AttrBuf), &AttrSize);

        Timeout.QuadPart = -(LONGLONG)500 * 10000;
        Status = NtAlpcSendWaitReceivePort(
            g_BridgePort, 0, NULL, NULL, &Msg.Header, &BufLen, RecvAttrs, &Timeout);
        if (Status == STATUS_TIMEOUT)
            continue;
        if (!NT_SUCCESS(Status))
            break;

        MsgType = (UCHAR)(Msg.Header.u2.s2.Type & 0xFF);
        if (RecvAttrs->ValidAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE)
            CtxAttr = AlpcGetMessageAttribute(RecvAttrs, ALPC_MESSAGE_CONTEXT_ATTRIBUTE);
        if (CtxAttr != NULL)
            Slot = (AI_BRIDGE_SLOT *)CtxAttr->PortContext;

        if (MsgType == LPC_CONNECTION_REQUEST)
        {
            AI_BRIDGE_SLOT *NewSlot = AllocBridgeSlot();
            BOOLEAN Accept = (NewSlot != NULL && Msg.Payload.Version == AI_PROTOCOL_VERSION);
            HANDLE ServerPort = NULL;

            Status = NtAlpcAcceptConnectPort(
                &ServerPort, g_BridgePort, 0, NULL, NULL,
                Accept ? NewSlot : NULL, &Msg.Header, NULL, Accept);
            if (Accept && NT_SUCCESS(Status))
                NewSlot->ServerPort = ServerPort;
            else if (NewSlot != NULL)
                FreeBridgeSlot(NewSlot);
        }
        else if (MsgType == LPC_REQUEST)
        {
            RtlZeroMemory(&Reply, sizeof(Reply));
            Reply.Header = Msg.Header;
            Reply.Header.u1.s1.DataLength = sizeof(Reply.Payload);
            Reply.Header.u1.s1.TotalLength = sizeof(Reply);
            Reply.Payload.Version = AI_PROTOCOL_VERSION;
            Reply.Payload.Opcode = Msg.Payload.Opcode;

            if (Msg.Payload.Opcode == AiOpDesktopTool)
            {
                int Rc;
                Msg.Payload.Data.DesktopTool.ToolName[sizeof(Msg.Payload.Data.DesktopTool.ToolName) - 1] = '\0';
                Msg.Payload.Data.DesktopTool.ArgsJson[sizeof(Msg.Payload.Data.DesktopTool.ArgsJson) - 1] = '\0';
                Rc = AiDesktopExecuteTool(
                    Msg.Payload.Data.DesktopTool.ToolName,
                    Msg.Payload.Data.DesktopTool.ArgsJson,
                    g_MainHwnd,
                    Reply.Payload.Data.DesktopTool.ResultJson,
                    sizeof(Reply.Payload.Data.DesktopTool.ResultJson));
                Reply.Payload.Status = (Rc == 0) ? AiStatusOk : AiStatusInternal;
            }
            else if (Msg.Payload.Opcode == AiOpPing)
            {
                Reply.Payload.Status = AiStatusOk;
            }
            else
            {
                Reply.Payload.Status = AiStatusInvalidArg;
            }

            if (Slot != NULL && Slot->Active && Slot->ServerPort != NULL)
            {
                NtAlpcSendWaitReceivePort(
                    Slot->ServerPort, ALPC_MSGFLG_REPLY_MESSAGE,
                    &Reply.Header, NULL, NULL, NULL, NULL, NULL);
            }
        }
        else if (MsgType == LPC_PORT_CLOSED || MsgType == LPC_CLIENT_DIED)
        {
            if (Slot != NULL)
                FreeBridgeSlot(Slot);
        }
    }

    for (i = 0; i < AI_MAX_BRIDGE_CLIENTS; i++)
    {
        if (g_BridgeSlots[i].Active)
            FreeBridgeSlot(&g_BridgeSlots[i]);
    }
    if (g_BridgePort != NULL)
    {
        NtClose(g_BridgePort);
        g_BridgePort = NULL;
    }
    return 0;
}

/* ==================== Background Search & Agent Workers ==================== */

static DWORD WINAPI SearchWorkerThread(LPVOID Param)
{
    char *Query = (char *)Param;
    HANDLE Port = NULL;
    AI_SEARCH_RESULT_PACK *Pack;

    Pack = (AI_SEARCH_RESULT_PACK *)calloc(1, sizeof(*Pack));
    if (Pack != NULL && Query != NULL && Query[0] != '\0')
    {
        if (AiClientConnect(&Port) == AiStatusOk)
        {
            AiClientSearch(Port, Query, AI_MAX_SEARCH_RESULTS, Pack->Hits, &Pack->Count);
            AiClientClose(Port);
        }
    }
    free(Query);
    if (g_MainHwnd != NULL)
        PostMessageW(g_MainHwnd, WM_AI_SEARCH_DONE, 0, (LPARAM)Pack);
    else
        free(Pack);
    return 0;
}

static DWORD WINAPI AgentWorkerThread(LPVOID Param)
{
    AI_AGENT_TASK *Task = (AI_AGENT_TASK *)Param;
    AI_PROMPT_RESPONSE *Resp;
    HANDLE Port = NULL;
    DWORD Res;

    Resp = (AI_PROMPT_RESPONSE *)calloc(1, sizeof(*Resp));
    if (Resp != NULL && Task != NULL)
    {
        Res = AiClientConnect(&Port);
        if (Res != AiStatusOk)
        {
            Resp->State = AiPromptError;
            _snprintf(Resp->ReplyText, sizeof(Resp->ReplyText), "Cannot connect to aisvc ALPC broker.");
        }
        else
        {
            if (Task->IsApproval)
                Res = AiClientApproveAction(Port, Task->ApprovalId, Task->Approved, Resp);
            else
                Res = AiClientSubmitPrompt(Port, Task->PromptUtf8, Task->Mode, Resp);
            AiClientClose(Port);
            if (Res != AiStatusOk && Resp->ReplyText[0] == '\0')
            {
                Resp->State = AiPromptError;
                _snprintf(Resp->ReplyText, sizeof(Resp->ReplyText), "Agent call failed (err=%lu)",
                          (unsigned long)Res);
            }
        }
    }
    free(Task);
    if (g_MainHwnd != NULL)
        PostMessageW(g_MainHwnd, WM_AI_AGENT_DONE, 0, (LPARAM)Resp);
    else
        free(Resp);
    return 0;
}

static void SetApprovalBannerVisible(BOOL Visible, const AI_PROMPT_RESPONSE *Resp)
{
    if (Visible && Resp != NULL)
    {
        char MsgUtf8[512];
        WCHAR MsgW[512];

        g_PendingApprovalId = Resp->ApprovalId;
        _snprintf(MsgUtf8, sizeof(MsgUtf8),
                  "Tier %u Approval Required: %s  ->  %s",
                  Resp->ToolTier, Resp->ToolName,
                  Resp->ToolTarget[0] ? Resp->ToolTarget : "(none)");
        MsgUtf8[sizeof(MsgUtf8) - 1] = '\0';
        Utf8ToWide(MsgUtf8, MsgW, ARRAYSIZE(MsgW));
        SetWindowTextW(g_ApprovalLabel, MsgW);
        ShowWindow(g_ApprovalLabel, SW_SHOW);
        ShowWindow(g_ApproveBtn, SW_SHOW);
        ShowWindow(g_DenyBtn, SW_SHOW);
    }
    else
    {
        g_PendingApprovalId = 0;
        ShowWindow(g_ApprovalLabel, SW_HIDE);
        ShowWindow(g_ApproveBtn, SW_HIDE);
        ShowWindow(g_DenyBtn, SW_HIDE);
    }
}

static void SubmitBarPrompt(DWORD Mode)
{
    WCHAR TextW[512];
    AI_AGENT_TASK *Task;
    HANDLE Thr;

    if (GetWindowTextW(g_InputHwnd, TextW, ARRAYSIZE(TextW)) <= 0)
        return;

    Task = (AI_AGENT_TASK *)calloc(1, sizeof(*Task));
    if (Task == NULL)
        return;
    Task->Mode = Mode;
    WideToUtf8(TextW, Task->PromptUtf8, sizeof(Task->PromptUtf8));

    SetApprovalBannerVisible(FALSE, NULL);
    SetWindowTextW(g_OutputHwnd, L"Thinking...");

    Thr = CreateThread(NULL, 0, AgentWorkerThread, Task, 0, NULL);
    if (Thr != NULL)
        CloseHandle(Thr);
    else
        free(Task);
}

static void SubmitBarApproval(BOOL Approved)
{
    AI_AGENT_TASK *Task;
    HANDLE Thr;

    if (g_PendingApprovalId == 0)
        return;

    Task = (AI_AGENT_TASK *)calloc(1, sizeof(*Task));
    if (Task == NULL)
        return;
    Task->IsApproval = TRUE;
    Task->ApprovalId = g_PendingApprovalId;
    Task->Approved = Approved;

    SetApprovalBannerVisible(FALSE, NULL);
    SetWindowTextW(g_OutputHwnd, Approved ? L"Executing approved action..." : L"Cancelling action...");

    Thr = CreateThread(NULL, 0, AgentWorkerThread, Task, 0, NULL);
    if (Thr != NULL)
        CloseHandle(Thr);
    else
        free(Task);
}

static void ToggleCommandBar(HWND Hwnd, BOOL ForceShow)
{
    if (!ForceShow && IsWindowVisible(Hwnd))
    {
        ShowWindow(Hwnd, SW_HIDE);
        return;
    }

    {
        int ScrW = GetSystemMetrics(SM_CXSCREEN);
        int BarW = 740;
        int BarH = 440;
        SetWindowPos(Hwnd, HWND_TOPMOST, (ScrW - BarW) / 2, 72, BarW, BarH, SWP_SHOWWINDOW);
        SetForegroundWindow(Hwnd);
        SetFocus(g_InputHwnd);
        SendMessageW(g_InputHwnd, EM_SETSEL, 0, -1);
    }
}

static LRESULT CALLBACK InputSubclassProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    if (Msg == WM_KEYDOWN)
    {
        if (WParam == VK_RETURN)
        {
            SubmitBarPrompt(AI_PROMPT_MODE_DO);
            return 0;
        }
        if (WParam == VK_ESCAPE)
        {
            ShowWindow(g_MainHwnd, SW_HIDE);
            return 0;
        }
    }
    return CallWindowProcW(g_OrigInputProc, Hwnd, Msg, WParam, LParam);
}

static LRESULT CALLBACK MainBarWndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    switch (Msg)
    {
        case WM_CREATE:
        {
            ApplyDwmStyling(Hwnd);

            g_TitleFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_UiFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_BgBrush = CreateSolidBrush(RGB(24, 26, 32));
            g_EditBrush = CreateSolidBrush(RGB(34, 38, 46));
            g_WarnBrush = CreateSolidBrush(RGB(72, 50, 18));

            g_InputHwnd = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                16, 16, 540, 32, Hwnd, (HMENU)IDC_INPUT, NULL, NULL);
            SendMessageW(g_InputHwnd, WM_SETFONT, (WPARAM)g_TitleFont, TRUE);
            g_OrigInputProc = (WNDPROC)SetWindowLongPtrW(g_InputHwnd, GWLP_WNDPROC, (LONG_PTR)InputSubclassProc);

            {
                HWND BtnAsk = CreateWindowExW(
                    0, L"BUTTON", L"Ask",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    568, 16, 72, 32, Hwnd, (HMENU)IDC_BTN_ASK, NULL, NULL);
                HWND BtnDo = CreateWindowExW(
                    0, L"BUTTON", L"Do",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    650, 16, 72, 32, Hwnd, (HMENU)IDC_BTN_DO, NULL, NULL);
                SendMessageW(BtnAsk, WM_SETFONT, (WPARAM)g_UiFont, TRUE);
                SendMessageW(BtnDo, WM_SETFONT, (WPARAM)g_UiFont, TRUE);
            }

            g_ResultsHwnd = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                16, 60, 706, 138, Hwnd, (HMENU)IDC_RESULTS, NULL, NULL);
            SendMessageW(g_ResultsHwnd, WM_SETFONT, (WPARAM)g_UiFont, TRUE);

            g_ApprovalLabel = CreateWindowExW(
                0, L"STATIC", L"",
                WS_CHILD | SS_LEFT | SS_NOPREFIX,
                16, 208, 520, 28, Hwnd, (HMENU)IDC_APPROVAL_LABEL, NULL, NULL);
            SendMessageW(g_ApprovalLabel, WM_SETFONT, (WPARAM)g_UiFont, TRUE);

            g_ApproveBtn = CreateWindowExW(
                0, L"BUTTON", L"Approve",
                WS_CHILD | BS_PUSHBUTTON,
                546, 206, 84, 28, Hwnd, (HMENU)IDC_BTN_APPROVE, NULL, NULL);
            g_DenyBtn = CreateWindowExW(
                0, L"BUTTON", L"Deny",
                WS_CHILD | BS_PUSHBUTTON,
                638, 206, 84, 28, Hwnd, (HMENU)IDC_BTN_DENY, NULL, NULL);
            SendMessageW(g_ApproveBtn, WM_SETFONT, (WPARAM)g_UiFont, TRUE);
            SendMessageW(g_DenyBtn, WM_SETFONT, (WPARAM)g_UiFont, TRUE);

            g_OutputHwnd = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"LiberNT AI ready. Type to search files & folder descriptions, or press Enter for agent mode.",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                16, 244, 706, 178, Hwnd, (HMENU)IDC_OUTPUT, NULL, NULL);
            SendMessageW(g_OutputHwnd, WM_SETFONT, (WPARAM)g_UiFont, TRUE);

            RegisterHotKey(Hwnd, ID_HOTKEY_WIN_SPACE, MOD_WIN | MOD_NOREPEAT, VK_SPACE);
            RegisterHotKey(Hwnd, ID_HOTKEY_ALT_SPACE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
            return 0;
        }

        case WM_HOTKEY:
            if (WParam == ID_HOTKEY_WIN_SPACE || WParam == ID_HOTKEY_ALT_SPACE)
                ToggleCommandBar(Hwnd, FALSE);
            return 0;

        case WM_AI_TOGGLE_BAR:
            ToggleCommandBar(Hwnd, WParam != 0);
            return 0;

        case WM_COMMAND:
        {
            WORD Id = LOWORD(WParam);
            WORD Code = HIWORD(WParam);

            if (Id == IDC_INPUT && Code == EN_CHANGE)
            {
                SetTimer(Hwnd, ID_TIMER_SEARCH, 120, NULL);
            }
            else if (Id == IDC_BTN_ASK)
            {
                SubmitBarPrompt(AI_PROMPT_MODE_ASK);
            }
            else if (Id == IDC_BTN_DO)
            {
                SubmitBarPrompt(AI_PROMPT_MODE_DO);
            }
            else if (Id == IDC_BTN_APPROVE)
            {
                SubmitBarApproval(TRUE);
            }
            else if (Id == IDC_BTN_DENY)
            {
                SubmitBarApproval(FALSE);
            }
            else if (Id == IDC_RESULTS && Code == LBN_DBLCLK)
            {
                LRESULT Sel = SendMessageW(g_ResultsHwnd, LB_GETCURSEL, 0, 0);
                if (Sel >= 0 && (DWORD)Sel < g_LastHitCount)
                {
                    WCHAR PathW[MAX_PATH];
                    if (Utf8ToWide(g_LastHits[Sel].Path, PathW, ARRAYSIZE(PathW)))
                        ShellExecuteW(Hwnd, L"open", PathW, NULL, NULL, SW_SHOWNORMAL);
                }
            }
            return 0;
        }

        case WM_TIMER:
            if (WParam == ID_TIMER_SEARCH)
            {
                WCHAR QueryW[256];
                KillTimer(Hwnd, ID_TIMER_SEARCH);
                if (GetWindowTextW(g_InputHwnd, QueryW, ARRAYSIZE(QueryW)) > 1)
                {
                    char *QueryUtf8 = (char *)calloc(1, 256);
                    if (QueryUtf8 != NULL)
                    {
                        HANDLE Thr;
                        WideToUtf8(QueryW, QueryUtf8, 256);
                        Thr = CreateThread(NULL, 0, SearchWorkerThread, QueryUtf8, 0, NULL);
                        if (Thr != NULL)
                            CloseHandle(Thr);
                        else
                            free(QueryUtf8);
                    }
                }
                else
                {
                    SendMessageW(g_ResultsHwnd, LB_RESETCONTENT, 0, 0);
                    g_LastHitCount = 0;
                }
            }
            return 0;

        case WM_AI_SEARCH_DONE:
        {
            AI_SEARCH_RESULT_PACK *Pack = (AI_SEARCH_RESULT_PACK *)LParam;
            DWORD i;

            SendMessageW(g_ResultsHwnd, LB_RESETCONTENT, 0, 0);
            g_LastHitCount = 0;
            if (Pack != NULL)
            {
                g_LastHitCount = Pack->Count;
                for (i = 0; i < Pack->Count && i < AI_MAX_SEARCH_RESULTS; i++)
                {
                    char LineUtf8[512];
                    WCHAR LineW[512];

                    g_LastHits[i] = Pack->Hits[i];
                    if (Pack->Hits[i].Description[0] != '\0')
                    {
                        _snprintf(LineUtf8, sizeof(LineUtf8), "[%s] %s  --  %s",
                                  Pack->Hits[i].IsDir ? "DIR" : "FILE",
                                  Pack->Hits[i].Path,
                                  Pack->Hits[i].Description);
                    }
                    else
                    {
                        _snprintf(LineUtf8, sizeof(LineUtf8), "[%s] %s",
                                  Pack->Hits[i].IsDir ? "DIR" : "FILE",
                                  Pack->Hits[i].Path);
                    }
                    LineUtf8[sizeof(LineUtf8) - 1] = '\0';
                    Utf8ToWide(LineUtf8, LineW, ARRAYSIZE(LineW));
                    SendMessageW(g_ResultsHwnd, LB_ADDSTRING, 0, (LPARAM)LineW);
                }
                free(Pack);
            }
            return 0;
        }

        case WM_AI_AGENT_DONE:
        {
            AI_PROMPT_RESPONSE *Resp = (AI_PROMPT_RESPONSE *)LParam;
            if (Resp != NULL)
            {
                WCHAR ReplyW[AI_MAX_REPLY_UTF8];
                if (Resp->State == AiPromptPendingApproval)
                {
                    SetApprovalBannerVisible(TRUE, Resp);
                }
                else
                {
                    SetApprovalBannerVisible(FALSE, NULL);
                }
                Utf8ToWide(Resp->ReplyText, ReplyW, ARRAYSIZE(ReplyW));
                SetWindowTextW(g_OutputHwnd, ReplyW);
                free(Resp);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)WParam, OPAQUE);
            if ((HWND)LParam == g_ApprovalLabel)
            {
                SetTextColor((HDC)WParam, RGB(255, 224, 130));
                SetBkColor((HDC)WParam, RGB(72, 50, 18));
                return (LRESULT)g_WarnBrush;
            }
            SetTextColor((HDC)WParam, RGB(224, 228, 236));
            SetBkColor((HDC)WParam, RGB(34, 38, 46));
            return (LRESULT)g_EditBrush;

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)WParam, RGB(236, 240, 248));
            SetBkColor((HDC)WParam, RGB(34, 38, 46));
            return (LRESULT)g_EditBrush;

        case WM_ERASEBKGND:
        {
            RECT Rc;
            GetClientRect(Hwnd, &Rc);
            FillRect((HDC)WParam, &Rc, g_BgBrush);
            return 1;
        }

        case WM_DESTROY:
            UnregisterHotKey(Hwnd, ID_HOTKEY_WIN_SPACE);
            UnregisterHotKey(Hwnd, ID_HOTKEY_ALT_SPACE);
            if (g_BgBrush)
                DeleteObject(g_BgBrush);
            if (g_EditBrush)
                DeleteObject(g_EditBrush);
            if (g_WarnBrush)
                DeleteObject(g_WarnBrush);
            if (g_UiFont)
                DeleteObject(g_UiFont);
            if (g_TitleFont)
                DeleteObject(g_TitleFont);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Hwnd, Msg, WParam, LParam);
}

/* ==================== Folder Description Editor Dialog ==================== */

typedef struct _DESC_EDITOR_STATE
{
    WCHAR FolderPath[MAX_PATH];
    HWND EditHwnd;
    HWND StatusHwnd;
} DESC_EDITOR_STATE;

static LRESULT CALLBACK DescEditorWndProc(HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
    DESC_EDITOR_STATE *St = (DESC_EDITOR_STATE *)GetWindowLongPtrW(Hwnd, GWLP_USERDATA);

    switch (Msg)
    {
        case WM_CREATE:
        {
            CREATESTRUCTW *Cs = (CREATESTRUCTW *)LParam;
            char DescUtf8[AI_MAX_DESC_UTF8];
            char SrcUtf8[AI_MAX_SOURCE_UTF8];
            WCHAR DescW[AI_MAX_DESC_UTF8];
            WCHAR HeaderW[MAX_PATH + 32];

            St = (DESC_EDITOR_STATE *)Cs->lpCreateParams;
            SetWindowLongPtrW(Hwnd, GWLP_USERDATA, (LONG_PTR)St);
            ApplyDwmStyling(Hwnd);

            _snwprintf(HeaderW, ARRAYSIZE(HeaderW), L"Folder: %s", St->FolderPath);
            HeaderW[ARRAYSIZE(HeaderW) - 1] = UNICODE_NULL;

            CreateWindowExW(0, L"STATIC", HeaderW, WS_CHILD | WS_VISIBLE | SS_LEFT,
                            14, 12, 472, 22, Hwnd, NULL, NULL, NULL);

            St->EditHwnd = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                14, 40, 472, 84, Hwnd, (HMENU)IDC_DESC_EDIT, NULL, NULL);

            St->StatusHwnd = CreateWindowExW(
                0, L"STATIC", L"Stored in NTFS Alternate Data Stream (:LiberNT.Description)",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                14, 132, 472, 20, Hwnd, (HMENU)IDC_DESC_STATUS, NULL, NULL);

            CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                            14, 160, 86, 28, Hwnd, (HMENU)IDC_DESC_SAVE, NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"AI Summarize", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            108, 160, 118, 28, Hwnd, (HMENU)IDC_DESC_AUTO, NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            234, 160, 80, 28, Hwnd, (HMENU)IDC_DESC_CLEAR, NULL, NULL);
            CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            406, 160, 80, 28, Hwnd, (HMENU)IDC_DESC_CLOSE, NULL, NULL);

            if (AiAdsReadFolderDescription(St->FolderPath, DescUtf8, sizeof(DescUtf8),
                                           SrcUtf8, sizeof(SrcUtf8), NULL, 0) == AiStatusOk)
            {
                Utf8ToWide(DescUtf8, DescW, ARRAYSIZE(DescW));
                SetWindowTextW(St->EditHwnd, DescW);
            }
            return 0;
        }

        case WM_COMMAND:
            if (St == NULL)
                break;
            switch (LOWORD(WParam))
            {
                case IDC_DESC_SAVE:
                {
                    WCHAR DescW[AI_MAX_DESC_UTF8];
                    char DescUtf8[AI_MAX_DESC_UTF8];
                    HANDLE Port = NULL;

                    GetWindowTextW(St->EditHwnd, DescW, ARRAYSIZE(DescW));
                    WideToUtf8(DescW, DescUtf8, sizeof(DescUtf8));
                    AiClientConnect(&Port);
                    if (AiClientSetFolderDescription(Port, St->FolderPath, DescUtf8, "user", "") == AiStatusOk)
                        SetWindowTextW(St->StatusHwnd, L"Saved [user] description to NTFS ADS and index.");
                    else
                        SetWindowTextW(St->StatusHwnd, L"Failed to save description.");
                    AiClientClose(Port);
                    return 0;
                }
                case IDC_DESC_AUTO:
                {
                    HANDLE Port = NULL;
                    char DescUtf8[AI_MAX_DESC_UTF8];
                    char TagsUtf8[AI_MAX_TAGS_UTF8];
                    char SrcUtf8[AI_MAX_SOURCE_UTF8];
                    WCHAR DescW[AI_MAX_DESC_UTF8];

                    if (AiClientConnect(&Port) == AiStatusOk)
                    {
                        DWORD Res = AiClientAutoDescribeFolder(
                            Port, St->FolderPath, TRUE,
                            DescUtf8, sizeof(DescUtf8),
                            TagsUtf8, sizeof(TagsUtf8),
                            SrcUtf8, sizeof(SrcUtf8));
                        AiClientClose(Port);
                        if (Res == AiStatusOk)
                        {
                            Utf8ToWide(DescUtf8, DescW, ARRAYSIZE(DescW));
                            SetWindowTextW(St->EditHwnd, DescW);
                            SetWindowTextW(St->StatusHwnd, L"Generated [ai] description and updated index.");
                        }
                        else
                        {
                            SetWindowTextW(St->StatusHwnd, L"AI summarize failed (check provider config).");
                        }
                    }
                    return 0;
                }
                case IDC_DESC_CLEAR:
                {
                    HANDLE Port = NULL;
                    AiClientConnect(&Port);
                    AiClientClearFolderDescription(Port, St->FolderPath);
                    AiClientClose(Port);
                    SetWindowTextW(St->EditHwnd, L"");
                    SetWindowTextW(St->StatusHwnd, L"Cleared NTFS folder description.");
                    return 0;
                }
                case IDC_DESC_CLOSE:
                    DestroyWindow(Hwnd);
                    return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Hwnd, Msg, WParam, LParam);
}

static int RunDescribeEditor(HINSTANCE hInst, PCWSTR FolderPath)
{
    WNDCLASSEXW Wc;
    DESC_EDITOR_STATE State;
    HWND Hwnd;
    MSG Msg;

    memset(&State, 0, sizeof(State));
    GetFullPathNameW(FolderPath, ARRAYSIZE(State.FolderPath), State.FolderPath, NULL);

    memset(&Wc, 0, sizeof(Wc));
    Wc.cbSize = sizeof(Wc);
    Wc.lpfnWndProc = DescEditorWndProc;
    Wc.hInstance = hInst;
    Wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    Wc.lpszClassName = AI_DESC_CLASS;
    RegisterClassExW(&Wc);

    Hwnd = CreateWindowExW(
        WS_EX_TOPMOST, AI_DESC_CLASS, L"LiberNT Folder Description",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 512, 236,
        NULL, NULL, hInst, &State);
    if (Hwnd == NULL)
        return 1;

    ShowWindow(Hwnd, SW_SHOW);
    UpdateWindow(Hwnd);
    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR CmdLine, int ShowCmd)
{
    int Argc = 0;
    PWSTR *Argv;
    BOOL ShowImmediately = FALSE;
    HANDLE SingleMutex = NULL;
    WNDCLASSEXW Wc;
    MSG Msg;
    int i;

    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(CmdLine);
    UNREFERENCED_PARAMETER(ShowCmd);

    Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    if (Argv != NULL)
    {
        for (i = 1; i < Argc; i++)
        {
            if (_wcsicmp(Argv[i], L"--describe") == 0 && i + 1 < Argc)
            {
                int Rc = RunDescribeEditor(hInst, Argv[i + 1]);
                LocalFree(Argv);
                return Rc;
            }
            if (_wcsicmp(Argv[i], L"--show") == 0)
                ShowImmediately = TRUE;
        }
        LocalFree(Argv);
    }

    SingleMutex = CreateMutexW(NULL, FALSE, L"Local\\LiberNtAiShellInstance");
    if (SingleMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND Existing = FindWindowW(AI_BAR_CLASS, NULL);
        if (Existing != NULL)
            PostMessageW(Existing, WM_AI_TOGGLE_BAR, ShowImmediately ? 1 : 0, 0);
        CloseHandle(SingleMutex);
        return 0;
    }

    memset(&Wc, 0, sizeof(Wc));
    Wc.cbSize = sizeof(Wc);
    Wc.lpfnWndProc = MainBarWndProc;
    Wc.hInstance = hInst;
    Wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Wc.lpszClassName = AI_BAR_CLASS;
    if (!RegisterClassExW(&Wc))
    {
        if (SingleMutex)
            CloseHandle(SingleMutex);
        return 1;
    }

    g_MainHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        AI_BAR_CLASS,
        L"LiberNT AI",
        WS_POPUP | WS_BORDER,
        240, 72, 740, 440,
        NULL, NULL, hInst, NULL);
    if (g_MainHwnd == NULL)
    {
        if (SingleMutex)
            CloseHandle(SingleMutex);
        return 1;
    }

    g_BridgeThread = CreateThread(NULL, 0, DesktopBridgeThread, NULL, 0, NULL);

    if (ShowImmediately)
        ToggleCommandBar(g_MainHwnd, TRUE);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    InterlockedExchange(&g_BridgeStop, 1);
    if (g_BridgeThread != NULL)
    {
        WaitForSingleObject(g_BridgeThread, 3000);
        CloseHandle(g_BridgeThread);
    }
    if (SingleMutex)
        CloseHandle(SingleMutex);
    return 0;
}
