/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Interactive desktop window orchestrator, layout undo stack, and control inspector
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winnls.h>
#include <shellapi.h>

#include "ai_json.h"
#include "desktop_orch.h"

#define AI_MAX_ENUM_WINDOWS  32
#define AI_MAX_UNDO_WINDOWS  16
#define AI_MAX_CONTROLS      24

typedef struct _AI_WIN_INFO
{
    HWND Hwnd;
    DWORD Pid;
    RECT Rect;
    BOOL Minimized;
    BOOL Maximized;
    BOOL Active;
    char Title[160];
    char ClassName[80];
} AI_WIN_INFO;

typedef struct _AI_ENUM_CTX
{
    HWND ExcludeHwnd;
    HWND Foreground;
    AI_WIN_INFO Items[AI_MAX_ENUM_WINDOWS];
    int Count;
} AI_ENUM_CTX;

typedef struct _AI_UNDO_ENTRY
{
    HWND Hwnd;
    WINDOWPLACEMENT Placement;
} AI_UNDO_ENTRY;

static AI_UNDO_ENTRY g_UndoStack[AI_MAX_UNDO_WINDOWS];
static int g_UndoCount = 0;

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
    return MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, Out, (int)Cap) > 0;
}

static BOOL ContainsCaseInsensitive(const char *Haystack, const char *Needle)
{
    size_t NLen;
    size_t i;

    if (Haystack == NULL || Needle == NULL || Needle[0] == '\0')
        return FALSE;
    NLen = strlen(Needle);
    for (; *Haystack != '\0'; Haystack++)
    {
        for (i = 0; i < NLen; i++)
        {
            if (Haystack[i] == '\0')
                return FALSE;
            if (tolower((unsigned char)Haystack[i]) != tolower((unsigned char)Needle[i]))
                break;
        }
        if (i == NLen)
            return TRUE;
    }
    return FALSE;
}

static BOOL IsManagedDesktopWindow(HWND Hwnd, HWND ExcludeHwnd)
{
    LONG_PTR Style;
    LONG_PTR ExStyle;
    WCHAR Title[128];
    WCHAR Cls[64];

    if (Hwnd == NULL || Hwnd == ExcludeHwnd || !IsWindowVisible(Hwnd))
        return FALSE;
    if (GetWindow(Hwnd, GW_OWNER) != NULL)
        return FALSE;

    Style = GetWindowLongPtrW(Hwnd, GWL_STYLE);
    ExStyle = GetWindowLongPtrW(Hwnd, GWL_EXSTYLE);
    if (Style & WS_CHILD)
        return FALSE;
    if ((ExStyle & WS_EX_TOOLWINDOW) && !(ExStyle & WS_EX_APPWINDOW))
        return FALSE;

    if (GetClassNameW(Hwnd, Cls, ARRAYSIZE(Cls)) > 0)
    {
        if (_wcsicmp(Cls, L"Progman") == 0 ||
            _wcsicmp(Cls, L"WorkerW") == 0 ||
            _wcsicmp(Cls, L"Shell_TrayWnd") == 0 ||
            _wcsicmp(Cls, L"LiberNtAiShellBar") == 0)
        {
            return FALSE;
        }
    }

    if (GetWindowTextW(Hwnd, Title, ARRAYSIZE(Title)) <= 0)
        return FALSE;

    return TRUE;
}

static BOOL CALLBACK EnumDesktopWinProc(HWND Hwnd, LPARAM Param)
{
    AI_ENUM_CTX *Ctx = (AI_ENUM_CTX *)Param;
    AI_WIN_INFO *Info;
    WCHAR TitleW[160];
    WCHAR ClsW[80];

    if (Ctx->Count >= AI_MAX_ENUM_WINDOWS)
        return FALSE;
    if (!IsManagedDesktopWindow(Hwnd, Ctx->ExcludeHwnd))
        return TRUE;

    Info = &Ctx->Items[Ctx->Count];
    memset(Info, 0, sizeof(*Info));
    Info->Hwnd = Hwnd;
    GetWindowThreadProcessId(Hwnd, &Info->Pid);
    GetWindowRect(Hwnd, &Info->Rect);
    Info->Minimized = IsIconic(Hwnd);
    Info->Maximized = IsZoomed(Hwnd);
    Info->Active = (Hwnd == Ctx->Foreground);

    TitleW[0] = UNICODE_NULL;
    ClsW[0] = UNICODE_NULL;
    GetWindowTextW(Hwnd, TitleW, ARRAYSIZE(TitleW));
    GetClassNameW(Hwnd, ClsW, ARRAYSIZE(ClsW));
    WideToUtf8(TitleW, Info->Title, sizeof(Info->Title));
    WideToUtf8(ClsW, Info->ClassName, sizeof(Info->ClassName));

    Ctx->Count++;
    return TRUE;
}

