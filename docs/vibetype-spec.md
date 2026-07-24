# Vibetype Protocol Spec

## 1. Goal

Vibetype 是一个 Linux 语音输入法：前端负责触发录音、采集与分段音频；后端负责 SenseVoice ASR、文本后处理（标点规范化、术语纠正、可选 Qwen3 润色）与最终文本输出；前端收到最终结果后 commit 到当前应用。

## 2. Scope

### 2.1 已实现功能

- **前端**：CLI + IBus + Fcitx5，三者共用同一套 JSON-RPC 2.0 client 协议。
- **后端**：C++17 + xtils，作为独立 daemon。
- **IPC**：xtils IPC / JSON-RPC 2.0 over Unix domain socket。
- **录音**：前端通过 ALSA `arecord` 采集；默认使用 `default` 设备，可通过 `--audio-device` 指定。
- **音频格式**：16 kHz, mono, PCM16 WAV。
- **触发键**：默认 F12，可配置。
- **长录音分段**：前端分段保存为 WAV segment，后端逐段 ASR。
- **后端聚合**：按 session 拼接分段文本，输出 partial 和 final。
- **Commit**：只由前端执行；停止录音后等待后端 final result，再一次性 commit。
- **文本规范化**：含中日韩文字时全角标点，纯 ASCII 场景半角标点；正确处理数字中的小数点。
- **内置计算机术语纠正**：从 `computer_terms.json` 加载，默认开启，可通过 `enable_builtin_corrections: false` 关闭。
- **自定义纠正规则**：`text-processing.json` 或 `backend.json` 的 `custom_corrections` 字段，优先级高于内置词表。
- **可选 Qwen3 final 润色**：默认关闭；`enable_qwen_polish: true` 且模型就绪时生效；失败时回退到规则纠正结果。
- **热重载配置**：backend 启动时加载，每次 `startSession` 检查 mtime；`SIGHUP` 信号触发即时强制重载（100 ms 内）；每 5 秒自动 mtime 轮询；`vibetype.reloadConfig` RPC 触发强制重载；重载成功后广播 `vibetype.statusChanged`。
- **模型自动下载**：SenseVoice 和 Qwen 模型均支持，模型缺失时后台下载。

### 2.2 非目标（当前版本）

- 流式音频传输（当前通过 WAV 文件路径提交 segment）。
- Partial commit（partial result 仅用于显示）。
- 原生 VAD 预处理（计划中）。
- Fcitx5 配置 UI（当前通过文件配置）。

## 3. Architecture

```text
+---------------------------+
| CLI / IBus / Fcitx5       |
| Python or C++ Frontend    |
| - F12 trigger             |
| - ALSA capture (arecord)  |
| - segment WAV files       |
| - display partial/final   |
| - commit final text       |
+-------------+-------------+
              |
              | JSON-RPC 2.0 over Unix socket
              v
+-------------+-------------+
| vibetype-backend          |
| C++17 + xtils daemon      |
| - session state           |
| - per-segment ASR queue   |
| - text normalization      |
| - corrections + Qwen      |
| - config hot-reload       |
| - partial/final output    |
+-------------+-------------+
              |
              v
+-------------+-------------+
| SenseVoice Engine         |
| llama.cpp/ggml-based      |
| - GGUF model loading      |
| - segment ASR             |
+---------------------------+
              |
        (optional)
              v
+-------------+-------------+
| QwenEngine                |
| Qwen3-0.6B Q4_K_M GGUF   |
| - final text polish       |
| - on-demand model load    |
+---------------------------+
```

Fcitx5 前端与 IBus/CLI 共用同一套 JSON-RPC 2.0 client 协议，不需要修改后端。

## 4. IPC Transport

- **Socket path**: `${XDG_RUNTIME_DIR}/vibetype/vibetype.sock`（默认；可通过 `--socket` 或 `backend.json` 的 `socket` 字段覆盖）
- **Protocol**: JSON-RPC 2.0
- **Encoding**: UTF-8 JSON
- **Framing**: xtils IpcServer / IpcClient 默认 framing（内部 length-prefixed JSON）
- **Large payload policy**: 不在 JSON 中传音频数据；只传 WAV 文件路径和 metadata

## 5. JSON-RPC 2.0 API

### 5.1 `vibetype.hello`

Frontend 启动后用于能力协商。

