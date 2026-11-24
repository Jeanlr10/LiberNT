/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Native C11 GGUF v2/v3 loader and quantized Llama-architecture transformer inference engine
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_NO_STATUS
#define _INC_WINDOWS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if (defined(__x86_64__) || defined(_M_AMD64)) && (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#include <immintrin.h>
#define AI_HAS_X86_AVX2_DISPATCH 1
#else
#define AI_HAS_X86_AVX2_DISPATCH 0
#endif

#include "gguf_engine.h"

#define GGUF_MAGIC 0x46554747U /* "GGUF" */

enum
{
    GGUF_TYPE_UINT8   = 0,
    GGUF_TYPE_INT8    = 1,
    GGUF_TYPE_UINT16  = 2,
    GGUF_TYPE_INT16   = 3,
    GGUF_TYPE_UINT32  = 4,
    GGUF_TYPE_INT32   = 5,
    GGUF_TYPE_FLOAT32 = 6,
    GGUF_TYPE_BOOL    = 7,
    GGUF_TYPE_STRING  = 8,
    GGUF_TYPE_ARRAY   = 9,
    GGUF_TYPE_UINT64  = 10,
    GGUF_TYPE_INT64   = 11,
    GGUF_TYPE_FLOAT64 = 12
};

enum
{
    GGML_TYPE_F32  = 0,
    GGML_TYPE_F16  = 1,
    GGML_TYPE_Q4_0 = 2,
    GGML_TYPE_Q8_0 = 8
};

#define AI_GGUF_MAX_CTX_CAP 512U

typedef struct _AI_GGUF_TENSOR
{
    char Name[96];
    uint32_t NDims;
    uint64_t Dims[4];
    uint32_t Type;
    uint64_t Offset;
    const uint8_t *Data;
} AI_GGUF_TENSOR;

typedef struct _AI_GGUF_LAYER
{
    const AI_GGUF_TENSOR *AttnNorm;
    const AI_GGUF_TENSOR *Wq;
    const AI_GGUF_TENSOR *Wk;
    const AI_GGUF_TENSOR *Wv;
    const AI_GGUF_TENSOR *Wo;
    const AI_GGUF_TENSOR *FfnNorm;
    const AI_GGUF_TENSOR *Wgate;
    const AI_GGUF_TENSOR *Wup;
    const AI_GGUF_TENSOR *Wdown;
} AI_GGUF_LAYER;

struct _AI_GGUF_MODEL
{
    /* Memory-mapped file backing */
    const uint8_t *MapBase;
    size_t MapSize;
#ifdef _WIN32
    HANDLE FileHandle;
    HANDLE MappingHandle;
#else
    int FileFd;
#endif

    /* Model metadata & hyperparameters */
    AI_GGUF_INFO Info;
    float RmsEps;
    float RopeTheta;
    uint32_t Alignment;
    uint32_t BosToken;
    uint32_t EosToken;

    /* Vocabulary */
    char **Vocab;
    uint32_t *VocabLen;

    /* Tensors & layer pointers */
    AI_GGUF_TENSOR *Tensors;
    const AI_GGUF_TENSOR *TokEmbd;
    const AI_GGUF_TENSOR *OutNorm;
    const AI_GGUF_TENSOR *OutWeight;
    AI_GGUF_LAYER *Layers;

    /* Reusable inference scratch & KV cache */
    uint32_t RuntimeCtx;
    float *X;
    float *Xb;
    float *Xb2;
    float *Hb;
    float *Hb2;
    float *Q;
    float *K;
    float *V;
    float *Att;
    float *Logits;
    float *KeyCache;
    float *ValCache;
};

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

/* ==================== IEEE 754 FP16 & SIMD Dot Products ==================== */

float AiGgufHalfToFloat(uint16_t Half)
{
    uint32_t Sign = (uint32_t)(Half & 0x8000U) << 16;
    uint32_t Exp = (uint32_t)(Half >> 10) & 0x1FU;
    uint32_t Mant = (uint32_t)(Half & 0x03FFU);
    uint32_t Bits;
    float Out;

    if (Exp == 0)
    {
        if (Mant == 0)
        {
            Bits = Sign;
        }
        else
        {
            Exp = 127 - 14;
            while ((Mant & 0x0400U) == 0)
            {
                Mant <<= 1;
                Exp--;
            }
            Mant &= 0x03FFU;
            Bits = Sign | (Exp << 23) | (Mant << 13);
        }
    }
    else if (Exp == 31)
    {
        Bits = Sign | 0x7F800000U | (Mant << 13);
    }
    else
    {
        Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
    }

    memcpy(&Out, &Bits, sizeof(Out));
    return Out;
}

static int DetectCpuAvx2Fma(void)
{
#if AI_HAS_X86_AVX2_DISPATCH
    unsigned int Eax = 0, Ebx = 0, Ecx = 0, Edx = 0;
    unsigned int MaxLeaf = __get_cpuid_max(0, NULL);
    if (MaxLeaf < 7)
        return 0;
    if (!__get_cpuid(1, &Eax, &Ebx, &Ecx, &Edx))
        return 0;
    /* Check OSXSAVE (bit 27), AVX (bit 28), FMA (bit 12) */
    if ((Ecx & (1U << 27)) == 0 || (Ecx & (1U << 28)) == 0 || (Ecx & (1U << 12)) == 0)
        return 0;
    __cpuid_count(7, 0, Eax, Ebx, Ecx, Edx);
    /* Check AVX2 (EBX bit 5) */
    if ((Ebx & (1U << 5)) == 0)
        return 0;
    return 1;
#else
    return 0;
#endif
}

static float DotF32Scalar(const float *A, const float *B, uint32_t N)
{
    float Sum = 0.0f;
    uint32_t i;
    for (i = 0; i < N; i++)
        Sum += A[i] * B[i];
    return Sum;
}

