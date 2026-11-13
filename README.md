# Agora

**Agora** is a native **C++20 / Qt6** desktop studio where multiple local LLMs debate in a shared room, hand off tasks via `@mentions`, and collaboratively write, patch, review, and execute code inside an isolated **Bubblewrap (`bwrap`)** sandbox workspace.

Built with zero external C++ dependencies beyond the C++20 standard library and system `Qt6` (`Core`, `Gui`, `Widgets`, `Network`, `Sql`, `Test`), Agora is designed for single-GPU local workstations running `llama.cpp`'s `llama-server` in router mode. Participants can also run on hosted APIs (OpenAI, Anthropic, Gemini, OpenRouter, ...) alongside local models.

---

## Features

- **Multi-Model Debate & Collaboration Room**:
  - Add multiple models to the same room with distinct handles and system prompts. 14 role presets: `@architect`, `@coder`, `@reviewer`, `@moderator`, `@tester`, `@security`, `@debugger`, `@perf`, `@devops`, `@docs`, `@product`, `@ux`, `@skeptic`, `@researcher`.
  - **Councils**: load a ready-made team in one click: Coding Council, Full Dev Team, Debug Squad, Security Review, Performance Pass, Design Debate, Product & Docs. Participants already in the room with the same handle keep their model and prompt.
  - The roster and the open room are saved automatically and restored on the next start.
  - **Dynamic Speaker Scheduling**: A participant passes the floor by ending its message with another participant's `@handle` (e.g. `-> @coder`); otherwise turns go Round-Robin.
  - **Perspective-Aware Context Formatting**: Each participant sees its own prior turns as `assistant` and peer models' turns (plus tool executions) as structured `user` messages prefixed with `[Speaker: @handle (Model)]`. Consecutive peer messages are merged so strict chat templates accept them.
  - **Context Budget**: History is trimmed to fit each model's context window (`n_ctx` from `/props`), calibrated from the real prompt token counts the server reports. The user's task is always kept; on a context-overflow error the turn is retried once with less history.
  - **Ending a Debate**: A participant that writes `[DONE]` closes the session once the task is complete; otherwise the debate runs to the turn limit.
  - **Live Human Intervention**: Stop at any moment, step turn-by-turn, or interject messages mid-debate.
- **External AI Providers** (top bar → **☁ Providers**):
  - Any OpenAI-compatible chat API can host participants, mixed freely with local models in the same room. Presets for OpenAI, Anthropic, Google Gemini, OpenRouter, Groq, DeepSeek, Mistral, Ollama and LM Studio.
  - Pick the provider and model per participant. **Test connection & fetch models** lists the provider's models from `GET /models`.
  - API keys can be given as `$ENV_VAR` to read them from the environment; a key typed directly is stored in plain text in Agora's settings file.
  - **Context Budget** (default 32k tokens) caps the history sent per request, which is also the cost cap per turn on paid APIs.
  - External models skip VRAM management. Agora retries once without `temperature` for models that reject it, and once after 5 s on rate limits or overload (HTTP 429/5xx).
  - Everything an external participant sees, including file contents and command output from the workspace, is sent to that provider. Tools still run only in the local sandbox, under the same approval policy.
- **Workspace Checkpoints**:
  - The workspace is snapshotted before and after every turn as commits under `refs/agora/<session>`, using a private index. HEAD, branches and your staging area are never touched, so this is safe inside a real project.
  - Each turn that changed files gets **View changes** (per-turn diff) and **Undo this turn** links in the transcript. Undo saves the current state first, so it can itself be undone.
- **Tool Approval** (Settings → Tool Approval):
  - *Never ask*, *Ask before writes & commands* (`write_file`, `patch_file`, `run_command`), or *Ask before every tool call*.
  - The approval dialog shows the exact command, file content or patch. Choose **Allow once**, **Allow `<tool>` for this session**, or **Deny** with an optional note that the model sees.
