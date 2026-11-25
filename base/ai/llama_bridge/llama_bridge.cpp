/*
 * PROJECT:     LiberNT AI Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ABI-stable C bridge exporting llama.cpp inference and embeddings from llama.dll
 * COPYRIGHT:   Copyright 2026 LiberNT Contributors
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "llama.h"

#ifdef _WIN32
#define AI_LLAMA_EXPORT extern "C" __declspec(dllexport)
#else
#define AI_LLAMA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct AiLlamaHandle
{
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    const llama_vocab *vocab = nullptr;
    uint32_t n_ctx = 2048;
    uint32_t n_threads = 4;
    std::mutex lock;
};

static std::once_flag g_backend_once;

static void QuietLlamaLog(enum ggml_log_level level, const char *text, void *user_data)
{
    (void)level;
    (void)text;
    (void)user_data;
}

static void CopyStrSafe(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    size_t len = std::strlen(src);
    if (len >= cap)
        len = cap - 1;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

static std::string FormatChatPrompt(
    const llama_model *model,
    const char **roles,
    const char **contents,
    uint32_t msg_count)
{
    const char *tmpl = llama_model_chat_template(model, nullptr);
    if (tmpl && tmpl[0] != '\0')
    {
        std::vector<llama_chat_message> msgs(msg_count);
        size_t est = 256;
        for (uint32_t i = 0; i < msg_count; i++)
        {
            msgs[i].role = roles[i] ? roles[i] : "user";
            msgs[i].content = contents[i] ? contents[i] : "";
            est += std::strlen(msgs[i].content) * 2 + 32;
        }
        std::vector<char> buf(est);
        int32_t n = llama_chat_apply_template(
            tmpl, msgs.data(), msgs.size(), true, buf.data(), (int32_t)buf.size());
        if (n > (int32_t)buf.size())
        {
            buf.resize((size_t)n + 1);
            n = llama_chat_apply_template(
                tmpl, msgs.data(), msgs.size(), true, buf.data(), (int32_t)buf.size());
        }
        if (n > 0)
            return std::string(buf.data(), (size_t)n);
    }

    std::string out;
    for (uint32_t i = 0; i < msg_count; i++)
    {
        out += "<|im_start|>";
        out += (roles[i] ? roles[i] : "user");
        out += "\n";
        if (contents[i])
            out += contents[i];
        out += "<|im_end|>\n";
    }
    out += "<|im_start|>assistant\n";
    return out;
}

AI_LLAMA_EXPORT void *AiLlamaOpen(
    const char *model_path,
    uint32_t context_len,
    uint32_t threads,
    char *err_buf,
    size_t err_cap)
{
    if (err_buf && err_cap > 0)
        err_buf[0] = '\0';
    if (!model_path || model_path[0] == '\0')
    {
        CopyStrSafe(err_buf, err_cap, "empty model path");
        return nullptr;
    }

    std::call_once(g_backend_once, []() {
        llama_log_set(QuietLlamaLog, nullptr);
        llama_backend_init();
    });

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0;

    llama_model *model = llama_model_load_from_file(model_path, mparams);
    if (!model)
    {
        CopyStrSafe(err_buf, err_cap, "llama_model_load_from_file failed");
        return nullptr;
    }

    uint32_t use_ctx = context_len ? context_len : 2048U;
    uint32_t use_threads = (threads >= 1 && threads <= 64) ? threads : 4U;

    AiLlamaHandle *h = new AiLlamaHandle();
    h->model = model;
    h->vocab = llama_model_get_vocab(model);
    h->n_ctx = use_ctx;
    h->n_threads = use_threads;

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = use_ctx;
    cparams.n_batch = 512;
    cparams.n_threads = (int32_t)use_threads;
    cparams.n_threads_batch = (int32_t)use_threads;
    cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    cparams.embeddings = false;
    cparams.no_perf = true;

    llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx)
    {
        delete h;
        llama_model_free(model);
        CopyStrSafe(err_buf, err_cap, "llama_init_from_model failed");
        return nullptr;
    }
    h->ctx = ctx;
    return h;
}

AI_LLAMA_EXPORT void AiLlamaClose(void *handle)
{
    AiLlamaHandle *h = static_cast<AiLlamaHandle *>(handle);
    if (!h)
        return;
    if (h->ctx)
        llama_free(h->ctx);
    if (h->model)
        llama_model_free(h->model);
    delete h;
}

AI_LLAMA_EXPORT void AiLlamaGetInfo(
    void *handle,
    char *model_desc_out,
    size_t desc_cap,
    char *backend_out,
    size_t backend_cap)
{
    AiLlamaHandle *h = static_cast<AiLlamaHandle *>(handle);
    if (!h || !h->model)
        return;

    char desc[128] = {0};
    llama_model_desc(h->model, desc, sizeof(desc));
    CopyStrSafe(model_desc_out, desc_cap, desc);

    if (backend_out && backend_cap > 0)
    {
        std::snprintf(
            backend_out,
            backend_cap,
            "llama.cpp (CPU/AVX2, %dL, d=%d, ctx=%u, t=%u)",
            llama_model_n_layer(h->model),
            llama_model_n_embd(h->model),
            h->n_ctx,
            h->n_threads);
        backend_out[backend_cap - 1] = '\0';
    }
}

AI_LLAMA_EXPORT int AiLlamaChat(
    void *handle,
    const char **roles,
    const char **contents,
    uint32_t msg_count,
    uint32_t max_tokens,
    char *out_utf8,
    size_t out_cap,
    char *err_buf,
    size_t err_cap)
{
    AiLlamaHandle *h = static_cast<AiLlamaHandle *>(handle);
    if (out_utf8 && out_cap > 0)
        out_utf8[0] = '\0';
    if (err_buf && err_cap > 0)
        err_buf[0] = '\0';
    if (!h || !h->ctx || !h->vocab || !roles || !contents || msg_count == 0)
    {
        CopyStrSafe(err_buf, err_cap, "invalid llama chat args");
        return -1;
    }

    std::lock_guard<std::mutex> guard(h->lock);

    std::string prompt = FormatChatPrompt(h->model, roles, contents, msg_count);

    int32_t n_needed = -llama_tokenize(
        h->vocab, prompt.c_str(), (int32_t)prompt.size(), nullptr, 0, true, true);
    if (n_needed <= 0)
    {
        CopyStrSafe(err_buf, err_cap, "llama_tokenize failed");
        return -1;
    }

    std::vector<llama_token> tokens((size_t)n_needed);
    int32_t n_tok = llama_tokenize(
        h->vocab, prompt.c_str(), (int32_t)prompt.size(), tokens.data(), n_needed, true, true);
    if (n_tok <= 0)
    {
        CopyStrSafe(err_buf, err_cap, "llama_tokenize returned 0");
        return -1;
    }
    tokens.resize((size_t)n_tok);

    uint32_t max_gen = max_tokens ? max_tokens : 128U;
    if (max_gen > h->n_ctx / 2U)
        max_gen = h->n_ctx / 2U;

    /* Sliding tail window if prompt exceeds context */
    uint32_t max_prompt = h->n_ctx - max_gen;
    if (tokens.size() > max_prompt && max_prompt > 8U)
    {
        llama_token bos = tokens[0];
        std::vector<llama_token> trimmed;
        trimmed.reserve(max_prompt);
        trimmed.push_back(bos);
        trimmed.insert(trimmed.end(), tokens.end() - (max_prompt - 1U), tokens.end());
        tokens.swap(trimmed);
    }

    llama_set_embeddings(h->ctx, false);
    llama_memory_clear(llama_get_memory(h->ctx), true);

    llama_batch_ext *batch = llama_batch_ext_init(h->ctx);
    if (!batch)
    {
        CopyStrSafe(err_buf, err_cap, "llama_batch_ext_init failed");
        return -1;
    }

    /* Prefill in chunks of 512 using explicit positions */
    llama_pos cur_pos = 0;
    for (size_t off = 0; off < tokens.size(); off += 512)
    {
        size_t chunk = std::min<size_t>(512, tokens.size() - off);
        bool is_last_chunk = (off + chunk >= tokens.size());
        llama_batch_ext_clear(batch);
        for (size_t i = 0; i < chunk; i++)
        {
            int32_t idx = llama_batch_ext_add_token(batch, 0, tokens[off + i]);
            llama_pos pos = cur_pos++;
            llama_batch_ext_set_pos(batch, idx, &pos);
        }
        if (is_last_chunk)
            llama_batch_ext_set_output_logits(batch, (int32_t)(chunk - 1), true);
        if (llama_process(h->ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0)
        {
            llama_batch_ext_free(batch);
            CopyStrSafe(err_buf, err_cap, "llama_process prefill failed");
            return -1;
        }
    }

    llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    llama_sampler *smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    std::string reply;
    for (uint32_t step = 0; step < max_gen; step++)
    {
        llama_token tok = llama_sampler_sample(smpl, h->ctx, -1);
        if (llama_vocab_is_eog(h->vocab, tok))
            break;

        char piece[256];
        int32_t plen = llama_token_to_piece(h->vocab, tok, piece, (int32_t)sizeof(piece), 0, true);
        if (plen > 0)
        {
            reply.append(piece, (size_t)plen);
            if (reply.size() >= out_cap - 1)
                break;
        }

        llama_batch_ext_clear(batch);
        int32_t idx = llama_batch_ext_add_token(batch, 0, tok);
        llama_pos pos = cur_pos++;
        llama_batch_ext_set_pos(batch, idx, &pos);
        llama_batch_ext_set_output_logits(batch, idx, true);
        if (llama_process(h->ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0)
            break;
    }

    llama_sampler_free(smpl);
    llama_batch_ext_free(batch);
    CopyStrSafe(out_utf8, out_cap, reply.c_str());
    return 0;
}

AI_LLAMA_EXPORT int AiLlamaEmbed(
    void *handle,
    const char *text_utf8,
    float *out_vec,
    uint32_t max_dim,
    uint32_t *out_dim)
{
    AiLlamaHandle *h = static_cast<AiLlamaHandle *>(handle);
    if (out_dim)
        *out_dim = 0;
    if (!h || !h->model || !h->vocab || !text_utf8 || !out_vec || !out_dim || max_dim == 0)
        return -1;

    std::lock_guard<std::mutex> guard(h->lock);

    llama_context_params eparams = llama_context_default_params();
    eparams.n_ctx = 512;
    eparams.n_batch = 512;
    eparams.n_threads = (int32_t)h->n_threads;
    eparams.n_threads_batch = (int32_t)h->n_threads;
    eparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    eparams.embeddings = true;
    eparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
    eparams.no_perf = true;

    llama_context *ectx = llama_init_from_model(h->model, eparams);
    if (!ectx)
        return -1;

    int32_t n_needed = -llama_tokenize(
        h->vocab, text_utf8, (int32_t)std::strlen(text_utf8), nullptr, 0, true, true);
    if (n_needed <= 0)
    {
        llama_free(ectx);
        return -1;
    }
    std::vector<llama_token> tokens((size_t)n_needed);
    int32_t n_tok = llama_tokenize(
        h->vocab, text_utf8, (int32_t)std::strlen(text_utf8), tokens.data(), n_needed, true, true);
    if (n_tok <= 0)
    {
        llama_free(ectx);
        return -1;
    }
    if (n_tok > 500)
        n_tok = 500;

    llama_batch_ext *ebatch = llama_batch_ext_init(ectx);
    if (!ebatch)
    {
        llama_free(ectx);
        return -1;
    }
    for (int32_t i = 0; i < n_tok; i++)
    {
        int32_t idx = llama_batch_ext_add_token(ebatch, 0, tokens[(size_t)i]);
        llama_pos pos = (llama_pos)i;
        llama_batch_ext_set_pos(ebatch, idx, &pos);
        llama_batch_ext_set_output_logits(ebatch, idx, true);
    }
    int rc = llama_process(ectx, LLAMA_PROCESS_TYPE_DECODE, ebatch);
    llama_batch_ext_free(ebatch);
    if (rc != 0)
    {
        llama_free(ectx);
        return -1;
    }

    const float *emb = llama_get_embeddings_seq(ectx, 0);
    if (!emb)
        emb = llama_get_embeddings_ith(ectx, -1);
    if (!emb)
        emb = llama_get_embeddings(ectx);
    if (!emb)
    {
        llama_free(ectx);
        return -1;
    }

    int32_t n_embd = llama_model_n_embd(h->model);
    uint32_t dim = ((uint32_t)n_embd < max_dim) ? (uint32_t)n_embd : max_dim;
    float norm = 0.0f;
    for (uint32_t i = 0; i < dim; i++)
    {
        out_vec[i] = emb[i];
        norm += emb[i] * emb[i];
    }
    norm = std::sqrt(norm);
    if (norm > 1e-8f)
    {
        for (uint32_t i = 0; i < dim; i++)
            out_vec[i] /= norm;
    }

    *out_dim = dim;
    llama_free(ectx);
    return 0;
}