#if AI_HAS_X86_AVX2_DISPATCH
__attribute__((target("avx2,fma")))
static float DotF32Avx2(const float *A, const float *B, uint32_t N)
{
    __m256 Acc = _mm256_setzero_ps();
    uint32_t i = 0;
    float Lanes[8];
    float Sum;

    for (; i + 8 <= N; i += 8)
    {
        __m256 Va = _mm256_loadu_ps(A + i);
        __m256 Vb = _mm256_loadu_ps(B + i);
        Acc = _mm256_fmadd_ps(Va, Vb, Acc);
    }
    _mm256_storeu_ps(Lanes, Acc);
    Sum = (Lanes[0] + Lanes[1]) + (Lanes[2] + Lanes[3]) + (Lanes[4] + Lanes[5]) + (Lanes[6] + Lanes[7]);
    for (; i < N; i++)
        Sum += A[i] * B[i];
    return Sum;
}
#endif

static size_t RowByteSize(uint32_t Type, uint32_t Cols)
{
    switch (Type)
    {
        case GGML_TYPE_F32:
            return (size_t)Cols * sizeof(float);
        case GGML_TYPE_F16:
            return (size_t)Cols * sizeof(uint16_t);
        case GGML_TYPE_Q8_0:
            return (size_t)((Cols + 31U) / 32U) * 34U;
        case GGML_TYPE_Q4_0:
            return (size_t)((Cols + 31U) / 32U) * 18U;
        default:
            return 0;
    }
}

static void DequantizeRow(const AI_GGUF_TENSOR *T, uint32_t RowIdx, float *Out, uint32_t Cols)
{
    size_t Stride = RowByteSize(T->Type, Cols);
    const uint8_t *Src = T->Data + (size_t)RowIdx * Stride;
    uint32_t i;

    if (T->Type == GGML_TYPE_F32)
    {
        memcpy(Out, Src, (size_t)Cols * sizeof(float));
    }
    else if (T->Type == GGML_TYPE_F16)
    {
        for (i = 0; i < Cols; i++)
        {
            uint16_t H;
            memcpy(&H, Src + i * 2, 2);
            Out[i] = AiGgufHalfToFloat(H);
        }
    }
    else if (T->Type == GGML_TYPE_Q8_0)
    {
        uint32_t Nb = (Cols + 31U) / 32U;
        uint32_t b;
        for (b = 0; b < Nb; b++)
        {
            const uint8_t *Blk = Src + (size_t)b * 34U;
            uint16_t DHalf;
            float Scale;
            uint32_t j;
            memcpy(&DHalf, Blk, 2);
            Scale = AiGgufHalfToFloat(DHalf);
            for (j = 0; j < 32 && b * 32U + j < Cols; j++)
                Out[b * 32U + j] = Scale * (float)(int8_t)Blk[2 + j];
        }
    }
    else if (T->Type == GGML_TYPE_Q4_0)
    {
        uint32_t Nb = (Cols + 31U) / 32U;
        uint32_t b;
        for (b = 0; b < Nb; b++)
        {
            const uint8_t *Blk = Src + (size_t)b * 18U;
            uint16_t DHalf;
            float Scale;
            uint32_t j;
            memcpy(&DHalf, Blk, 2);
            Scale = AiGgufHalfToFloat(DHalf);
            for (j = 0; j < 16; j++)
            {
                uint8_t Byte = Blk[2 + j];
                int Lo = (int)(Byte & 0x0FU) - 8;
                int Hi = (int)(Byte >> 4) - 8;
                if (b * 32U + j < Cols)
                    Out[b * 32U + j] = Scale * (float)Lo;
                if (b * 32U + 16U + j < Cols)
                    Out[b * 32U + 16U + j] = Scale * (float)Hi;
            }
        }
    }
}

static float DotQuantRow(const AI_GGUF_TENSOR *T, uint32_t RowIdx, const float *X, uint32_t Cols, int UseAvx2)
{
    size_t Stride = RowByteSize(T->Type, Cols);
    const uint8_t *Src = T->Data + (size_t)RowIdx * Stride;

    if (T->Type == GGML_TYPE_F32)
    {
        const float *SrcF = (const float *)(const void *)Src;
#if AI_HAS_X86_AVX2_DISPATCH
        if (UseAvx2)
            return DotF32Avx2(SrcF, X, Cols);
#else
        (void)UseAvx2;
#endif
        return DotF32Scalar(SrcF, X, Cols);
    }

    if (T->Type == GGML_TYPE_F16)
    {
        float Sum = 0.0f;
        uint32_t i;
        for (i = 0; i < Cols; i++)
        {
            uint16_t H;
            memcpy(&H, Src + i * 2, 2);
            Sum += AiGgufHalfToFloat(H) * X[i];
        }
        return Sum;
    }

    if (T->Type == GGML_TYPE_Q8_0)
    {
        uint32_t Nb = (Cols + 31U) / 32U;
        uint32_t b;
        float Total = 0.0f;
        for (b = 0; b < Nb; b++)
        {
            const uint8_t *Blk = Src + (size_t)b * 34U;
            uint16_t DHalf;
            float Scale;
            float Sub = 0.0f;
            uint32_t j;
            uint32_t Base = b * 32U;
            memcpy(&DHalf, Blk, 2);
            Scale = AiGgufHalfToFloat(DHalf);
            for (j = 0; j < 32 && Base + j < Cols; j++)
                Sub += (float)(int8_t)Blk[2 + j] * X[Base + j];
            Total += Scale * Sub;
        }
        return Total;
    }

    if (T->Type == GGML_TYPE_Q4_0)
    {
        uint32_t Nb = (Cols + 31U) / 32U;
        uint32_t b;
        float Total = 0.0f;
        for (b = 0; b < Nb; b++)
        {
            const uint8_t *Blk = Src + (size_t)b * 18U;
            uint16_t DHalf;
            float Scale;
            float Sub = 0.0f;
            uint32_t j;
            uint32_t Base = b * 32U;
            memcpy(&DHalf, Blk, 2);
            Scale = AiGgufHalfToFloat(DHalf);
            for (j = 0; j < 16; j++)
            {
                uint8_t Byte = Blk[2 + j];
                if (Base + j < Cols)
                    Sub += (float)((int)(Byte & 0x0FU) - 8) * X[Base + j];
                if (Base + 16U + j < Cols)
                    Sub += (float)((int)(Byte >> 4) - 8) * X[Base + 16U + j];
            }
            Total += Scale * Sub;
        }
        return Total;
    }

    return 0.0f;
}