Request:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "vibetype.hello",
  "params": {
    "client": "ibus-python",
    "protocol_version": 1
  }
}
```

Result:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocol_version": 1,
    "backend": "vibetype-backend",
    "features": [
      "segment_asr",
      "partial_result",
      "final_result",
      "config_reload",
      "text_corrections",
      "sensevoice_asr",
      "model_status"
    ],
    "model": {
      "state": "ready",
      "model_path": "/home/user/.config/vibetype/models/sensevoice-small-q8.gguf",
      "model_url": "https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/main/sensevoice-small-q8.gguf",
      "message": "model ready"
    }
  }
}
```

支持 Qwen 的后端始终在 `features` 中包含 `"qwen_polish"` 和 `"polish_status"`；是否启用及当前加载状态由 result 的 `qwen` 对象表示（见 §5.9）。

`vibetype.hello` 还会同时广播一条 `vibetype.statusChanged` 通知，使新连接的客户端无需主动轮询就能获得当前状态。

### 5.2 `vibetype.startSession`

开始一个录音/识别 session。

每次调用 `startSession` 时，后端会对 `backend.json` 和 `text-processing.json` 做 mtime 检查，若文件有变化则热重载配置后再开始 session。

Request params:

```json
{
  "session_id": "uuid-string",
  "audio_format": {
    "sample_rate": 16000,
    "channels": 1,
    "sample_format": "pcm_s16le",
    "container": "wav"
  },
  "frontend": "ibus-python"
}
```

Result:

```json
{
  "accepted": true
}
```

### 5.3 `vibetype.transcribeSegment`

提交一个音频分段。前端可以在录音仍在继续时发送 segment；后端异步 ASR 并输出 partial。

`wav_path` 必须位于 `${XDG_RUNTIME_DIR}/vibetype/` 目录下，否则返回 `access_denied` 错误。音频必须是 16 kHz mono PCM16 WAV，否则返回 `invalid_audio_format`。

Request params:

```json
{
  "session_id": "uuid-string",
  "segment_index": 0,
  "wav_path": "/run/user/1000/vibetype/session-uuid/segment-000.wav",
  "duration_ms": 20000
}
```

Result:

```json
{
  "accepted": true,
  "session_id": "uuid-string",
  "segment_index": 0
}
```

### 5.4 `vibetype.finishSession`

前端停止录音后调用，表示不会再发送更多 segment。后端必须等待已接受的 segment 全部处理完成，然后经过 final 处理流水线后输出 final result。

Request params:

```json
{
  "session_id": "uuid-string",
  "segment_count": 3
}
```

Result:

```json
{
  "accepted": true,
  "session_id": "uuid-string"
}
```

### 5.5 `vibetype.cancelSession`

取消当前 session。后端丢弃 session 状态。

Request params:

```json
{
  "session_id": "uuid-string",
  "reason": "user_cancelled"
}
```

Result:

```json
{
  "cancelled": true
}
```

### 5.6 `vibetype.modelStatus`

查询后端 SenseVoice 模型状态。

Result:

```json
{
  "state": "downloading | loading | ready | error",
  "model_path": "${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf",
  "model_url": "https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/main/sensevoice-small-q8.gguf",
  "message": "human readable status"
}
```

### 5.7 `vibetype.reloadConfig`

强制重载 `backend.json` 和 `text-processing.json`，并返回当前配置状态。重载成功后广播 `vibetype.statusChanged`。

> **用途**：供调试和诊断工具使用；正常运行时 backend 会通过 mtime 轮询或 SIGHUP 自动重载，无需外部调用此方法。

Result（成功）:

```json
{
  "ok": true,
  "status": {
    "backend_config_path": "/home/user/.config/vibetype/backend.json",
    "text_proc_config_path": "/home/user/.config/vibetype/text-processing.json",
    "loaded_at": "2025-01-01T12:00:00Z",
    "revision": 3,
    "enable_builtin_corrections": true,
    "enable_qwen_polish": false,
    "custom_corrections_count": 42
  }
}
```

Result（失败，保留旧配置）:

```json
{
  "ok": false,
  "error": "failed to parse /home/user/.config/vibetype/text-processing.json",
  "status": { "..." : "旧配置状态" }
}
```

### 5.8 `vibetype.configStatus`

查询当前已加载的配置状态，不触发重载。Result 格式同 `vibetype.reloadConfig` 的 `status` 字段。

> **用途**：供调试和诊断工具使用。

### 5.9 `vibetype.polishStatus` / hello 的 Qwen 状态