static void CollectDesktopWindows(HWND ExcludeHwnd, AI_ENUM_CTX *Ctx)
{
    memset(Ctx, 0, sizeof(*Ctx));
    Ctx->ExcludeHwnd = ExcludeHwnd;
    Ctx->Foreground = GetForegroundWindow();
    EnumWindows(EnumDesktopWinProc, (LPARAM)Ctx);
}

static HWND FindWindowByQuery(const AI_ENUM_CTX *Ctx, const char *Query)
{
    int i;

    if (Query == NULL || Query[0] == '\0')
        return NULL;

    if ((Query[0] == '0' && (Query[1] == 'x' || Query[1] == 'X')))
    {
        unsigned long long Val = strtoull(Query, NULL, 16);
        HWND Target = (HWND)(ULONG_PTR)Val;
        if (IsWindow(Target))
            return Target;
    }

    for (i = 0; i < Ctx->Count; i++)
    {
        if (ContainsCaseInsensitive(Ctx->Items[i].Title, Query) ||
            ContainsCaseInsensitive(Ctx->Items[i].ClassName, Query))
        {
            return Ctx->Items[i].Hwnd;
        }
    }

    if (_stricmp(Query, "cmd") == 0 || _stricmp(Query, "command prompt") == 0 || _stricmp(Query, "terminal") == 0)
    {
        for (i = 0; i < Ctx->Count; i++)
        {
            if (ContainsCaseInsensitive(Ctx->Items[i].Title, "cmd.exe") ||
                ContainsCaseInsensitive(Ctx->Items[i].ClassName, "Console"))
            {
                return Ctx->Items[i].Hwnd;
            }
        }
    }
    if (_stricmp(Query, "explorer") == 0 || _stricmp(Query, "file explorer") == 0)
    {
        for (i = 0; i < Ctx->Count; i++)
        {
            if (ContainsCaseInsensitive(Ctx->Items[i].ClassName, "CabinetWClass") ||
                ContainsCaseInsensitive(Ctx->Items[i].ClassName, "ExploreWClass"))
            {
                return Ctx->Items[i].Hwnd;
            }
        }
    }
    return NULL;
}

static void SaveUndoPlacements(HWND *Hwnds, int Count)
{
    int i;

    g_UndoCount = 0;
    for (i = 0; i < Count && g_UndoCount < AI_MAX_UNDO_WINDOWS; i++)
    {
        WINDOWPLACEMENT Wp;
        memset(&Wp, 0, sizeof(Wp));
        Wp.length = sizeof(Wp);
        if (GetWindowPlacement(Hwnds[i], &Wp))
        {
            g_UndoStack[g_UndoCount].Hwnd = Hwnds[i];
            g_UndoStack[g_UndoCount].Placement = Wp;
            g_UndoCount++;
        }
    }
}

static int ToolListWindows(HWND ExcludeHwnd, char *OutJson, size_t OutCap)
{
    AI_ENUM_CTX Ctx;
    AI_JSON_BUF Buf;
    int i;

    CollectDesktopWindows(ExcludeHwnd, &Ctx);

    AiJsonBufInit(&Buf, 512);
    AiJsonBufAppendStr(&Buf, "{\"count\":");
    AiJsonBufAppendInt64(&Buf, Ctx.Count);
    AiJsonBufAppendStr(&Buf, ",\"windows\":[");
    for (i = 0; i < Ctx.Count; i++)
    {
        char HwndStr[32];
        const AI_WIN_INFO *W = &Ctx.Items[i];

        if (i > 0)
            AiJsonBufAppendChar(&Buf, ',');
        _snprintf(HwndStr, sizeof(HwndStr), "0x%llx", (unsigned long long)(ULONG_PTR)W->Hwnd);
        AiJsonBufAppendStr(&Buf, "{\"hwnd\":");
        AiJsonBufAppendJsonString(&Buf, HwndStr);
        AiJsonBufAppendStr(&Buf, ",\"title\":");
        AiJsonBufAppendJsonString(&Buf, W->Title);
        AiJsonBufAppendStr(&Buf, ",\"class\":");
        AiJsonBufAppendJsonString(&Buf, W->ClassName);
        AiJsonBufAppendStr(&Buf, ",\"pid\":");
        AiJsonBufAppendInt64(&Buf, (int64_t)W->Pid);
        AiJsonBufAppendStr(&Buf, ",\"x\":");
        AiJsonBufAppendInt64(&Buf, W->Rect.left);
        AiJsonBufAppendStr(&Buf, ",\"y\":");
        AiJsonBufAppendInt64(&Buf, W->Rect.top);
        AiJsonBufAppendStr(&Buf, ",\"w\":");
        AiJsonBufAppendInt64(&Buf, W->Rect.right - W->Rect.left);
        AiJsonBufAppendStr(&Buf, ",\"h\":");
        AiJsonBufAppendInt64(&Buf, W->Rect.bottom - W->Rect.top);
        AiJsonBufAppendStr(&Buf, W->Active ? ",\"active\":true}" : ",\"active\":false}");
    }
    AiJsonBufAppendStr(&Buf, "]}");
    CopyStrSafe(OutJson, OutCap, Buf.Data ? Buf.Data : "{\"count\":0,\"windows\":[]}");
    AiJsonBufFree(&Buf);
    return 0;
}