static void MatVec(float *Out, const float *X, const AI_GGUF_TENSOR *W, uint32_t Rows, uint32_t Cols, int UseAvx2)
{
    uint32_t r;
    for (r = 0; r < Rows; r++)
        Out[r] = DotQuantRow(W, r, X, Cols, UseAvx2);
}

static void RmsNorm(float *Out, const float *X, const AI_GGUF_TENSOR *W, uint32_t N, float Eps)
{
    float Ss = 0.0f;
    float Inv;
    uint32_t i;

    for (i = 0; i < N; i++)
        Ss += X[i] * X[i];
    Ss = Ss / (float)N + Eps;
    Inv = 1.0f / sqrtf(Ss);

    if (W->Type == GGML_TYPE_F32)
    {
        const float *Wf = (const float *)(const void *)W->Data;
        for (i = 0; i < N; i++)
            Out[i] = Wf[i] * (Inv * X[i]);
    }
    else
    {
        DequantizeRow(W, 0, Out, N);
        for (i = 0; i < N; i++)
            Out[i] *= (Inv * X[i]);
    }
}

static void Softmax(float *X, uint32_t N)
{
    float MaxVal = X[0];
    float Sum = 0.0f;
    uint32_t i;

    for (i = 1; i < N; i++)
    {
        if (X[i] > MaxVal)
            MaxVal = X[i];
    }
    for (i = 0; i < N; i++)
    {
        X[i] = expf(X[i] - MaxVal);
        Sum += X[i];
    }
    if (Sum > 0.0f)
    {
        for (i = 0; i < N; i++)
            X[i] /= Sum;
    }
}

/* ==================== GGUF v2/v3 Binary Reader ==================== */

typedef struct _BIN_READER
{
    const uint8_t *Base;
    size_t Size;
    size_t Pos;
} BIN_READER;

static int RdBytes(BIN_READER *R, void *Out, size_t Len)
{
    if (Len > R->Size - R->Pos)
        return -1;
    if (Out != NULL && Len > 0)
        memcpy(Out, R->Base + R->Pos, Len);
    R->Pos += Len;
    return 0;
}

static int RdU32(BIN_READER *R, uint32_t *Out) { return RdBytes(R, Out, 4); }
static int RdU64(BIN_READER *R, uint64_t *Out) { return RdBytes(R, Out, 8); }
static int RdF32(BIN_READER *R, float *Out)    { return RdBytes(R, Out, 4); }

static int RdString(BIN_READER *R, char *Out, size_t OutCap)
{
    uint64_t Len = 0;
    size_t CopyLen;

    if (RdU64(R, &Len) != 0)
        return -1;
    if (Len > R->Size - R->Pos)
        return -1;
    if (Out != NULL && OutCap > 0)
    {
        CopyLen = (Len < OutCap - 1) ? (size_t)Len : (OutCap - 1);
        memcpy(Out, R->Base + R->Pos, CopyLen);
        Out[CopyLen] = '\0';
    }
    R->Pos += (size_t)Len;
    return 0;
}

static int SkipValue(BIN_READER *R, uint32_t Type)
{
    switch (Type)
    {
        case GGUF_TYPE_UINT8:
        case GGUF_TYPE_INT8:
        case GGUF_TYPE_BOOL:
            return RdBytes(R, NULL, 1);
        case GGUF_TYPE_UINT16:
        case GGUF_TYPE_INT16:
            return RdBytes(R, NULL, 2);
        case GGUF_TYPE_UINT32:
        case GGUF_TYPE_INT32:
        case GGUF_TYPE_FLOAT32:
            return RdBytes(R, NULL, 4);
        case GGUF_TYPE_UINT64:
        case GGUF_TYPE_INT64:
        case GGUF_TYPE_FLOAT64:
            return RdBytes(R, NULL, 8);
        case GGUF_TYPE_STRING:
            return RdString(R, NULL, 0);
        case GGUF_TYPE_ARRAY:
        {
            uint32_t ElemType = 0;
            uint64_t Count = 0;
            uint64_t i;
            if (RdU32(R, &ElemType) != 0 || RdU64(R, &Count) != 0)
                return -1;
            for (i = 0; i < Count; i++)
            {
                if (SkipValue(R, ElemType) != 0)
                    return -1;
            }
            return 0;
        }
        default:
            return -1;
    }
}

static int RdScalarU32(BIN_READER *R, uint32_t Type, uint32_t *Out)
{
    if (Type == GGUF_TYPE_UINT32 || Type == GGUF_TYPE_INT32)
        return RdU32(R, Out);
    if (Type == GGUF_TYPE_UINT64 || Type == GGUF_TYPE_INT64)
    {
        uint64_t V = 0;
        if (RdU64(R, &V) != 0)
            return -1;
        *Out = (uint32_t)V;
        return 0;
    }
    return SkipValue(R, Type);
}

static int KeyEndsWith(const char *Key, const char *Suffix)
{
    size_t KLen = strlen(Key);
    size_t SLen = strlen(Suffix);
    if (KLen < SLen)
        return 0;
    return strcmp(Key + KLen - SLen, Suffix) == 0;
}

static const AI_GGUF_TENSOR *FindTensor(const AI_GGUF_MODEL *Model, const char *Name)
{
    uint32_t i;
    for (i = 0; i < Model->Info.TensorCount; i++)
    {
        if (strcmp(Model->Tensors[i].Name, Name) == 0)
            return &Model->Tensors[i];
    }
    return NULL;
}

static const char *QuantTypeName(uint32_t Type)
{
    switch (Type)
    {
        case GGML_TYPE_F32:  return "F32";
        case GGML_TYPE_F16:  return "F16";
        case GGML_TYPE_Q4_0: return "Q4_0";
        case GGML_TYPE_Q8_0: return "Q8_0";
        default:             return "UNSUPPORTED";
    }
}