该 RPC 无参数，返回值也会作为 `vibetype.hello` result 的 `qwen` 字段：

```json
{
  "qwen": {
    "enabled": false,
    "state": "idle | downloading | loading | ready | error",
    "message": "human readable status",
    "model_path": "~/.config/vibetype/models/qwen3-0.6b-q4_k_m.gguf"
  }
}
```

## 6. Server Notifications

### 6.1 `vibetype.partialResult`

后端完成一个或多个 segment 后发送。partial 只用于前端显示状态，不允许 commit。

```json
{
  "session_id": "uuid-string",
  "completed_segments": [0, 1],
  "text": "当前累计识别文本（规范化+纠正后）",
  "is_final": false
}
```

### 6.2 `vibetype.finalResult`

后端收到 `finishSession` 且所有 segment 完成并通过 final 流水线处理后发送。前端收到后一次性 commit。

```json
{
  "session_id": "uuid-string",
  "segment_count": 3,
  "text": "完整识别文本（规范化+纠正+可选Qwen润色）",
  "is_final": true
}
```

### 6.3 `vibetype.modelStatusChanged`

SenseVoice 模型状态变化时（下载/加载/就绪/失败）发送。Params 格式与 `vibetype.modelStatus` result 相同。

### 6.4 `vibetype.statusChanged`

配置重载成功或后端状态变化时发送：

```json
{
  "reason": "config_reloaded",
  "revision": 3
}
```

或 `vibetype.hello` 调用时（通知所有已连接客户端）：

```json
{
  "backend": "vibetype-backend",
  "features": ["segment_asr", "partial_result", "..."]
}
```

### 6.5 `vibetype.error`

```json
{
  "session_id": "uuid-string",
  "code": "asr_failed",
  "message": "human readable error"
}
```

## 7. Error Codes

| 代码 | 含义 |
|------|------|
| `invalid_request` | JSON-RPC request 不合法 |
| `unsupported_protocol` | 协议版本不兼容 |
| `invalid_audio_format` | 音频不是 16 kHz mono PCM16 WAV |
| `file_not_found` | `wav_path` 不存在 |
| `access_denied` | `wav_path` 不在 `${XDG_RUNTIME_DIR}/vibetype/` 下 |
| `session_not_found` | session 不存在或已结束 |
| `asr_failed` | ASR 推理失败 |
| `backend_busy` | 后端当前无法接受更多任务 |
| `model_not_ready` | 模型仍在下载或加载 |

## 8. Text Processing Pipeline

后端文本处理分两条流水线：

**Partial pipeline**（每段 ASR 完成后，快速路径）：

```
raw ASR text
  → NormalizeTranscriptText()        # 标点规范化（CJK/ASCII 上下文）
  → ApplyCorrections(custom_corrections)  # 内置词表 + 用户自定义规则
  → vibetype.partialResult
```

**Final pipeline**（所有分段完成后，完整路径）：

```
join partial segments
  → NormalizeTranscriptText()        # 重新规范化拼接后的文本
  → ApplyCorrections(custom_corrections)
  → QwenEngine.Polish()              # 可选，仅当 enable_qwen_polish=true 且模型就绪
  → ApplyCorrections()               # 二次纠正（修复 Qwen 产生的回退）
  → NormalizeTranscriptText()        # 规范化模型输出
  → （Qwen 失败时回退到规则纠正结果，记录警告日志）
  → vibetype.finalResult
```

### 8.1 NormalizeTranscriptText

- 含中日韩汉字/假名（CJK 上下文）时，ASCII 标点转全角（`.` → `。`，`,` → `，`等）。
- 纯 ASCII / 英文上下文时，全角标点转半角。
- 正确处理数字中间的小数点（`3.14` 保留为 `3.14`，不转 `3。14`）。
- 清理多余空格；标点前无空格，标点后保留合适空格（CJK 上下文下标点后无空格）。

### 8.2 ApplyCorrections

- 长键优先（避免短键遮蔽长键）。
- UTF-8 字符边界对齐匹配（不会从多字节序列中间开始替换）。
- CJK 文字相邻位置视为词边界（支持在连续中文中匹配术语）。
- ASCII 词边界：空格、标点、字符串开头/末尾。
- `custom_corrections: []` 明确清空自定义规则。

### 8.3 内置词表与自定义规则优先级

