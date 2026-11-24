/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Native C11 GGUF v2/v3 loader and quantized Llama-architecture transformer inference engine
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _AI_GGUF_MODEL AI_GGUF_MODEL;

typedef struct _AI_GGUF_INFO
{
    uint32_t Version;
    uint32_t TensorCount;
    uint32_t VocabSize;
    uint32_t EmbedDim;
    uint32_t FfnDim;
    uint32_t LayerCount;
    uint32_t HeadCount;
    uint32_t KvHeadCount;
    uint32_t ContextLen;
    int Avx2Enabled;
    char Arch[32];
    char Name[64];
    char QuantSummary[32];
} AI_GGUF_INFO;

int AiGgufLoadFile(
    const char *PathUtf8,
    AI_GGUF_MODEL **OutModel,
    char *ErrBuf,
    size_t ErrCap);

void AiGgufFree(AI_GGUF_MODEL *Model);

void AiGgufGetInfo(const AI_GGUF_MODEL *Model, AI_GGUF_INFO *OutInfo);

uint32_t AiGgufEncode(
    const AI_GGUF_MODEL *Model,
    const char *TextUtf8,
    int AddBos,
    uint32_t *OutTokens,
    uint32_t MaxTokens);

int AiGgufGenerate(
    AI_GGUF_MODEL *Model,
    const char *PromptUtf8,
    uint32_t MaxNewTokens,
    char *OutUtf8,
    size_t OutCap);

int AiGgufEmbed(
    AI_GGUF_MODEL *Model,
    const char *TextUtf8,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim);

float AiGgufHalfToFloat(uint16_t Half);

#ifdef __cplusplus
}
#endif
