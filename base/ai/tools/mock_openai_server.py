#!/usr/bin/env python3
"""
Deterministic OpenAI-compatible dev/test server for LiberNT AI Milestone 2.

Implements:
  - GET  /health
  - POST /v1/chat/completions  (tool calling + folder auto-summarization)
  - POST /v1/embeddings        (deterministic 16-dim normalized trigram vectors)

Usage:
  python3 mock_openai_server.py --host 127.0.0.1 --port 11435
Inside QEMU with `-netdev user,id=net0`, LiberNT reaches this at:
  http://10.0.2.2:11435/v1
"""

import argparse
import hashlib
import http.server
import json
import math
import re
import socketserver


def embed_text(text: str, dim: int = 16):
    vec = [0.0] * dim
    norm = re.sub(r"[^a-z0-9]+", " ", text.lower()).strip()
    if not norm:
        vec[0] = 1.0
        return vec
    tokens = norm.split()
    for tok in tokens:
        for i in range(max(1, len(tok) - 2)):
            gram = tok[i : i + 3]
            h = int(hashlib.md5(gram.encode("utf-8")).hexdigest(), 16)
            vec[h % dim] += 1.0 if ((h >> 8) & 1) else -1.0
    mag = math.sqrt(sum(v * v for v in vec)) or 1.0
    return [round(v / mag, 6) for v in vec]