typedef struct _AI_CTRL_CTX
{
    AI_JSON_BUF *Buf;
    int Count;
} AI_CTRL_CTX;

static BOOL CALLBACK EnumChildCtrlProc(HWND Child, LPARAM Param)
{
    AI_CTRL_CTX *Ctx = (AI_CTRL_CTX *)Param;
    WCHAR ClsW[64];
    WCHAR TextW[192];
    char ClsUtf8[64];
    char TextUtf8[256];
    char HwndStr[32];
    DWORD_PTR TextLen = 0;
    int CtrlId;

    if (Ctx->Count >= AI_MAX_CONTROLS)
        return FALSE;

    ClsW[0] = UNICODE_NULL;
    TextW[0] = UNICODE_NULL;
    GetClassNameW(Child, ClsW, ARRAYSIZE(ClsW));
    if (!SendMessageTimeoutW(Child, WM_GETTEXT, ARRAYSIZE(TextW), (LPARAM)TextW,
                             SMTO_ABORTIFHUNG, 150, &TextLen))
    {
        GetWindowTextW(Child, TextW, ARRAYSIZE(TextW));
    }
    if (TextW[0] == UNICODE_NULL && ClsW[0] == UNICODE_NULL)
        return TRUE;

    WideToUtf8(ClsW, ClsUtf8, sizeof(ClsUtf8));
    WideToUtf8(TextW, TextUtf8, sizeof(TextUtf8));
    CtrlId = GetDlgCtrlID(Child);
    _snprintf(HwndStr, sizeof(HwndStr), "0x%llx", (unsigned long long)(ULONG_PTR)Child);

    if (Ctx->Count > 0)
        AiJsonBufAppendChar(Ctx->Buf, ',');
    AiJsonBufAppendStr(Ctx->Buf, "{\"hwnd\":");
    AiJsonBufAppendJsonString(Ctx->Buf, HwndStr);
    AiJsonBufAppendStr(Ctx->Buf, ",\"id\":");
    AiJsonBufAppendInt64(Ctx->Buf, CtrlId);
    AiJsonBufAppendStr(Ctx->Buf, ",\"class\":");
    AiJsonBufAppendJsonString(Ctx->Buf, ClsUtf8);
    AiJsonBufAppendStr(Ctx->Buf, ",\"text\":");
    AiJsonBufAppendJsonString(Ctx->Buf, TextUtf8);
    AiJsonBufAppendChar(Ctx->Buf, '}');
    Ctx->Count++;
    return TRUE;
}

static int ToolInspectWindow(HWND ExcludeHwnd, const char *ArgsJson, char *OutJson, size_t OutCap)
{
    AI_ENUM_CTX Ctx;
    char Target[160];
    HWND Hwnd;
    WCHAR TitleW[160];
    char TitleUtf8[160];
    AI_JSON_BUF Buf;
    AI_CTRL_CTX CtrlCtx;

    Target[0] = '\0';
    AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "target", Target, sizeof(Target));

    CollectDesktopWindows(ExcludeHwnd, &Ctx);
    Hwnd = Target[0] ? FindWindowByQuery(&Ctx, Target) : (Ctx.Count > 0 ? Ctx.Items[0].Hwnd : NULL);
    if (Hwnd == NULL)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"matching window not found\"}");
        return -1;
    }

    TitleW[0] = UNICODE_NULL;
    GetWindowTextW(Hwnd, TitleW, ARRAYSIZE(TitleW));
    WideToUtf8(TitleW, TitleUtf8, sizeof(TitleUtf8));

    AiJsonBufInit(&Buf, 512);
    AiJsonBufAppendStr(&Buf, "{\"title\":");
    AiJsonBufAppendJsonString(&Buf, TitleUtf8);
    AiJsonBufAppendStr(&Buf, ",\"controls\":[");
    CtrlCtx.Buf = &Buf;
    CtrlCtx.Count = 0;
    EnumChildWindows(Hwnd, EnumChildCtrlProc, (LPARAM)&CtrlCtx);
    AiJsonBufAppendStr(&Buf, "]}");
    CopyStrSafe(OutJson, OutCap, Buf.Data ? Buf.Data : "{\"controls\":[]}");
    AiJsonBufFree(&Buf);
    return 0;
}

