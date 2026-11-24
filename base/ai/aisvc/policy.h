/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Capability tier definitions and deterministic policy gate for OS agent tools
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _AI_CAPABILITY_TIER
{
    AiTier0ReadOnly = 0,
    AiTier1Reversible = 1,
    AiTier2Mutating = 2,
    AiTierUnknown = 99
} AI_CAPABILITY_TIER;

typedef enum _AI_POLICY_DECISION
{
    AiPolicyAllow = 0,
    AiPolicyNeedApproval = 1,
    AiPolicyDeny = 2
} AI_POLICY_DECISION;

AI_CAPABILITY_TIER AiPolicyGetToolTier(const char *ToolName);

AI_POLICY_DECISION AiPolicyEvaluate(
    const char *ToolName,
    int AskOnlyMode,
    int RequireApprovalTier1);

void AiPolicyExtractTarget(
    const char *ToolName,
    const char *ArgsJson,
    char *TargetOut,
    size_t TargetCap);

const char *AiPolicyGetToolsSchemaJson(int AskOnlyMode);

#ifdef __cplusplus
}
#endif