def handle_chat(body: dict):
    messages = body.get("messages") or []
    tools = body.get("tools") or []
    tool_names = {t.get("function", {}).get("name") for t in tools if isinstance(t, dict)}

    user_msgs = [m.get("content") or "" for m in messages if m.get("role") == "user"]
    tool_msgs = [m for m in messages if m.get("role") == "tool"]
    prompt = user_msgs[-1] if user_msgs else ""
    prompt_low = prompt.lower()

    # 1. Folder auto-description prompt (no tools passed)
    if "summarize the purpose of folder" in prompt_low:
        files = re.findall(r"\[(?:FILE|DIR)\]\s+([^\s(]+)", prompt)
        if files:
            sample = ", ".join(files[:4])
            desc = f"Workspace folder containing {sample}"
            tags = ",".join(re.sub(r"[^a-z0-9]+", "", f.split(".")[0].lower()) for f in files[:3] if f)
        else:
            desc = "General user documents folder"
            tags = "documents"
        reply = json.dumps({"description": desc[:135], "tags": tags[:64]})
        return {"choices": [{"index": 0, "message": {"role": "assistant", "content": reply}}]}

    # 2. Second turn after tool execution -> synthesize final grounded answer
    if tool_msgs:
        last_tool = tool_msgs[-1].get("content") or ""
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": f"Completed using LiberNT tool output: {last_tool[:400]}",
                    },
                }
            ]
        }

    # 3. First turn: Tier 2 write_file request
    if "create file" in prompt_low or "write file" in prompt_low:
        m_path = re.search(r"([A-Za-z]:\\[^\s\"']+)", prompt)
        target_path = m_path.group(1) if m_path else r"C:\Users\Administrator\Documents\ai_note.txt"
        args = json.dumps({"path": target_path, "content": "Created by LiberNT AI agent (Tier 2 approved).\r\n"})
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_write_1",
                                "type": "function",
                                "function": {"name": "write_file", "arguments": args},
                            }
                        ],
                    },
                }
            ]
        }

    # 4. First turn: Tier 2 run_command request
    if "run command" in prompt_low:
        cmd = prompt.split("run command", 1)[1].strip() or "echo LiberNT AI command ok"
        args = json.dumps({"command": cmd})
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_cmd_1",
                                "type": "function",
                                "function": {"name": "run_command", "arguments": args},
                            }
                        ],
                    },
                }
            ]
        }

    # 4b. First turn: Desktop window orchestration tools
    if "list_windows" in tool_names and "list windows" in prompt_low:
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_listwin_1",
                                "type": "function",
                                "function": {"name": "list_windows", "arguments": "{}"},
                            }
                        ],
                    },
                }
            ]
        }

    if "inspect_window" in tool_names and "inspect" in prompt_low:
        target = prompt.split("inspect", 1)[1].strip() or "Notepad"
        args = json.dumps({"target": target})
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_inspect_1",
                                "type": "function",
                                "function": {"name": "inspect_window", "arguments": args},
                            }
                        ],
                    },
                }
            ]
        }

    if "arrange_windows" in tool_names and any(k in prompt_low for k in ("tile", "split", "undo layout", "arrange")):
        if "undo" in prompt_low:
            args = json.dumps({"layout": "undo"})
        elif "horizontal" in prompt_low:
            args = json.dumps({"layout": "split_horizontal", "targets": "Notepad,Command Prompt"})
        else:
            targets = "Notepad,Command Prompt" if ("notepad" in prompt_low and "cmd" in prompt_low) else ""
            args = json.dumps({"layout": "split_vertical", "targets": targets})
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_arrange_1",
                                "type": "function",
                                "function": {"name": "arrange_windows", "arguments": args},
                            }
                        ],
                    },
                }
            ]
        }

    if "close_window" in tool_names and "close window" in prompt_low:
        target = prompt.split("close window", 1)[1].strip() or "Notepad"
        args = json.dumps({"target": target})
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_close_1",
                                "type": "function",
                                "function": {"name": "close_window", "arguments": args},
                            }
                        ],
                    },
                }
            ]
        }

    # 5. First turn: search_files for file/note/tax/fiscal questions
    if "search_files" in tool_names and any(
        k in prompt_low for k in ("find", "search", "tax", "fiscal", "plan", "note", "where")
    ):
        words = [w for w in re.findall(r"[A-Za-z0-9_]+", prompt) if len(w) >= 3]
        stop = {"find", "search", "where", "about", "what", "the", "are", "for", "with", "show", "files"}
        q_words = [w for w in words if w.lower() not in stop]
        query = " ".join(q_words[:3]) if q_words else "fiscal"
        args = json.dumps({"query": query, "max_results": 4})
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_search_1",
                                "type": "function",
                                "function": {"name": "search_files", "arguments": args},
                            }
                        ],
                    },
                }
            ]
        }

    # 6. First turn: system status
    if "system_status" in tool_names and "status" in prompt_low:
        return {
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [
                            {
                                "id": "call_status_1",
                                "type": "function",
                                "function": {"name": "system_status", "arguments": "{}"},
                            }
                        ],
                    },
                }
            ]
        }

    return {
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": f"LiberNT mock assistant received: {prompt[:200]}",
                },
            }
        ]
    }


class MockOpenAiHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[mock_openai] {self.address_string()} - {fmt % args}", flush=True)

    def _send_json(self, code: int, obj: dict):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.rstrip("/") in ("", "/health", "/v1/models"):
            self._send_json(200, {"status": "ok", "models": ["mock-libernt"]})
            return
        self._send_json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length > 0 else b"{}"
        try:
            body = json.loads(raw.decode("utf-8"))
        except Exception as exc:
            self._send_json(400, {"error": {"message": f"invalid json: {exc}"}})
            return

        if self.path.endswith("/chat/completions"):
            self._send_json(200, handle_chat(body))
            return

        if self.path.endswith("/embeddings"):
            inp = body.get("input", "")
            if isinstance(inp, list):
                inp = " ".join(str(x) for x in inp)
            vec = embed_text(str(inp))
            self._send_json(200, {"data": [{"index": 0, "embedding": vec}]})
            return

        self._send_json(404, {"error": {"message": f"unknown endpoint {self.path}"}})


class ReusableTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=11435)
    args = ap.parse_args()

    with ReusableTCPServer((args.host, args.port), MockOpenAiHandler) as srv:
        print(f"MOCK_OPENAI_READY http://{args.host}:{args.port}/v1", flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    main()
