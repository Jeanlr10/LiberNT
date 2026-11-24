#!/usr/bin/env python3
"""
Generate a valid, spec-compliant GGUF v3 Llama-architecture model file for
testing LiberNT's built-in on-device GGUF inference engine (F32/F16/Q8_0/Q4_0).

Usage:
  python3 make_test_gguf.py --out tiny_libernt.gguf --quant q8_0 --mode chat
  python3 make_test_gguf.py --out tiny_tool.gguf --quant f32 --mode tool
"""

import argparse
import math
import struct

GGUF_MAGIC = 0x46554747
GGUF_VERSION = 3

GGUF_TYPE_UINT32 = 4
GGUF_TYPE_FLOAT32 = 6
GGUF_TYPE_STRING = 8
GGUF_TYPE_ARRAY = 9

GGML_TYPE_F32 = 0
GGML_TYPE_F16 = 1
GGML_TYPE_Q4_0 = 2
GGML_TYPE_Q8_0 = 8


def float_to_f16_bits(val: float) -> int:
    return struct.unpack("<H", struct.pack("<e", float(val)))[0]


def pack_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<Q", len(b)) + b


def pack_kv_str(key: str, val: str) -> bytes:
    return pack_string(key) + struct.pack("<I", GGUF_TYPE_STRING) + pack_string(val)


def pack_kv_u32(key: str, val: int) -> bytes:
    return pack_string(key) + struct.pack("<II", GGUF_TYPE_UINT32, val)


def pack_kv_f32(key: str, val: float) -> bytes:
    return pack_string(key) + struct.pack("<If", GGUF_TYPE_FLOAT32, val)


def pack_kv_str_array(key: str, items: list[str]) -> bytes:
    out = bytearray()
    out += pack_string(key)
    out += struct.pack("<IIQ", GGUF_TYPE_ARRAY, GGUF_TYPE_STRING, len(items))
    for s in items:
        out += pack_string(s)
    return bytes(out)


def encode_matrix(rows: list[list[float]], qtype: int) -> bytes:
    out = bytearray()
    for row in rows:
        cols = len(row)
        if qtype == GGML_TYPE_F32:
            out += struct.pack(f"<{cols}f", *row)
        elif qtype == GGML_TYPE_F16:
            out += struct.pack(f"<{cols}H", *(float_to_f16_bits(v) for v in row))
        elif qtype == GGML_TYPE_Q8_0:
            assert cols % 32 == 0
            for off in range(0, cols, 32):
                blk = row[off : off + 32]
                amax = max(abs(v) for v in blk)
                scale = amax / 127.0 if amax > 0 else 1.0
                d_bits = float_to_f16_bits(scale)
                out += struct.pack("<H", d_bits)
                for v in blk:
                    q = int(round(v / scale)) if amax > 0 else 0
                    q = max(-127, min(127, q))
                    out += struct.pack("<b", q)
        elif qtype == GGML_TYPE_Q4_0:
            assert cols % 32 == 0
            for off in range(0, cols, 32):
                blk = row[off : off + 32]
                amax = max(abs(v) for v in blk)
                scale = amax / 7.0 if amax > 0 else 1.0
                d_bits = float_to_f16_bits(scale)
                out += struct.pack("<H", d_bits)
                qs = []
                for v in blk:
                    q = int(round(v / scale)) + 8 if amax > 0 else 8
                    q = max(0, min(15, q))
                    qs.append(q)
                for j in range(16):
                    out.append((qs[j] & 0xF) | ((qs[j + 16] & 0xF) << 4))
        else:
            raise ValueError(f"unsupported quant {qtype}")
    return bytes(out)


