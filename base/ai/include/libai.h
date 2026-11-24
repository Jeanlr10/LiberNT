/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Public client SDK for communicating with aisvc over ALPC and managing NTFS folder descriptions
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <windef.h>
#include "ai_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LIBAI_EXPORTS
#define LIBAI_API __declspec(dllexport)
#else
#define LIBAI_API __declspec(dllimport)
#endif

LIBAI_API
DWORD
WINAPI
AiClientConnect(
    _Out_ PHANDLE OutPort);

LIBAI_API
VOID
WINAPI
AiClientClose(
    _In_ HANDLE Port);

LIBAI_API
DWORD
WINAPI
AiClientGetStatus(
    _In_ HANDLE Port,
    _Out_ AI_STATUS_INFO *OutInfo);

LIBAI_API
DWORD
WINAPI
AiClientSearch(
    _In_ HANDLE Port,
    _In_ PCSTR QueryUtf8,
    _In_ DWORD MaxResults,
    _Out_ AI_SEARCH_HIT *OutHits,
    _Out_ PDWORD OutCount);

LIBAI_API
DWORD
WINAPI
AiClientIndexNow(
    _In_ HANDLE Port,
    _Out_opt_ PDWORD OutIndexedCount);

LIBAI_API
DWORD
WINAPI
AiClientSetFolderDescription(
    _In_opt_ HANDLE Port,
    _In_ PCWSTR FolderPath,
    _In_ PCSTR DescUtf8,
    _In_opt_ PCSTR SourceUtf8,
    _In_opt_ PCSTR TagsUtf8);

LIBAI_API
DWORD
WINAPI
AiClientGetFolderDescription(
    _In_opt_ HANDLE Port,
    _In_ PCWSTR FolderPath,
    _Out_ PSTR DescUtf8,
    _In_ DWORD DescCap,
    _Out_opt_ PSTR SourceUtf8,
    _In_ DWORD SourceCap);

LIBAI_API
DWORD
WINAPI
AiClientClearFolderDescription(
    _In_opt_ HANDLE Port,
    _In_ PCWSTR FolderPath);

LIBAI_API
DWORD
WINAPI
AiAdsWriteFolderDescription(
    _In_ PCWSTR FolderPath,
    _In_ PCSTR DescUtf8,
    _In_opt_ PCSTR SourceUtf8,
    _In_opt_ PCSTR TagsUtf8);

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
    _In_ DWORD TagsCap);

LIBAI_API
DWORD
WINAPI
AiAdsClearFolderDescription(
    _In_ PCWSTR FolderPath);

#ifdef __cplusplus
}
#endif