```
text-processing.json custom_corrections   (最高优先级)
  > backend.json custom_corrections
  > computer_terms.json 内置词表          (最低优先级)
```

内置词表条目仅在用户自定义规则中没有对应 key 时才生效（合并时内置词不覆盖用户规则）。

## 9. Configuration

### 9.1 Config File Paths

| 层级 | `backend.json` | `text-processing.json` |
|------|----------------|------------------------|
| 安装默认配置 | `/usr/share/vibetype/backend.json` | `/usr/share/vibetype/text-processing.json` |
| 用户覆盖配置 | `${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/backend.json` | `${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/text-processing.json` |

安装默认配置先加载，用户配置只覆盖其中显式出现的字段；用户 `text-processing.json` 的文本处理字段优先级最高。两个用户文件都是可选的。启动类 CLI 参数优先级高于配置文件。

### 9.2 backend.json 字段

**启动字段**（仅在 backend 启动时读取，热重载时忽略）：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `socket` | string | `$XDG_RUNTIME_DIR/vibetype/vibetype.sock` | Unix socket 路径 |
| `model` | string | `~/.config/vibetype/models/sensevoice-small-q8.gguf` | SenseVoice GGUF 路径 |
| `model_url` | string | HuggingFace URL | 模型缺失时下载地址 |
| `threads` | int | 8 | ASR 推理线程数 |
| `fake_asr` | bool | false | 开启假 ASR（协议调试用） |
| `fake_text` | string | "" | 假 ASR 返回的文本 |
| `qwen_model` | string | `~/.config/vibetype/models/qwen3-0.6b-q4_k_m.gguf` | Qwen3 GGUF 路径 |
| `qwen_model_url` | string | HuggingFace URL | Qwen 模型缺失时下载地址 |
| `qwen_enabled` | bool | false | 启动时预加载 Qwen 模型 |
| `qwen_threads` | int | 4 | Qwen 推理线程数 |

**文本处理字段**（热重载有效；也可放在 `text-processing.json` 中，后者优先级更高）：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `enable_builtin_corrections` | bool | true | 是否加载内置计算机术语词表 |
| `custom_corrections` | object 或 array | `{}` | 用户自定义纠正规则（见 §9.4） |
| `enable_qwen_polish` | bool | false | 是否启用 Qwen final 润色 |
| `qwen_max_tokens` | int | 256 | Qwen 最大生成 token 数 |
| `qwen_n_ctx` | int | 512 | Qwen context 窗口大小 |
| `qwen_timeout_ms` | int | 10000 | Qwen 推理超时（毫秒） |
| `qwen_system_prompt` | string | 安装默认配置中的基础提示词 | 纯文本 system prompt；用户配置可覆盖；ChatML 标记由后端按模型模板生成 |
| `qwen_prompt_template` | string | 空 | 兼容旧配置的原始模板，不建议新配置使用 |

后端代码不内置提示词，基础提示词来自安装默认配置。启用 `enable_qwen_polish` 时若默认层和用户层合并后两个提示词字段仍均为空，后端会跳过 LLM 整理、保留规则处理结果，并在日志及 `vibetype.polishStatus` 中报告 `error`。修改用户 `qwen_system_prompt` 后仅需重载配置，无需重新编译。

示例 `backend.json`（最小化配置）：

```json
{
  "model": "/data/models/sensevoice-small-q8.gguf",
  "threads": 4,
  "qwen_enabled": false,
  "enable_builtin_corrections": true
}
```

### 9.3 text-processing.json 字段

包含 §9.2 中所有文本处理字段，优先级高于 `backend.json` 中的对应字段。

示例 `text-processing.json`：

```json
{
  "enable_builtin_corrections": true,
  "enable_qwen_polish": false,
  "qwen_max_tokens": 256,
  "qwen_n_ctx": 512,
  "qwen_timeout_ms": 10000,
  "custom_corrections": [
    { "from": "维博太普",   "to": "Vibetype" },
    { "from": "吉特哈布",   "to": "GitHub" },
    { "from": "k8s",        "to": "Kubernetes" }
  ]
}
```

### 9.4 custom_corrections 格式

`custom_corrections` 支持两种等价格式：

**对象格式**（适合少量规则）：

```json
{
  "custom_corrections": {
    "维博太普": "Vibetype",
    "吉特哈布": "GitHub",
    "k8s": "Kubernetes"
  }
}
```

**数组格式**（推荐，便于排序和注释管理，最多 500 条，key/value 各最多 256 字节）：