static int ToolFocusWindow(HWND ExcludeHwnd, const char *ArgsJson, char *OutJson, size_t OutCap)
{
    AI_ENUM_CTX Ctx;
    char Target[160];
    HWND Hwnd;

    Target[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "target", Target, sizeof(Target)) != 0 || Target[0] == '\0')
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing target\"}");
        return -1;
    }

    CollectDesktopWindows(ExcludeHwnd, &Ctx);
    Hwnd = FindWindowByQuery(&Ctx, Target);
    if (Hwnd == NULL)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"matching window not found\"}");
        return -1;
    }

    if (IsIconic(Hwnd))
        ShowWindow(Hwnd, SW_RESTORE);
    BringWindowToTop(Hwnd);
    SetForegroundWindow(Hwnd);

    CopyStrSafe(OutJson, OutCap, "{\"ok\":true,\"action\":\"focused\"}");
    return 0;
}

static int ToolArrangeWindows(HWND ExcludeHwnd, const char *ArgsJson, char *OutJson, size_t OutCap)
{
    AI_ENUM_CTX Ctx;
    char Layout[64];
    char TargetsCsv[256];
    HWND Selected[AI_MAX_UNDO_WINDOWS];
    int SelCount = 0;
    RECT WorkArea;
    int i;

    Layout[0] = '\0';
    TargetsCsv[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "layout", Layout, sizeof(Layout)) != 0 || Layout[0] == '\0')
        CopyStrSafe(Layout, sizeof(Layout), "split_vertical");
    AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "targets", TargetsCsv, sizeof(TargetsCsv));

    if (_stricmp(Layout, "undo") == 0)
    {
        int Restored = 0;
        for (i = 0; i < g_UndoCount; i++)
        {
            if (IsWindow(g_UndoStack[i].Hwnd))
            {
                SetWindowPlacement(g_UndoStack[i].Hwnd, &g_UndoStack[i].Placement);
                Restored++;
            }
        }
        g_UndoCount = 0;
        _snprintf(OutJson, OutCap, "{\"ok\":true,\"layout\":\"undo\",\"restored\":%d}", Restored);
        OutJson[OutCap - 1] = '\0';
        return 0;
    }

    CollectDesktopWindows(ExcludeHwnd, &Ctx);

    if (TargetsCsv[0] != '\0')
    {
        char Copy[256];
        char *Tok;
        char *Next = NULL;

        CopyStrSafe(Copy, sizeof(Copy), TargetsCsv);
        Tok = strtok_s(Copy, ",;", &Next);
        while (Tok != NULL && SelCount < AI_MAX_UNDO_WINDOWS)
        {
            HWND Found;
            while (*Tok == ' ')
                Tok++;
            Found = FindWindowByQuery(&Ctx, Tok);
            if (Found != NULL)
            {
                int Dup = 0;
                for (i = 0; i < SelCount; i++)
                {
                    if (Selected[i] == Found)
                        Dup = 1;
                }
                if (!Dup)
                    Selected[SelCount++] = Found;
            }
            Tok = strtok_s(NULL, ",;", &Next);
        }
    }
    else
    {
        for (i = 0; i < Ctx.Count && SelCount < 4; i++)
            Selected[SelCount++] = Ctx.Items[i].Hwnd;
    }

    if (SelCount == 0)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"no matching desktop windows to arrange\"}");
        return -1;
    }

    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &WorkArea, 0))
    {
        WorkArea.left = 0;
        WorkArea.top = 0;
        WorkArea.right = GetSystemMetrics(SM_CXSCREEN);
        WorkArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    SaveUndoPlacements(Selected, SelCount);

    {
        int W = WorkArea.right - WorkArea.left;
        int H = WorkArea.bottom - WorkArea.top;

        if (_stricmp(Layout, "maximize") == 0)
        {
            for (i = 0; i < SelCount; i++)
                ShowWindow(Selected[i], SW_MAXIMIZE);
        }
        else if (_stricmp(Layout, "minimize") == 0)
        {
            for (i = 0; i < SelCount; i++)
                ShowWindow(Selected[i], SW_MINIMIZE);
        }
        else if (_stricmp(Layout, "left") == 0)
        {
            ShowWindow(Selected[0], SW_RESTORE);
            SetWindowPos(Selected[0], HWND_TOP, WorkArea.left, WorkArea.top, W / 2, H, SWP_SHOWWINDOW);
        }
        else if (_stricmp(Layout, "right") == 0)
        {
            ShowWindow(Selected[0], SW_RESTORE);
            SetWindowPos(Selected[0], HWND_TOP, WorkArea.left + W / 2, WorkArea.top, W - W / 2, H, SWP_SHOWWINDOW);
        }
        else if (_stricmp(Layout, "split_horizontal") == 0)
        {
            int SlotH = H / SelCount;
            for (i = 0; i < SelCount; i++)
            {
                ShowWindow(Selected[i], SW_RESTORE);
                SetWindowPos(Selected[i], HWND_TOP, WorkArea.left, WorkArea.top + i * SlotH,
                             W, (i == SelCount - 1) ? (H - i * SlotH) : SlotH, SWP_SHOWWINDOW);
            }
        }
        else if (_stricmp(Layout, "grid") == 0 && SelCount > 2)
        {
            int Cols = 2;
            int Rows = (SelCount + 1) / 2;
            int CellW = W / Cols;
            int CellH = H / Rows;
            for (i = 0; i < SelCount; i++)
            {
                int r = i / Cols;
                int c = i % Cols;
                ShowWindow(Selected[i], SW_RESTORE);
                SetWindowPos(Selected[i], HWND_TOP,
                             WorkArea.left + c * CellW, WorkArea.top + r * CellH,
                             CellW, CellH, SWP_SHOWWINDOW);
            }
        }
        else
        {
            /* Default: split_vertical (side-by-side columns) */
            int SlotW = W / SelCount;
            for (i = 0; i < SelCount; i++)
            {
                ShowWindow(Selected[i], SW_RESTORE);
                SetWindowPos(Selected[i], HWND_TOP, WorkArea.left + i * SlotW, WorkArea.top,
                             (i == SelCount - 1) ? (W - i * SlotW) : SlotW, H, SWP_SHOWWINDOW);
            }
        }
    }

    _snprintf(OutJson, OutCap, "{\"ok\":true,\"layout\":\"%s\",\"arranged\":%d}", Layout, SelCount);
    OutJson[OutCap - 1] = '\0';
    return 0;
}