static int MapModelFile(const char *PathUtf8, AI_GGUF_MODEL *M, char *ErrBuf, size_t ErrCap)
{
#ifdef _WIN32
    WCHAR PathW[MAX_PATH];
    LARGE_INTEGER FileSize;

    if (MultiByteToWideChar(CP_UTF8, 0, PathUtf8, -1, PathW, ARRAYSIZE(PathW)) <= 0)
    {
        CopyStrSafe(ErrBuf, ErrCap, "invalid model path");
        return -1;
    }
    M->FileHandle = CreateFileW(
        PathW, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (M->FileHandle == INVALID_HANDLE_VALUE)
    {
        M->FileHandle = NULL;
        _snprintf(ErrBuf, ErrCap, "cannot open GGUF file (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    FileSize.QuadPart = 0;
    if (!GetFileSizeEx(M->FileHandle, &FileSize))
    {
        _snprintf(ErrBuf, ErrCap, "GetFileSizeEx failed (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    if (FileSize.QuadPart < 24)
    {
        _snprintf(ErrBuf, ErrCap, "GGUF file too small (%lld)", (long long)FileSize.QuadPart);
        return -1;
    }
    M->MappingHandle = CreateFileMappingW(M->FileHandle, NULL, PAGE_READONLY, 0, 0, NULL);
    if (M->MappingHandle == NULL)
    {
        CopyStrSafe(ErrBuf, ErrCap, "CreateFileMappingW failed");
        return -1;
    }
    M->MapBase = (const uint8_t *)MapViewOfFile(M->MappingHandle, FILE_MAP_READ, 0, 0, 0);
    if (M->MapBase == NULL)
    {
        CopyStrSafe(ErrBuf, ErrCap, "MapViewOfFile failed");
        return -1;
    }
    M->MapSize = (size_t)FileSize.QuadPart;
    return 0;
#else
    struct stat St;
    M->FileFd = open(PathUtf8, O_RDONLY);
    if (M->FileFd < 0)
    {
        CopyStrSafe(ErrBuf, ErrCap, "cannot open GGUF file");
        return -1;
    }
    if (fstat(M->FileFd, &St) != 0 || St.st_size < 24)
    {
        CopyStrSafe(ErrBuf, ErrCap, "GGUF file too small");
        return -1;
    }
    M->MapBase = (const uint8_t *)mmap(NULL, (size_t)St.st_size, PROT_READ, MAP_PRIVATE, M->FileFd, 0);
    if (M->MapBase == MAP_FAILED)
    {
        M->MapBase = NULL;
        CopyStrSafe(ErrBuf, ErrCap, "mmap failed");
        return -1;
    }
    M->MapSize = (size_t)St.st_size;
    return 0;
#endif
}

int AiGgufLoadFile(
    const char *PathUtf8,
    AI_GGUF_MODEL **OutModel,
    char *ErrBuf,
    size_t ErrCap)
{
    AI_GGUF_MODEL *M = NULL;
    BIN_READER R;
    uint32_t Magic = 0;
    uint32_t Version = 0;
    uint64_t TensorCount = 0;
    uint64_t KvCount = 0;
    uint64_t i;
    size_t DataOffset;
    uint32_t l;

    if (OutModel == NULL || PathUtf8 == NULL || PathUtf8[0] == '\0')
        return -1;
    *OutModel = NULL;
    if (ErrBuf && ErrCap > 0)
        ErrBuf[0] = '\0';

    M = (AI_GGUF_MODEL *)calloc(1, sizeof(*M));
    if (M == NULL)
        return -1;
#ifndef _WIN32
    M->FileFd = -1;
#endif
    M->RmsEps = 1e-5f;
    M->RopeTheta = 10000.0f;
    M->Alignment = 32U;
    M->BosToken = 1U;
    M->EosToken = 2U;
    M->Info.Avx2Enabled = DetectCpuAvx2Fma();
    CopyStrSafe(M->Info.Arch, sizeof(M->Info.Arch), "llama");

    if (MapModelFile(PathUtf8, M, ErrBuf, ErrCap) != 0)
        goto Fail;

    R.Base = M->MapBase;
    R.Size = M->MapSize;
    R.Pos = 0;

    if (RdU32(&R, &Magic) != 0 || Magic != GGUF_MAGIC)
    {
        CopyStrSafe(ErrBuf, ErrCap, "invalid GGUF magic");
        goto Fail;
    }
    if (RdU32(&R, &Version) != 0 || (Version != 2 && Version != 3))
    {
        CopyStrSafe(ErrBuf, ErrCap, "unsupported GGUF version");
        goto Fail;
    }
    if (RdU64(&R, &TensorCount) != 0 || RdU64(&R, &KvCount) != 0)
    {
        CopyStrSafe(ErrBuf, ErrCap, "truncated GGUF header");
        goto Fail;
    }
    if (TensorCount == 0 || TensorCount > 8192)
    {
        CopyStrSafe(ErrBuf, ErrCap, "invalid tensor count");
        goto Fail;
    }

    M->Info.Version = Version;
    M->Info.TensorCount = (uint32_t)TensorCount;

    for (i = 0; i < KvCount; i++)
    {
        char Key[128];
        uint32_t ValType = 0;

        if (RdString(&R, Key, sizeof(Key)) != 0 || RdU32(&R, &ValType) != 0)
        {
            CopyStrSafe(ErrBuf, ErrCap, "corrupt GGUF metadata KV");
            goto Fail;
        }

        if (strcmp(Key, "general.architecture") == 0 && ValType == GGUF_TYPE_STRING)
        {
            if (RdString(&R, M->Info.Arch, sizeof(M->Info.Arch)) != 0)
                goto Fail;
        }
        else if (strcmp(Key, "general.name") == 0 && ValType == GGUF_TYPE_STRING)
        {
            if (RdString(&R, M->Info.Name, sizeof(M->Info.Name)) != 0)
                goto Fail;
        }
        else if (strcmp(Key, "general.alignment") == 0)
        {
            if (RdScalarU32(&R, ValType, &M->Alignment) != 0)
                goto Fail;
            if (M->Alignment == 0)
                M->Alignment = 32U;
        }
        else if (KeyEndsWith(Key, ".block_count"))
        {
            if (RdScalarU32(&R, ValType, &M->Info.LayerCount) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".embedding_length"))
        {
            if (RdScalarU32(&R, ValType, &M->Info.EmbedDim) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".feed_forward_length"))
        {
            if (RdScalarU32(&R, ValType, &M->Info.FfnDim) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".attention.head_count"))
        {
            if (RdScalarU32(&R, ValType, &M->Info.HeadCount) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".attention.head_count_kv"))
        {
            if (RdScalarU32(&R, ValType, &M->Info.KvHeadCount) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".context_length"))
        {
            if (RdScalarU32(&R, ValType, &M->Info.ContextLen) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".layer_norm_rms_epsilon") && ValType == GGUF_TYPE_FLOAT32)
        {
            if (RdF32(&R, &M->RmsEps) != 0)
                goto Fail;
        }
        else if (KeyEndsWith(Key, ".rope.freq_base") && ValType == GGUF_TYPE_FLOAT32)
        {
            if (RdF32(&R, &M->RopeTheta) != 0)
                goto Fail;
        }
        else if (strcmp(Key, "tokenizer.ggml.bos_token_id") == 0)
        {
            if (RdScalarU32(&R, ValType, &M->BosToken) != 0)
                goto Fail;
        }
        else if (strcmp(Key, "tokenizer.ggml.eos_token_id") == 0)
        {
            if (RdScalarU32(&R, ValType, &M->EosToken) != 0)
                goto Fail;
        }
        else if (strcmp(Key, "tokenizer.ggml.tokens") == 0 && ValType == GGUF_TYPE_ARRAY)
        {
            uint32_t ElemType = 0;
            uint64_t Count = 0;
            uint64_t v;

            if (RdU32(&R, &ElemType) != 0 || RdU64(&R, &Count) != 0 || ElemType != GGUF_TYPE_STRING)
                goto Fail;
            if (Count == 0 || Count > 262144)
                goto Fail;

            M->Info.VocabSize = (uint32_t)Count;
            M->Vocab = (char **)calloc((size_t)Count, sizeof(char *));
            M->VocabLen = (uint32_t *)calloc((size_t)Count, sizeof(uint32_t));
            if (M->Vocab == NULL || M->VocabLen == NULL)
                goto Fail;

            for (v = 0; v < Count; v++)
            {
                uint64_t SLen = 0;
                if (RdU64(&R, &SLen) != 0 || SLen > R.Size - R.Pos || SLen > 1024)
                    goto Fail;
                M->Vocab[v] = (char *)malloc((size_t)SLen + 1);
                if (M->Vocab[v] == NULL)
                    goto Fail;
                memcpy(M->Vocab[v], R.Base + R.Pos, (size_t)SLen);
                M->Vocab[v][SLen] = '\0';
                M->VocabLen[v] = (uint32_t)SLen;
                R.Pos += (size_t)SLen;
            }
        }
        else
        {
            if (SkipValue(&R, ValType) != 0)
                goto Fail;
        }
    }

    M->Tensors = (AI_GGUF_TENSOR *)calloc((size_t)TensorCount, sizeof(AI_GGUF_TENSOR));
    if (M->Tensors == NULL)
        goto Fail;

    for (i = 0; i < TensorCount; i++)
    {
        AI_GGUF_TENSOR *T = &M->Tensors[i];
        uint32_t d;

        if (RdString(&R, T->Name, sizeof(T->Name)) != 0 || RdU32(&R, &T->NDims) != 0)
            goto Fail;
        if (T->NDims == 0 || T->NDims > 4)
            goto Fail;
        for (d = 0; d < T->NDims; d++)
        {
            if (RdU64(&R, &T->Dims[d]) != 0)
                goto Fail;
        }
        if (RdU32(&R, &T->Type) != 0 || RdU64(&R, &T->Offset) != 0)
            goto Fail;
    }

    DataOffset = (R.Pos + (size_t)M->Alignment - 1U) & ~((size_t)M->Alignment - 1U);
    if (DataOffset > M->MapSize)
    {
        CopyStrSafe(ErrBuf, ErrCap, "tensor data offset out of bounds");
        goto Fail;
    }

    for (i = 0; i < TensorCount; i++)
    {
        AI_GGUF_TENSOR *T = &M->Tensors[i];
        size_t AbsOff = DataOffset + (size_t)T->Offset;
        size_t Cols = (size_t)T->Dims[0];
        size_t Rows = (T->NDims > 1) ? (size_t)T->Dims[1] : 1U;
        size_t Bytes = RowByteSize(T->Type, (uint32_t)Cols) * Rows;

        if (Bytes == 0 || AbsOff > M->MapSize || Bytes > M->MapSize - AbsOff)
        {
            CopyStrSafe(ErrBuf, ErrCap, "unsupported tensor quant type or out-of-bounds tensor");
            goto Fail;
        }
        T->Data = M->MapBase + AbsOff;
    }

    if (M->Info.KvHeadCount == 0)
        M->Info.KvHeadCount = M->Info.HeadCount;
    if (M->Info.ContextLen == 0)
        M->Info.ContextLen = 256U;
    if (M->Info.EmbedDim == 0 || M->Info.LayerCount == 0 || M->Info.HeadCount == 0 ||
        M->Info.VocabSize == 0 || M->Info.FfnDim == 0 ||
        (M->Info.EmbedDim % M->Info.HeadCount) != 0 ||
        (M->Info.HeadCount % M->Info.KvHeadCount) != 0)
    {
        CopyStrSafe(ErrBuf, ErrCap, "invalid or incomplete model hyperparameters");
        goto Fail;
    }

    M->TokEmbd = FindTensor(M, "token_embd.weight");
    M->OutNorm = FindTensor(M, "output_norm.weight");
    M->OutWeight = FindTensor(M, "output.weight");
    if (M->OutWeight == NULL)
        M->OutWeight = M->TokEmbd;
    if (M->TokEmbd == NULL || M->OutNorm == NULL || M->OutWeight == NULL)
    {
        CopyStrSafe(ErrBuf, ErrCap, "missing core GGUF tensors (token_embd/output_norm)");
        goto Fail;
    }

    CopyStrSafe(M->Info.QuantSummary, sizeof(M->Info.QuantSummary), QuantTypeName(M->TokEmbd->Type));

    M->Layers = (AI_GGUF_LAYER *)calloc(M->Info.LayerCount, sizeof(AI_GGUF_LAYER));
    if (M->Layers == NULL)
        goto Fail;

    for (l = 0; l < M->Info.LayerCount; l++)
    {
        char Name[96];
        AI_GGUF_LAYER *L = &M->Layers[l];

        snprintf(Name, sizeof(Name), "blk.%u.attn_norm.weight", l);
        L->AttnNorm = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.attn_q.weight", l);
        L->Wq = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.attn_k.weight", l);
        L->Wk = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.attn_v.weight", l);
        L->Wv = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.attn_output.weight", l);
        L->Wo = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.ffn_norm.weight", l);
        L->FfnNorm = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.ffn_gate.weight", l);
        L->Wgate = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.ffn_up.weight", l);
        L->Wup = FindTensor(M, Name);
        snprintf(Name, sizeof(Name), "blk.%u.ffn_down.weight", l);
        L->Wdown = FindTensor(M, Name);

        if (!L->AttnNorm || !L->Wq || !L->Wk || !L->Wv || !L->Wo ||
            !L->FfnNorm || !L->Wgate || !L->Wup || !L->Wdown)
        {
            CopyStrSafe(ErrBuf, ErrCap, "missing transformer layer tensors");
            goto Fail;
        }
    }

    {
        uint32_t Dim = M->Info.EmbedDim;
        uint32_t KvDim = (Dim * M->Info.KvHeadCount) / M->Info.HeadCount;
        uint32_t Ctx = (M->Info.ContextLen < AI_GGUF_MAX_CTX_CAP) ? M->Info.ContextLen : AI_GGUF_MAX_CTX_CAP;

        M->RuntimeCtx = Ctx;
        M->X = (float *)calloc(Dim, sizeof(float));
        M->Xb = (float *)calloc(Dim, sizeof(float));
        M->Xb2 = (float *)calloc(Dim, sizeof(float));
        M->Hb = (float *)calloc(M->Info.FfnDim, sizeof(float));
        M->Hb2 = (float *)calloc(M->Info.FfnDim, sizeof(float));
        M->Q = (float *)calloc(Dim, sizeof(float));
        M->K = (float *)calloc(KvDim, sizeof(float));
        M->V = (float *)calloc(KvDim, sizeof(float));
        M->Att = (float *)calloc((size_t)M->Info.HeadCount * Ctx, sizeof(float));
        M->Logits = (float *)calloc(M->Info.VocabSize, sizeof(float));
        M->KeyCache = (float *)calloc((size_t)M->Info.LayerCount * Ctx * KvDim, sizeof(float));
        M->ValCache = (float *)calloc((size_t)M->Info.LayerCount * Ctx * KvDim, sizeof(float));

        if (!M->X || !M->Xb || !M->Xb2 || !M->Hb || !M->Hb2 || !M->Q || !M->K || !M->V ||
            !M->Att || !M->Logits || !M->KeyCache || !M->ValCache)
        {
            CopyStrSafe(ErrBuf, ErrCap, "out of memory allocating transformer scratch");
            goto Fail;
        }
    }

    *OutModel = M;
    return 0;

Fail:
    AiGgufFree(M);
    return -1;
}

void AiGgufFree(AI_GGUF_MODEL *M)
{
    uint32_t i;

    if (M == NULL)
        return;

    if (M->Vocab != NULL)
    {
        for (i = 0; i < M->Info.VocabSize; i++)
            free(M->Vocab[i]);
        free(M->Vocab);
    }
    free(M->VocabLen);
    free(M->Tensors);
    free(M->Layers);
    free(M->X);
    free(M->Xb);
    free(M->Xb2);
    free(M->Hb);
    free(M->Hb2);
    free(M->Q);
    free(M->K);
    free(M->V);
    free(M->Att);
    free(M->Logits);
    free(M->KeyCache);
    free(M->ValCache);

#ifdef _WIN32
    if (M->MapBase != NULL)
        UnmapViewOfFile(M->MapBase);
    if (M->MappingHandle != NULL)
        CloseHandle(M->MappingHandle);
    if (M->FileHandle != NULL)
        CloseHandle(M->FileHandle);
#else
    if (M->MapBase != NULL)
        munmap((void *)M->MapBase, M->MapSize);
    if (M->FileFd >= 0)
        close(M->FileFd);
#endif
    free(M);
}

void AiGgufGetInfo(const AI_GGUF_MODEL *Model, AI_GGUF_INFO *OutInfo)
{
    if (Model == NULL || OutInfo == NULL)
        return;
    *OutInfo = Model->Info;
}

/* ==================== Transformer Forward Pass ==================== */

static void ForwardToken(AI_GGUF_MODEL *M, uint32_t Token, uint32_t Pos, int ComputeLogits)
{
    uint32_t Dim = M->Info.EmbedDim;
    uint32_t HeadDim = Dim / M->Info.HeadCount;
    uint32_t KvDim = HeadDim * M->Info.KvHeadCount;
    uint32_t KvMul = M->Info.HeadCount / M->Info.KvHeadCount;
    uint32_t Ctx = M->RuntimeCtx;
    uint32_t l, i, h, t;

    if (Token >= M->Info.VocabSize)
        Token = 0;
    DequantizeRow(M->TokEmbd, Token, M->X, Dim);

    for (l = 0; l < M->Info.LayerCount; l++)
    {
        const AI_GGUF_LAYER *L = &M->Layers[l];
        size_t LayerOff = (size_t)l * Ctx * KvDim;
        float *KPos = M->KeyCache + LayerOff + (size_t)Pos * KvDim;
        float *VPos = M->ValCache + LayerOff + (size_t)Pos * KvDim;

        RmsNorm(M->Xb, M->X, L->AttnNorm, Dim, M->RmsEps);
        MatVec(M->Q, M->Xb, L->Wq, Dim, Dim, M->Info.Avx2Enabled);
        MatVec(KPos, M->Xb, L->Wk, KvDim, Dim, M->Info.Avx2Enabled);
        MatVec(VPos, M->Xb, L->Wv, KvDim, Dim, M->Info.Avx2Enabled);

        /* RoPE positional encoding */
        for (i = 0; i < Dim; i += 2)
        {
            uint32_t HeadIdx = i % HeadDim;
            float Freq = 1.0f / powf(M->RopeTheta, (float)HeadIdx / (float)HeadDim);
            float Angle = (float)Pos * Freq;
            float Fcr = cosf(Angle);
            float Fci = sinf(Angle);
            float Q0 = M->Q[i];
            float Q1 = M->Q[i + 1];

            M->Q[i]     = Q0 * Fcr - Q1 * Fci;
            M->Q[i + 1] = Q0 * Fci + Q1 * Fcr;

            if (i < KvDim)
            {
                float K0 = KPos[i];
                float K1 = KPos[i + 1];
                KPos[i]     = K0 * Fcr - K1 * Fci;
                KPos[i + 1] = K0 * Fci + K1 * Fcr;
            }
        }

        /* Grouped-Query Multi-Head Attention */
        for (h = 0; h < M->Info.HeadCount; h++)
        {
            const float *Qh = M->Q + h * HeadDim;
            float *AttH = M->Att + h * Ctx;
            float *OutH = M->Xb + h * HeadDim;
            uint32_t KvHead = h / KvMul;
            float InvScale = 1.0f / sqrtf((float)HeadDim);

            for (t = 0; t <= Pos; t++)
            {
                const float *Kh = M->KeyCache + LayerOff + (size_t)t * KvDim + KvHead * HeadDim;
                float Score = 0.0f;
                for (i = 0; i < HeadDim; i++)
                    Score += Qh[i] * Kh[i];
                AttH[t] = Score * InvScale;
            }

            Softmax(AttH, Pos + 1U);

            memset(OutH, 0, HeadDim * sizeof(float));
            for (t = 0; t <= Pos; t++)
            {
                const float *Vh = M->ValCache + LayerOff + (size_t)t * KvDim + KvHead * HeadDim;
                float A = AttH[t];
                for (i = 0; i < HeadDim; i++)
                    OutH[i] += A * Vh[i];
            }
        }

        MatVec(M->Xb2, M->Xb, L->Wo, Dim, Dim, M->Info.Avx2Enabled);
        for (i = 0; i < Dim; i++)
            M->X[i] += M->Xb2[i];

        /* SwiGLU FFN */
        RmsNorm(M->Xb, M->X, L->FfnNorm, Dim, M->RmsEps);
        MatVec(M->Hb, M->Xb, L->Wgate, M->Info.FfnDim, Dim, M->Info.Avx2Enabled);
        MatVec(M->Hb2, M->Xb, L->Wup, M->Info.FfnDim, Dim, M->Info.Avx2Enabled);
        for (i = 0; i < M->Info.FfnDim; i++)
        {
            float Val = M->Hb[i];
            Val = Val / (1.0f + expf(-Val));
            M->Hb[i] = Val * M->Hb2[i];
        }
        MatVec(M->Xb, M->Hb, L->Wdown, Dim, M->Info.FfnDim, M->Info.Avx2Enabled);
        for (i = 0; i < Dim; i++)
            M->X[i] += M->Xb[i];
    }

    RmsNorm(M->X, M->X, M->OutNorm, Dim, M->RmsEps);

    if (ComputeLogits)
        MatVec(M->Logits, M->X, M->OutWeight, M->Info.VocabSize, Dim, M->Info.Avx2Enabled);
}

/* ==================== Tokenizer & Generation ==================== */

uint32_t AiGgufEncode(
    const AI_GGUF_MODEL *M,
    const char *TextUtf8,
    int AddBos,
    uint32_t *OutTokens,
    uint32_t MaxTokens)
{
    uint32_t Count = 0;
    size_t Pos = 0;
    size_t TextLen;

    if (M == NULL || TextUtf8 == NULL || OutTokens == NULL || MaxTokens == 0)
        return 0;

    if (AddBos && M->BosToken < M->Info.VocabSize && Count < MaxTokens)
        OutTokens[Count++] = M->BosToken;

    TextLen = strlen(TextUtf8);
    while (Pos < TextLen && Count < MaxTokens)
    {
        uint32_t BestId = 0;
        uint32_t BestLen = 0;
        uint32_t v;

        for (v = 0; v < M->Info.VocabSize; v++)
        {
            uint32_t VLen = M->VocabLen[v];
            if (VLen == 0 || VLen <= BestLen || Pos + VLen > TextLen)
                continue;
            if (memcmp(TextUtf8 + Pos, M->Vocab[v], VLen) == 0)
            {
                BestId = v;
                BestLen = VLen;
            }
        }

        if (BestLen > 0)
        {
            OutTokens[Count++] = BestId;
            Pos += BestLen;
        }
        else
        {
            /* Single-byte fallback token "<0xXX>" if present */
            char ByteTok[8];
            uint32_t Found = 0;
            snprintf(ByteTok, sizeof(ByteTok), "<0x%02X>", (unsigned char)TextUtf8[Pos]);
            for (v = 0; v < M->Info.VocabSize; v++)
            {
                if (strcmp(M->Vocab[v], ByteTok) == 0)
                {
                    OutTokens[Count++] = v;
                    Found = 1;
                    break;
                }
            }
            if (!Found && Count < MaxTokens)
                OutTokens[Count++] = 0;
            Pos++;
        }
    }

    return Count;
}

static void AppendDecodedToken(const AI_GGUF_MODEL *M, uint32_t Tok, char *Out, size_t OutCap, size_t *OutPos)
{
    const char *Piece;
    size_t Len;
    size_t i;

    if (Tok >= M->Info.VocabSize)
        return;
    Piece = M->Vocab[Tok];
    Len = M->VocabLen[Tok];

    /* SentencePiece space marker U+2581 (\xE2\x96\x81) -> ASCII space */
    for (i = 0; i < Len && *OutPos + 1 < OutCap; )
    {
        if (i + 2 < Len &&
            (unsigned char)Piece[i] == 0xE2 &&
            (unsigned char)Piece[i + 1] == 0x96 &&
            (unsigned char)Piece[i + 2] == 0x81)
        {
            Out[(*OutPos)++] = ' ';
            i += 3;
        }
        else
        {
            Out[(*OutPos)++] = Piece[i++];
        }
    }
    Out[*OutPos] = '\0';
}

static uint32_t Argmax(const float *Logits, uint32_t N)
{
    uint32_t Best = 0;
    float BestVal = Logits[0];
    uint32_t i;

    for (i = 1; i < N; i++)
    {
        if (Logits[i] > BestVal)
        {
            BestVal = Logits[i];
            Best = i;
        }
    }
    return Best;
}

int AiGgufGenerate(
    AI_GGUF_MODEL *M,
    const char *PromptUtf8,
    uint32_t MaxNewTokens,
    char *OutUtf8,
    size_t OutCap)
{
    uint32_t *AllTokens = NULL;
    uint32_t AllCap = 4096U;
    uint32_t AllCount;
    uint32_t PromptTokens[AI_GGUF_MAX_CTX_CAP];
    uint32_t PromptCount;
    uint32_t MaxPrompt;
    uint32_t Pos = 0;
    uint32_t NextTok = 0;
    uint32_t Step;
    size_t OutPos = 0;

    if (M == NULL || PromptUtf8 == NULL || OutUtf8 == NULL || OutCap < 2)
        return -1;
    OutUtf8[0] = '\0';

    if (MaxNewTokens == 0)
        MaxNewTokens = 64;
    if (MaxNewTokens >= M->RuntimeCtx)
        MaxNewTokens = M->RuntimeCtx / 2U;
    if (MaxNewTokens == 0)
        MaxNewTokens = 1;

    AllTokens = (uint32_t *)malloc(AllCap * sizeof(uint32_t));
    if (AllTokens == NULL)
        return -1;

    AllCount = AiGgufEncode(M, PromptUtf8, 1, AllTokens, AllCap);
    if (AllCount == 0)
    {
        free(AllTokens);
        return -1;
    }

    MaxPrompt = M->RuntimeCtx - MaxNewTokens;
    if (MaxPrompt < 8U && M->RuntimeCtx > 8U)
        MaxPrompt = M->RuntimeCtx - 1U;

    if (AllCount <= MaxPrompt)
    {
        memcpy(PromptTokens, AllTokens, AllCount * sizeof(uint32_t));
        PromptCount = AllCount;
    }
    else
    {
        /* Sliding tail window: preserve BOS at index 0 and the most recent tokens */
        PromptTokens[0] = AllTokens[0];
        memcpy(PromptTokens + 1, AllTokens + (AllCount - (MaxPrompt - 1U)),
               (MaxPrompt - 1U) * sizeof(uint32_t));
        PromptCount = MaxPrompt;
    }
    free(AllTokens);

    /* Prefill prompt tokens */
    for (Pos = 0; Pos < PromptCount; Pos++)
    {
        int NeedLogits = (Pos + 1U == PromptCount) ? 1 : 0;
        ForwardToken(M, PromptTokens[Pos], Pos, NeedLogits);
    }

    NextTok = Argmax(M->Logits, M->Info.VocabSize);

    /* Autoregressive decode loop */
    for (Step = 0; Step < MaxNewTokens && Pos < M->RuntimeCtx; Step++)
    {
        if (NextTok == M->EosToken)
            break;
        if (NextTok < M->Info.VocabSize &&
            (strcmp(M->Vocab[NextTok], "</s>") == 0 ||
             strcmp(M->Vocab[NextTok], "<|im_end|>") == 0 ||
             strcmp(M->Vocab[NextTok], "<|eot_id|>") == 0))
        {
            break;
        }

        AppendDecodedToken(M, NextTok, OutUtf8, OutCap, &OutPos);
        ForwardToken(M, NextTok, Pos, 1);
        Pos++;
        NextTok = Argmax(M->Logits, M->Info.VocabSize);
    }

    return 0;
}

int AiGgufEmbed(
    AI_GGUF_MODEL *M,
    const char *TextUtf8,
    float *OutVec,
    uint32_t MaxDim,
    uint32_t *OutDim)
{
    uint32_t Tokens[AI_GGUF_MAX_CTX_CAP];
    uint32_t Count;
    uint32_t Dim;
    uint32_t OutN;
    uint32_t Pos;
    uint32_t i;
    float Norm = 0.0f;

    if (M == NULL || TextUtf8 == NULL || OutVec == NULL || OutDim == NULL || MaxDim == 0)
        return -1;
    *OutDim = 0;

    Dim = M->Info.EmbedDim;
    OutN = (Dim < MaxDim) ? Dim : MaxDim;

    Count = AiGgufEncode(M, TextUtf8, 1, Tokens, (M->RuntimeCtx < 64U) ? M->RuntimeCtx : 64U);
    if (Count == 0)
        return -1;

    memset(OutVec, 0, OutN * sizeof(float));
    for (Pos = 0; Pos < Count; Pos++)
    {
        ForwardToken(M, Tokens[Pos], Pos, 0);
        for (i = 0; i < OutN; i++)
            OutVec[i] += M->X[i];
    }

    for (i = 0; i < OutN; i++)
    {
        OutVec[i] /= (float)Count;
        Norm += OutVec[i] * OutVec[i];
    }
    Norm = sqrtf(Norm);
    if (Norm > 1e-8f)
    {
        for (i = 0; i < OutN; i++)
            OutVec[i] /= Norm;
    }

    *OutDim = OutN;
    return 0;
}