```json
{
  "custom_corrections": [
    { "from": "维博太普",   "to": "Vibetype" },
    { "from": "吉特哈布",   "to": "GitHub" },
    { "from": "k8s",        "to": "Kubernetes" }
  ]
}
```

两种格式不可混用；同一文件中 `custom_corrections` 只用一种格式。

### 9.5 Config Hot-Reload 机制

backend 通过以下三种方式触发配置重载：

1. **SIGHUP**（立即，100 ms 内）：
   ```bash
   kill -HUP $(systemctl --user show vibetype-backend -p MainPID --value)
   ```
   或
   ```bash
   pkill -HUP vibetype-backend
   ```

2. **mtime 轮询**（每 5 秒自动）：main loop 每 100 ms tick 一次，每 50 tick（约 5 秒）检查两个配置文件的 mtime，变化时自动重载。

3. **每次 startSession 时 mtime 检查**：保证每次用户触发识别前配置都是最新的。

4. **`vibetype.reloadConfig` RPC**：测试/诊断专用，可从 CLI 工具发送。

重载成功时：
- 日志记录 `revision` 递增。
- 广播 `vibetype.statusChanged`（`reason: "config_reloaded"`，含新 `revision` 编号）。
- 若 `enable_qwen_polish` 从 false 变为 true、提示词非空且 QwenEngine 尚未启动，则立即触发异步下载+加载；提示词为空时报告错误并跳过。

重载失败时（JSON 解析错误）：
- 保留旧配置，记录错误日志。
- `last_error` 字段反映错误信息，`revision` 不变。
- 相同 mtime 不会重复尝试（避免日志洪泛）。

### 9.6 Frontend Config

前端配置（IBus/CLI/Fcitx5）独立于后端配置，详见各前端文档：

| 字段 | 说明 |
|------|------|
| `TriggerKey` / `--hold-key` | 触发键，默认 F12 |
| `SegmentSeconds` / `--segment-seconds` | 每段最大时长（秒），建议 20–30 |
| `SocketPath` / `--socket` | 后端 socket 路径 |
| `AudioDevice` / `--audio-device` | ALSA 设备，默认 `default` |

## 10. Model Management

### 10.1 SenseVoice 模型

- 默认路径：`${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf`
- 模型缺失时：后端仍启动 IPC socket，后台下载，广播 `vibetype.modelStatusChanged`。
- 下载工具：`curl`（需要已安装）。

### 10.2 Qwen3-0.6B 润色模型（可选）

- 默认路径：`${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/qwen3-0.6b-q4_k_m.gguf`
- **默认禁用**：不会自动下载，除非显式开启。
- 开启方式：在配置中同时提供非空 `qwen_system_prompt` 并设置 `enable_qwen_polish: true`；`--qwen-enabled` 仅作为测试覆盖，不能替代提示词。
- 开启后若模型不存在，后台自动下载（从 HuggingFace）。
- Qwen 推理失败或超时时，回退到规则纠正结果，记录警告日志，不影响最终输出。
- 润色仅作用于 final pipeline，不影响 partial result。

## 11. Acceptance Criteria

1. 后端可以启动，监听 `${XDG_RUNTIME_DIR}/vibetype/vibetype.sock`。
2. CLI / IBus / Fcitx5 前端均可连接后端并完成 `vibetype.hello`。
3. 默认 F12 可以开始/停止录音。
4. 前端使用 ALSA 默认输入设备生成 16 kHz mono PCM16 WAV。
5. 长录音被前端分成多个 WAV segment。
6. 后端对每个 segment 做 ASR 并输出 partial result。
7. 停止录音后，后端处理完全部 segment 并输出 final result。
8. 前端只在收到 final result 后一次性 commit。
9. 内置计算机术语词表默认开启，正确纠正常见术语大小写。
10. 自定义纠正规则（对象或数组格式）正确应用，优先级高于内置词表。
11. 修改 `text-processing.json` 后，后端在 5 秒内自动重载（mtime 轮询）或在下次 startSession 时重载。
12. `SIGHUP` 在 100 ms 内触发强制重载。
13. Qwen 润色默认关闭；开启后模型缺失时后台下载，失败时回退。
14. 模型缺失时后端仍可启动，前端能看到 `modelStatus` 非 ready 并提示用户。
15. CMake 可以生成 `.deb`、`.rpm` 和 `.tar.gz` 包。