static int ToolLaunchApp(const char *ArgsJson, char *OutJson, size_t OutCap)
{
    char App[260];
    char Params[260];
    WCHAR AppW[260];
    WCHAR ParamsW[260];
    HINSTANCE Res;

    App[0] = '\0';
    Params[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "app", App, sizeof(App)) != 0 || App[0] == '\0')
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing app\"}");
        return -1;
    }
    AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "args", Params, sizeof(Params));

    if (!Utf8ToWide(App, AppW, ARRAYSIZE(AppW)))
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"invalid app path\"}");
        return -1;
    }
    ParamsW[0] = UNICODE_NULL;
    if (Params[0] != '\0')
        Utf8ToWide(Params, ParamsW, ARRAYSIZE(ParamsW));

    Res = ShellExecuteW(NULL, L"open", AppW, ParamsW[0] ? ParamsW : NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)Res <= 32)
    {
        _snprintf(OutJson, OutCap, "{\"error\":\"ShellExecuteW failed (%d)\"}", (int)(INT_PTR)Res);
        OutJson[OutCap - 1] = '\0';
        return -1;
    }

    _snprintf(OutJson, OutCap, "{\"ok\":true,\"launched\":\"%s\"}", App);
    OutJson[OutCap - 1] = '\0';
    return 0;
}

static int ToolCloseWindow(HWND ExcludeHwnd, const char *ArgsJson, char *OutJson, size_t OutCap)
{
    AI_ENUM_CTX Ctx;
    char Target[160];
    HWND Hwnd;

    Target[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "target", Target, sizeof(Target)) != 0 || Target[0] == '\0')
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing target\"}");
        return -1;
    }

    CollectDesktopWindows(ExcludeHwnd, &Ctx);
    Hwnd = FindWindowByQuery(&Ctx, Target);
    if (Hwnd == NULL)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"matching window not found\"}");
        return -1;
    }

    PostMessageW(Hwnd, WM_CLOSE, 0, 0);
    CopyStrSafe(OutJson, OutCap, "{\"ok\":true,\"action\":\"close_sent\"}");
    return 0;
}

static int ToolGetClipboard(char *OutJson, size_t OutCap)
{
    HANDLE Data;
    PCWSTR TextW;
    char TextUtf8[1536];
    AI_JSON_BUF Buf;

    if (!OpenClipboard(NULL))
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"OpenClipboard failed\"}");
        return -1;
    }

    TextUtf8[0] = '\0';
    Data = GetClipboardData(CF_UNICODETEXT);
    if (Data != NULL)
    {
        TextW = (PCWSTR)GlobalLock(Data);
        if (TextW != NULL)
        {
            WideToUtf8(TextW, TextUtf8, sizeof(TextUtf8));
            GlobalUnlock(Data);
        }
    }
    CloseClipboard();

    AiJsonBufInit(&Buf, 256);
    AiJsonBufAppendStr(&Buf, "{\"ok\":true,\"text\":");
    AiJsonBufAppendJsonString(&Buf, TextUtf8);
    AiJsonBufAppendChar(&Buf, '}');
    CopyStrSafe(OutJson, OutCap, Buf.Data ? Buf.Data : "{\"ok\":true,\"text\":\"\"}");
    AiJsonBufFree(&Buf);
    return 0;
}