- **Per-Turn Stats**: prompt and generated tokens, tokens/s, prompt prefill time and model swap/load time, shown in each message header.
- **VRAM-Aware Router Management**:
  - Integrates with `llama-server` router endpoints (`GET /models`, `POST /models/load`, `POST /models/unload`, `GET /props`).
  - Automatically calculates resident GGUF sizes against a configurable **VRAM Budget** (default `8.5 GiB` for 12 GB GPUs). Lightweight models remain co-resident, while large models (`9B`–`29B`) are swapped in and out on turn transitions.
- **Bubblewrap (`bwrap`) Isolated Shared Workspace**:
  - Every room shares a Git-initialized workspace mounted at `/workspace` inside `bwrap` (`--unshare-all` and `--new-session` by default, read-only host `/usr`, `/bin`, `/lib` and a minimal `/etc`).
  - Git always runs inside the sandbox. Agents can commit, but `.git/config` and `.git/hooks` are read-only, so nothing planted there can run when you use git on the host. An existing repository's config is never modified.
  - **Resource limits** per command: memory and process count via a systemd user scope (cgroup v2), falling back to an address-space `ulimit` when systemd is unavailable; plus a maximum file size and a wall-clock timeout. Output is capped at 64 KiB without killing the command.
  - Built-in tools exposed via OpenAI tool-calling schema + `<tool_call>` XML/JSON fallback parser:
    - `list_files(path)`
    - `read_file(path, start_line, end_line)`
    - `write_file(path, content)`
    - `patch_file(path, search, replace)` — the search block must match exactly once
    - `run_command(command, timeout_sec)`
    - `git_diff()`
  - File tools refuse symlinks, `..` traversal and direct `.git` access.
- **3-Pane Native Qt6 Interface**:
  - **Left Pane**: Participant cards with live status (`loaded`, `sleeping`, `unloaded`, `speaking`, provider), council templates, and the saved-sessions browser (right-click to delete).
  - **Center Pane**: Streaming transcript with Markdown rendering, reasoning (`<think>`) blocks, tool call cards, per-turn stats, checkpoint links, and `@mention` quick-chips.
  - **Right Pane**: Live file browser, syntax-highlighted code editor, colorized `git diff` viewer, and interactive `bwrap` sandbox terminal.
- **Fully asynchronous core**: model swaps, streams, tool calls and approvals all continue from callbacks, so the UI never blocks and Stop takes effect immediately.

---

## Requirements

- **Linux** with `bwrap` (Bubblewrap) and `git`
- Optional: a **systemd user session** (`systemd-run --user`) for memory and process limits on sandboxed commands
- **C++20 compiler** (`GCC 12+` or `Clang 15+`) and **CMake 3.20+**
- **Qt6** development packages (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network`, `Qt6Sql`, `Qt6Test`)
- **`llama-server`** running in router mode (`--models-dir ~/models --port 8089`), and/or an API key for an OpenAI-compatible provider

---

## Build & Run

```bash
cmake --preset release
cmake --build --preset release

# Run unit, sandbox, end-to-end flow and UI tests
ctest --preset release

# Launch the desktop app
./build/agora
```

A sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer) is available as the `asan` preset:

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

The end-to-end tests run a complete debate against a small fake `llama-server` (`tests/fake_llama_server.h`), so they need no models or GPU.

---

## Settings

**Settings** in the top bar (stored with `QSettings`):

| Setting | Default | Notes |
|---|---|---|
| Router Base URL / API Key | `http://127.0.0.1:8089` | |
| Local GGUF Models Folder | `~/models` | Used for model sizes in VRAM planning |
| Router Start Script | `~/llama-scripts/serve-router.sh` | Run by the **Start Router** button |
| Stream Idle Timeout | 600 s | Must cover prompt processing of your largest model |
| Sandbox Memory Limit | 4096 MiB | Per command, including child processes |
| Sandbox Process Limit | 512 | Needs a systemd user session |
| Tool Approval | Never ask | See *Tool Approval* above |
| Max Tool Rounds per Turn | 8 | How many tool-call rounds one participant may chain |
| Checkpoints | On | Per-turn snapshots, diffs and undo |

External providers are managed separately under **☁ Providers** (name, base URL, API key or `$ENV_VAR`, context budget).

---

## License

MIT — see [LICENSE](LICENSE).
