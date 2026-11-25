# LiberNT AI Subsystem (`base/ai/`)

A modular, opt-out (`-DENABLE_LIBERNT_AI=OFF`) AI operating-system layer built natively on NT primitives (Session 0 SCM service, ALPC IPC, NTFS Alternate Data Streams, SQLite FTS5 + cosine vector store, WinHTTP OpenAI-compatible remote inference, optional on-device `llama.cpp` via `llama.dll`, and a Session 1 Win32/DWM shell overlay).

## Architecture

```text
Session 1 (Interactive User Desktop)
  Explorer (CFSFolder.cpp)   <-- reads :LiberNT.Description ADS (Comments col & InfoTip)
  aishell.exe                <-- Win+Space / Ctrl+Shift+Space / tray bar, Folder Description editor,
        |                        and desktop ALPC server (\RPC Control\LiberNtAiDesktopPort)
  ai.exe (CLI)               <-- status, index, search, describe, ask, do, config
        |
   libai.dll                 <-- ALPC client SDK + NTFS ADS helpers
        |  (\RPC Control\LiberNtAiPort)
Session 0 (NT SCM Service — NT AUTHORITY\SYSTEM)
  aisvc.exe
    ├── indexer.c            <-- live ReadDirectoryChangesW NTFS watcher + incremental crawler
    ├── ai_store.c           <-- SQLite FTS5 + folder_descriptions + embeddings + audit_log
    ├── policy.c             <-- Tier 0/1/2 policy gate + fail-closed shell command classifier
    ├── agent.c              <-- multi-turn tool-calling OS operator & folder auto-summarizer
    ├── tools_win32.c        <-- hardened Win32/Job-Object cmd.exe runner + Session 1 ALPC bridge
    ├── provider_openai.c    <-- default remote OpenAI-compatible provider over winhttp.dll
    └── provider_local.c     <-- optional air-gapped local GGUF provider:
                                 1) llama.dll (upstream llama.cpp + ggml AVX2/FMA/F16C)
                                 2) gguf_engine.c (built-in zero-dependency C11 fallback)
```

## Build Options

- `-DENABLE_LIBERNT_AI=ON` (default `ON`): builds `libai.dll`, `aisvc.exe`, `ai.exe`, and `aishell.exe`.
- `-DENABLE_LIBERNT_AI_LLAMACPP=ON` (default `ON`): when `submodules/llama.cpp` is checked out (via `scripts/feeds update llama.cpp`), cross-compiles a self-contained stripped `llama.dll` (importing only `ucrtbase.dll`, `kernel32.dll`, `msvcrt.dll`) via `submodules/llama-cpp.cmake`. If the feed is not checked out, `aisvc.exe` automatically uses the built-in C11 GGUF engine for local mode.

## CLI & Shell Usage

```bat
:: Health and semantic NTFS index
ai status
ai index
ai search "quarterly budget"

:: User-visible NTFS folder descriptions (persisted in :LiberNT.Description ADS + SQLite)
ai describe C:\Users\Administrator\Documents\ProjectApollo
ai describe C:\Users\Administrator\Documents\ProjectApollo "Flight telemetry and notes"
ai describe --auto C:\Users\Administrator\Documents\ProjectApollo
ai describe --clear C:\Users\Administrator\Documents\ProjectApollo

:: Read-only Ask mode (Tier 0 tools + read-only shell inspection)
ai ask "What processes and services are running?"

:: Action mode (Tier 0/1 auto-allowed; Tier 2 requires explicit approval)
ai do "Tile Notepad and Command Prompt side by side"
ai do --yes "Create C:\Users\Administrator\Documents\notes.txt"

:: Runtime provider switching (defaults to remote OpenAI-compatible endpoint)
ai config
ai config set --provider remote --endpoint http://10.0.2.2:11434/v1 --model llama3.2
ai config set --provider local --model-path D:\SmolLM2-135M-Instruct-Q4_K_M.gguf --max-tokens 128
```

- **Interactive Command Bar (`aishell.exe`):** click the taskbar AI tray icon or press `Win+Space` / `Ctrl+Alt+Space` / `Ctrl+Shift+Space`.
- **Explorer Integration (`CFSFolder.cpp`):** right-click any folder -> **Edit AI Description...**; descriptions render natively in the **Comments** column (Details view) and hover **InfoTip**.

## Security & Capability Tiers

All agent tool calls are mediated by `aisvc.exe` (`policy.c`) and recorded in SQLite `audit_log`:

| Tier | Policy | Tools |
| --- | --- | --- |
| **Tier 0** (Read-Only) | Allowed in Ask & Do | `search_files`, `read_file_snippet`, `list_directory`, `get_folder_description`, `system_status`, `list_windows`, `inspect_window`, `get_clipboard`, and fail-closed read-only `run_command` (`tasklist`, `sc query`, `reg query`, `ipconfig`, `netstat`, `systeminfo`, `driverquery`, `dir`, `whoami`, `hostname`, `ver`, `where`, `findstr`, `netsh ... show`, `fsutil fsinfo/volume diskfree`) |
| **Tier 1** (Reversible) | Denied in Ask; allowed in Do | `set_folder_description`, `focus_window`, `arrange_windows` (with placement undo), `launch_app`, `set_clipboard` |
| **Tier 2** (Mutating) | Denied in Ask; explicit approval in Do | `write_file`, `delete_file`, `close_window`, `click_control`, `send_keys`, `power_action`, and any mutating, path-qualified, or chained/piped/redirected `run_command` |

User-authored folder descriptions (`source=user`) are pinned and cannot be overwritten by `ai describe --auto` unless `--force` is explicitly supplied.

## Host & VM Testing

- **Host ASan/UBSan test suites (`base/ai/tests/`):**
  - `t_store_sqlite.c`: SQLite FTS5, folder descriptions, user-pinning, cosine vector search, audit log.
  - `t_agent_provider.c`: JSON codec, OpenAI tool parsing, fail-closed shell risk classifier, Tier 0/1/2 policy & approval replay protection, auto-describe flow.
  - `t_gguf_local.c`: GGUF v3 parser/dequantizer (`F32`/`F16`/`Q8_0`/`Q4_0`), AVX2+FMA GEMV parity, local provider & offline agent loop.
- **Interactive GUI VM launcher:**
  ```bash
  # Remote/mock OpenAI provider (default)
  bash base/ai/tools/run_libernt_gui.sh

  # 100% air-gapped on-device GGUF mode via llama.dll
  bash base/ai/tools/run_libernt_gui.sh --no-net --local-model /path/to/model.gguf
  ```