static int ToolSetClipboard(const char *ArgsJson, char *OutJson, size_t OutCap)
{
    char TextUtf8[1536];
    int WideLen;
    HGLOBAL Mem;
    PWSTR Dst;

    TextUtf8[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "text", TextUtf8, sizeof(TextUtf8)) != 0)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing text\"}");
        return -1;
    }

    WideLen = MultiByteToWideChar(CP_UTF8, 0, TextUtf8, -1, NULL, 0);
    if (WideLen <= 0)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"invalid utf-8 text\"}");
        return -1;
    }

    Mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)WideLen * sizeof(WCHAR));
    if (Mem == NULL)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"out of memory\"}");
        return -1;
    }

    Dst = (PWSTR)GlobalLock(Mem);
    if (Dst == NULL)
    {
        GlobalFree(Mem);
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"GlobalLock failed\"}");
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, TextUtf8, -1, Dst, WideLen);
    GlobalUnlock(Mem);

    if (!OpenClipboard(NULL))
    {
        GlobalFree(Mem);
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"OpenClipboard failed\"}");
        return -1;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, Mem) == NULL)
    {
        CloseClipboard();
        GlobalFree(Mem);
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"SetClipboardData failed\"}");
        return -1;
    }
    CloseClipboard();

    _snprintf(OutJson, OutCap, "{\"ok\":true,\"chars\":%d}", WideLen - 1);
    OutJson[OutCap - 1] = '\0';
    return 0;
}

typedef struct _AI_FIND_CTRL_CTX
{
    const char *Query;
    HWND Found;
    char FoundClass[64];
} AI_FIND_CTRL_CTX;

static BOOL CALLBACK FindChildCtrlProc(HWND Child, LPARAM Param)
{
    AI_FIND_CTRL_CTX *Ctx = (AI_FIND_CTRL_CTX *)Param;
    WCHAR ClsW[64];
    WCHAR TextW[192];
    char ClsUtf8[64];
    char TextUtf8[256];
    DWORD_PTR TextLen = 0;

    if (Ctx->Found != NULL)
        return FALSE;

    if (Ctx->Query[0] == '0' && (Ctx->Query[1] == 'x' || Ctx->Query[1] == 'X'))
    {
        HWND Target = (HWND)(ULONG_PTR)strtoull(Ctx->Query, NULL, 16);
        if (Target == Child)
        {
            Ctx->Found = Child;
            ClsW[0] = UNICODE_NULL;
            GetClassNameW(Child, ClsW, ARRAYSIZE(ClsW));
            WideToUtf8(ClsW, Ctx->FoundClass, sizeof(Ctx->FoundClass));
            return FALSE;
        }
    }

    ClsW[0] = UNICODE_NULL;
    TextW[0] = UNICODE_NULL;
    GetClassNameW(Child, ClsW, ARRAYSIZE(ClsW));
    if (!SendMessageTimeoutW(Child, WM_GETTEXT, ARRAYSIZE(TextW), (LPARAM)TextW,
                             SMTO_ABORTIFHUNG, 150, &TextLen))
    {
        GetWindowTextW(Child, TextW, ARRAYSIZE(TextW));
    }

    WideToUtf8(ClsW, ClsUtf8, sizeof(ClsUtf8));
    WideToUtf8(TextW, TextUtf8, sizeof(TextUtf8));

    if (ContainsCaseInsensitive(TextUtf8, Ctx->Query) ||
        ContainsCaseInsensitive(ClsUtf8, Ctx->Query))
    {
        Ctx->Found = Child;
        CopyStrSafe(Ctx->FoundClass, sizeof(Ctx->FoundClass), ClsUtf8);
        return FALSE;
    }
    return TRUE;
}

static int ToolClickControl(HWND ExcludeHwnd, const char *ArgsJson, char *OutJson, size_t OutCap)
{
    AI_ENUM_CTX Ctx;
    AI_FIND_CTRL_CTX FindCtx;
    char Target[160];
    char Control[160];
    HWND TopHwnd;
    DWORD_PTR Res = 0;

    Target[0] = '\0';
    Control[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "target", Target, sizeof(Target)) != 0 || Target[0] == '\0' ||
        AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "control", Control, sizeof(Control)) != 0 || Control[0] == '\0')
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing target or control\"}");
        return -1;
    }

    CollectDesktopWindows(ExcludeHwnd, &Ctx);
    TopHwnd = FindWindowByQuery(&Ctx, Target);
    if (TopHwnd == NULL)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"matching window not found\"}");
        return -1;
    }

    memset(&FindCtx, 0, sizeof(FindCtx));
    FindCtx.Query = Control;
    EnumChildWindows(TopHwnd, FindChildCtrlProc, (LPARAM)&FindCtx);
    if (FindCtx.Found == NULL)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"matching control not found\"}");
        return -1;
    }

    if (IsIconic(TopHwnd))
        ShowWindow(TopHwnd, SW_RESTORE);
    SetForegroundWindow(TopHwnd);

    if (ContainsCaseInsensitive(FindCtx.FoundClass, "Button"))
    {
        SendMessageTimeoutW(FindCtx.Found, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 500, &Res);
    }
    else
    {
        PostMessageW(FindCtx.Found, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        PostMessageW(FindCtx.Found, WM_LBUTTONUP, 0, 0);
    }

    CopyStrSafe(OutJson, OutCap, "{\"ok\":true,\"action\":\"clicked\"}");
    return 0;
}

