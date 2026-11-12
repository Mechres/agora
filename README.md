# Agora

**Agora** is a native **C++20 / Qt6** desktop studio where multiple local LLMs debate in a shared room, hand off tasks via `@mentions`, and collaboratively write, patch, review, and execute code inside an isolated **Bubblewrap (`bwrap`)** sandbox workspace.

Built with zero external C++ dependencies beyond the C++20 standard library and system `Qt6` (`Core`, `Gui`, `Widgets`, `Network`, `Sql`, `Test`), Agora is designed for single-GPU local workstations running `llama.cpp`'s `llama-server` in router mode.

---

## Features

- **Multi-Model Debate & Collaboration Room**:
  - Add multiple local models to the same room with distinct handles (`@architect`, `@coder`, `@reviewer`, `@moderator`) and system prompts.
  - **Dynamic Speaker Scheduling**: Models can pass the floor directly by mentioning another participant's `@handle`, or take turns in Round-Robin order.
  - **Perspective-Aware Context Formatting**: Each participant sees its own prior turns as `assistant` and peer models' turns (plus tool executions) as structured `user` messages prefixed with `[Speaker: @handle (Model)]`.
  - **Live Human Intervention**: Pause/stop generation at any millisecond, step turn-by-turn, or interject messages mid-debate.
- **VRAM-Aware Router Management**:
  - Integrates with `llama-server` router endpoints (`GET /models`, `POST /models/load`, `POST /models/unload`).
  - Automatically calculates resident GGUF sizes against a configurable **VRAM Budget** (default `8.5 GiB` for 12 GB GPUs). Lightweight models remain co-resident for zero-latency banter, while large models (`9B`–`29B`) are automatically swapped in and out on turn transitions.
- **Bubblewrap (`bwrap`) Isolated Shared Workspace**:
  - Every room shares a Git-initialized workspace mounted at `/workspace` inside `bwrap` (`--unshare-all` by default for full network isolation, with read-only host `/usr`, `/bin`, `/lib` mounts).
  - Built-in tools exposed via OpenAI tool-calling schema + `<tool_call>` XML/JSON fallback parser:
    - `list_files(path)`
    - `read_file(path, start_line, end_line)`
    - `write_file(path, content)`
    - `patch_file(path, search, replace)`
    - `run_command(command, timeout_sec)`
    - `git_diff()`
- **3-Pane Native Qt6 Interface**:
  - **Left Pane**: Room roster with live model status badges (`loaded`, `sleeping`, `unloaded`), role presets, and SQLite session browser.
  - **Center Pane**: Streaming debate transcript with reasoning (`<think>`) blocks, tool call cards, and `@mention` quick-chips.
  - **Right Pane**: Live file browser (`QFileSystemWatcher`), syntax-highlighted code editor, colorized `git diff` viewer, and interactive `bwrap` sandbox terminal.

---

## Requirements

- **Linux** with `/usr/bin/bwrap` (Bubblewrap) and `git`
- **C++20 compiler** (`GCC 12+` or `Clang 15+`) and **CMake 3.20+**
- **Qt6** development packages (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network`, `Qt6Sql`, `Qt6Test`)
- **`llama-server`** running in router mode (`--models-dir ~/models --port 8089`)

---

## Build & Run

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run unit & sandbox integration tests
ctest --test-dir build --output-on-failure -V

# Launch the desktop app
./build/agora
```

---

## License

MIT — see [LICENSE](LICENSE).