def build_gguf(out_path: str, quant_name: str, mode: str):
    qmap = {
        "f32": GGML_TYPE_F32,
        "f16": GGML_TYPE_F16,
        "q8_0": GGML_TYPE_Q8_0,
        "q4_0": GGML_TYPE_Q4_0,
    }
    qtype = qmap[quant_name.lower()]

    dim = 32
    ffn = 64
    n_layers = 2
    n_heads = 4
    n_kv_heads = 2
    kv_dim = (dim * n_kv_heads) // n_heads
    n_ctx = 256
    align = 32

    if mode == "tool":
        # Autoregressive chain emits a real <tool_call> for system_status
        gen_tokens = [
            "<tool_call>",
            "{\"name\":\"system_status\",",
            "\"arguments\":{}}",
            "</tool_call>",
        ]
        model_name = f"libernt-local-tool-{quant_name}"
    else:
        gen_tokens = [
            "LiberNT ",
            "local ",
            "on-device ",
            "GGUF ",
            "transformer ",
            "active.",
        ]
        model_name = f"libernt-local-chat-{quant_name}"

    vocab = [
        "<unk>",                  # 0
        "<s>",                    # 1 (BOS)
        "</s>",                   # 2 (EOS)
        "<|im_start|>",           # 3
        "<|im_end|>",             # 4
        "assistant\n",            # 5 (trigger token at end of ChatML prompt)
        "system\n",               # 6
        "user\n",                 # 7
        "tool\n",                 # 8
        *gen_tokens,              # 9 .. 9+len(gen_tokens)-1
        "LiberNT",
        "taxes",
        "quarterly",
        "receipts",
        "kernel",
        "drivers",
        "folder",
        "documents",
        "summary",
        "project",
        "files",
        " ",
    ]
    while len(vocab) < dim:
        vocab.append(f"<tok_{len(vocab)}>")
    vocab = vocab[:dim]
    n_vocab = len(vocab)

    # Orthogonal unit token embeddings: token t activates dimension t
    tok_embd = [[1.0 if j == i else 0.0 for j in range(dim)] for i in range(n_vocab)]

    # Build output.weight so the transformer deterministically steps through gen_tokens -> EOS:
    # token 5 ("assistant\n") -> token 9 -> token 10 -> ... -> token 2 ("</s>")
    out_w = [[0.0 for _ in range(dim)] for _ in range(n_vocab)]
    chain = [5] + [9 + k for k in range(len(gen_tokens))] + [2]
    for a, b in zip(chain[:-1], chain[1:]):
        out_w[b][a] = 5.0
    # Default fallback transitions to EOS
    for t in range(n_vocab):
        if t not in chain[:-1]:
            out_w[2][t] = 2.0

    ones_dim = [[1.0 for _ in range(dim)]]
    # Small non-zero weights so RMSNorm, RoPE, GQA, and SwiGLU execute real math
    small_q = [[0.08 if i == j else 0.0 for j in range(dim)] for i in range(dim)]
    small_k = [[0.08 if i == j else 0.0 for j in range(dim)] for i in range(kv_dim)]
    small_v = [[0.05 if i == j else 0.0 for j in range(dim)] for i in range(kv_dim)]
    small_o = [[0.04 if i == j else 0.0 for j in range(dim)] for i in range(dim)]
    small_gate = [[0.05 if (j == i % dim) else 0.0 for j in range(dim)] for i in range(ffn)]
    small_up = [[0.05 if (j == i % dim) else 0.0 for j in range(dim)] for i in range(ffn)]
    small_down = [[0.02 if (i == j % dim) else 0.0 for j in range(ffn)] for i in range(dim)]

    tensors = []
    tensors.append(("token_embd.weight", [dim, n_vocab], qtype, encode_matrix(tok_embd, qtype)))
    for l in range(n_layers):
        tensors.append((f"blk.{l}.attn_norm.weight", [dim], GGML_TYPE_F32, encode_matrix(ones_dim, GGML_TYPE_F32)))
        tensors.append((f"blk.{l}.attn_q.weight", [dim, dim], qtype, encode_matrix(small_q, qtype)))
        tensors.append((f"blk.{l}.attn_k.weight", [dim, kv_dim], qtype, encode_matrix(small_k, qtype)))
        tensors.append((f"blk.{l}.attn_v.weight", [dim, kv_dim], qtype, encode_matrix(small_v, qtype)))
        tensors.append((f"blk.{l}.attn_output.weight", [dim, dim], qtype, encode_matrix(small_o, qtype)))
        tensors.append((f"blk.{l}.ffn_norm.weight", [dim], GGML_TYPE_F32, encode_matrix(ones_dim, GGML_TYPE_F32)))
        tensors.append((f"blk.{l}.ffn_gate.weight", [dim, ffn], qtype, encode_matrix(small_gate, qtype)))
        tensors.append((f"blk.{l}.ffn_up.weight", [dim, ffn], qtype, encode_matrix(small_up, qtype)))
        tensors.append((f"blk.{l}.ffn_down.weight", [ffn, dim], qtype, encode_matrix(small_down, qtype)))
    tensors.append(("output_norm.weight", [dim], GGML_TYPE_F32, encode_matrix(ones_dim, GGML_TYPE_F32)))
    tensors.append(("output.weight", [dim, n_vocab], qtype, encode_matrix(out_w, qtype)))

    kvs = [
        pack_kv_str("general.architecture", "llama"),
        pack_kv_str("general.name", model_name),
        pack_kv_u32("general.alignment", align),
        pack_kv_u32("llama.block_count", n_layers),
        pack_kv_u32("llama.embedding_length", dim),
        pack_kv_u32("llama.feed_forward_length", ffn),
        pack_kv_u32("llama.attention.head_count", n_heads),
        pack_kv_u32("llama.attention.head_count_kv", n_kv_heads),
        pack_kv_u32("llama.context_length", n_ctx),
        pack_kv_f32("llama.attention.layer_norm_rms_epsilon", 1e-5),
        pack_kv_f32("llama.rope.freq_base", 10000.0),
        pack_kv_u32("tokenizer.ggml.bos_token_id", 1),
        pack_kv_u32("tokenizer.ggml.eos_token_id", 2),
        pack_kv_str_array("tokenizer.ggml.tokens", vocab),
    ]

    blob = bytearray()
    blob += struct.pack("<IIQQ", GGUF_MAGIC, GGUF_VERSION, len(tensors), len(kvs))
    for kv in kvs:
        blob += kv

    data_buf = bytearray()
    for name, dims, ttype, raw in tensors:
        while len(data_buf) % align != 0:
            data_buf.append(0)
        offset = len(data_buf)
        data_buf += raw

        blob += pack_string(name)
        blob += struct.pack("<I", len(dims))
        for d in dims:
            blob += struct.pack("<Q", d)
        blob += struct.pack("<IQ", ttype, offset)

    while len(blob) % align != 0:
        blob.append(0)
    blob += data_buf

    with open(out_path, "wb") as fp:
        fp.write(blob)

    print(f"Wrote {out_path} ({len(blob)} bytes, quant={quant_name}, mode={mode}, tensors={len(tensors)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--quant", default="q8_0", choices=["f32", "f16", "q8_0", "q4_0"])
    ap.add_argument("--mode", default="chat", choices=["chat", "tool"])
    args = ap.parse_args()
    build_gguf(args.out, args.quant, args.mode)


if __name__ == "__main__":
    main()