static void SendVirtualKeyCombo(WORD ModVk, WORD KeyVk)
{
    INPUT Inputs[4];
    UINT n = 0;

    memset(Inputs, 0, sizeof(Inputs));
    if (ModVk != 0)
    {
        Inputs[n].type = INPUT_KEYBOARD;
        Inputs[n].ki.wVk = ModVk;
        n++;
    }
    Inputs[n].type = INPUT_KEYBOARD;
    Inputs[n].ki.wVk = KeyVk;
    n++;
    Inputs[n].type = INPUT_KEYBOARD;
    Inputs[n].ki.wVk = KeyVk;
    Inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
    n++;
    if (ModVk != 0)
    {
        Inputs[n].type = INPUT_KEYBOARD;
        Inputs[n].ki.wVk = ModVk;
        Inputs[n].ki.dwFlags = KEYEVENTF_KEYUP;
        n++;
    }
    SendInput(n, Inputs, sizeof(INPUT));
}

static int SendNamedKey(const char *Key)
{
    if (_stricmp(Key, "enter") == 0 || _stricmp(Key, "return") == 0)
        SendVirtualKeyCombo(0, VK_RETURN);
    else if (_stricmp(Key, "escape") == 0 || _stricmp(Key, "esc") == 0)
        SendVirtualKeyCombo(0, VK_ESCAPE);
    else if (_stricmp(Key, "tab") == 0)
        SendVirtualKeyCombo(0, VK_TAB);
    else if (_stricmp(Key, "backspace") == 0)
        SendVirtualKeyCombo(0, VK_BACK);
    else if (_stricmp(Key, "space") == 0)
        SendVirtualKeyCombo(0, VK_SPACE);
    else if (_stricmp(Key, "ctrl+c") == 0)
        SendVirtualKeyCombo(VK_CONTROL, 'C');
    else if (_stricmp(Key, "ctrl+v") == 0)
        SendVirtualKeyCombo(VK_CONTROL, 'V');
    else if (_stricmp(Key, "ctrl+s") == 0)
        SendVirtualKeyCombo(VK_CONTROL, 'S');
    else if (_stricmp(Key, "ctrl+a") == 0)
        SendVirtualKeyCombo(VK_CONTROL, 'A');
    else if (_stricmp(Key, "alt+f4") == 0)
        SendVirtualKeyCombo(VK_MENU, VK_F4);
    else
        return -1;
    return 0;
}

static int ToolSendKeys(HWND ExcludeHwnd, const char *ArgsJson, char *OutJson, size_t OutCap)
{
    char Target[160];
    char TextUtf8[512];
    char KeyName[64];
    int PressEnter = 0;

    Target[0] = '\0';
    TextUtf8[0] = '\0';
    KeyName[0] = '\0';
    AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "target", Target, sizeof(Target));
    AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "text", TextUtf8, sizeof(TextUtf8));
    AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "key", KeyName, sizeof(KeyName));
    AiJsonObjGetBool(ArgsJson, strlen(ArgsJson), "enter", &PressEnter);

    if (TextUtf8[0] == '\0' && KeyName[0] == '\0' && !PressEnter)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing text or key\"}");
        return -1;
    }

    if (Target[0] != '\0')
    {
        AI_ENUM_CTX Ctx;
        HWND Hwnd;

        CollectDesktopWindows(ExcludeHwnd, &Ctx);
        Hwnd = FindWindowByQuery(&Ctx, Target);
        if (Hwnd == NULL)
        {
            CopyStrSafe(OutJson, OutCap, "{\"error\":\"matching window not found\"}");
            return -1;
        }
        if (IsIconic(Hwnd))
            ShowWindow(Hwnd, SW_RESTORE);
        BringWindowToTop(Hwnd);
        SetForegroundWindow(Hwnd);
        Sleep(40);
    }

    if (TextUtf8[0] != '\0')
    {
        WCHAR Wide[512];
        int i;

        if (Utf8ToWide(TextUtf8, Wide, ARRAYSIZE(Wide)))
        {
            for (i = 0; Wide[i] != UNICODE_NULL; i++)
            {
                if (Wide[i] == L'\n')
                {
                    SendVirtualKeyCombo(0, VK_RETURN);
                }
                else if (Wide[i] == L'\t')
                {
                    SendVirtualKeyCombo(0, VK_TAB);
                }
                else if (Wide[i] != L'\r')
                {
                    INPUT Ev[2];
                    memset(Ev, 0, sizeof(Ev));
                    Ev[0].type = INPUT_KEYBOARD;
                    Ev[0].ki.wScan = Wide[i];
                    Ev[0].ki.dwFlags = KEYEVENTF_UNICODE;
                    Ev[1].type = INPUT_KEYBOARD;
                    Ev[1].ki.wScan = Wide[i];
                    Ev[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                    SendInput(2, Ev, sizeof(INPUT));
                }
            }
        }
    }

    if (KeyName[0] != '\0' && SendNamedKey(KeyName) != 0)
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"unsupported key name\"}");
        return -1;
    }
    if (PressEnter)
        SendVirtualKeyCombo(0, VK_RETURN);

    CopyStrSafe(OutJson, OutCap, "{\"ok\":true,\"action\":\"keys_sent\"}");
    return 0;
}

static void EnableShutdownPrivilege(void)
{
    HANDLE Token = NULL;
    TOKEN_PRIVILEGES Tp;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return;

    memset(&Tp, 0, sizeof(Tp));
    if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &Tp.Privileges[0].Luid))
    {
        Tp.PrivilegeCount = 1;
        Tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(Token, FALSE, &Tp, sizeof(Tp), NULL, NULL);
    }
    CloseHandle(Token);
}

static int ToolPowerAction(const char *ArgsJson, char *OutJson, size_t OutCap)
{
    char Action[32];
    BOOL Ok = FALSE;

    Action[0] = '\0';
    if (AiJsonObjGetString(ArgsJson, strlen(ArgsJson), "action", Action, sizeof(Action)) != 0 || Action[0] == '\0')
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"missing action\"}");
        return -1;
    }

    if (_stricmp(Action, "lock") == 0)
    {
        Ok = LockWorkStation();
    }
    else if (_stricmp(Action, "logoff") == 0)
    {
        Ok = ExitWindowsEx(EWX_LOGOFF, 0);
    }
    else if (_stricmp(Action, "reboot") == 0)
    {
        EnableShutdownPrivilege();
        Ok = ExitWindowsEx(EWX_REBOOT, 0);
    }
    else if (_stricmp(Action, "shutdown") == 0)
    {
        EnableShutdownPrivilege();
        Ok = ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, 0);
    }
    else
    {
        CopyStrSafe(OutJson, OutCap, "{\"error\":\"unsupported power_action (use lock, logoff, reboot, shutdown)\"}");
        return -1;
    }

    if (!Ok)
    {
        _snprintf(OutJson, OutCap, "{\"error\":\"power_action %s failed (%lu)\"}",
                  Action, (unsigned long)GetLastError());
        OutJson[OutCap - 1] = '\0';
        return -1;
    }

    _snprintf(OutJson, OutCap, "{\"ok\":true,\"action\":\"%s\"}", Action);
    OutJson[OutCap - 1] = '\0';
    return 0;
}

int AiDesktopExecuteTool(
    _In_ const char *ToolName,
    _In_ const char *ArgsJson,
    _In_opt_ HWND ExcludeHwnd,
    _Out_writes_(OutCap) char *OutJson,
    _In_ size_t OutCap)
{
    if (ToolName == NULL || OutJson == NULL || OutCap < 16)
        return -1;
    if (ArgsJson == NULL)
        ArgsJson = "{}";
    OutJson[0] = '\0';

    if (strcmp(ToolName, "list_windows") == 0)
        return ToolListWindows(ExcludeHwnd, OutJson, OutCap);
    if (strcmp(ToolName, "inspect_window") == 0)
        return ToolInspectWindow(ExcludeHwnd, ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "focus_window") == 0)
        return ToolFocusWindow(ExcludeHwnd, ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "arrange_windows") == 0)
        return ToolArrangeWindows(ExcludeHwnd, ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "launch_app") == 0)
        return ToolLaunchApp(ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "close_window") == 0)
        return ToolCloseWindow(ExcludeHwnd, ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "get_clipboard") == 0)
        return ToolGetClipboard(OutJson, OutCap);
    if (strcmp(ToolName, "set_clipboard") == 0)
        return ToolSetClipboard(ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "click_control") == 0)
        return ToolClickControl(ExcludeHwnd, ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "send_keys") == 0)
        return ToolSendKeys(ExcludeHwnd, ArgsJson, OutJson, OutCap);
    if (strcmp(ToolName, "power_action") == 0)
        return ToolPowerAction(ArgsJson, OutJson, OutCap);

    CopyStrSafe(OutJson, OutCap, "{\"error\":\"unknown desktop tool\"}");
    return -1;
}
