# Vibetype MVP Spec

## 1. Goal

Vibetype 是一个 Linux 语音输入法：前端负责触发录音、采集与分段音频；后端负责 SenseVoice ASR、分段结果聚合与最终文本输出；前端收到最终结果后 commit 到当前应用。

## 2. Scope

### 2.1 MVP 范围

- 前端：IBus + Python。
- 后端：C++17 + xtils，作为独立 daemon。
- IPC：xtils IPC / JSON-RPC 2.0 over Unix domain socket。
- 录音：前端使用 ALSA 默认输入设备。
- 音频格式：16 kHz, mono, PCM16 WAV。
- 触发键：默认 F12，可配置。
- 长录音：前端分段保存为 WAV segment，后端逐段 ASR。
- 后端聚合：后端按 session 拼接分段文本，可输出 partial，最终输出 final。
- Commit：只由前端执行；停止录音后等待后端 final result，再一次性 commit。

### 2.2 非目标

- MVP 不实现 Fcitx5 前端，但后端协议必须可被 Fcitx5 复用。
- MVP 不实现输入设备选择；配置界面预留设备选择入口。
- MVP 不实现流式音频传输；前端通过 WAV 文件路径提交 segment。
- MVP 不实现 partial commit。
- MVP 不实现完整文本润色；但后端 final 阶段预留润色插入点。

## 3. Reference-only 约束

`sensevoice-small-llamacpp-gguf/` 是参考模型与参考 runtime 包，不在本项目实现中直接修改。

后端 SenseVoice engine 基于现有 llama.cpp SenseVoice runtime 的思路重写：

- 参考模型文件：
  - `sensevoice-small-llamacpp-gguf/models/sensevoice-small-q8.gguf`
  - `sensevoice-small-llamacpp-gguf/models/fsmn-vad.gguf`
- 参考代码：
  - `sensevoice-small-llamacpp-gguf/code/llama.cpp/sensevoice/funasr-sensevoice/funasr-sensevoice.cpp`
- 注意：预置 `bin/llama-funasr-sensevoice` 不能作为长期依赖；不同机器可能出现 CPU 指令集不兼容。

## 4. Architecture

```text
+---------------------------+
| IBus Python Frontend      |
| - F12 trigger             |
| - ALSA capture            |
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
| - segment queue           |
| - partial/final output    |
| - future polish hook      |
+-------------+-------------+
              |
              v
+-------------+-------------+
| sensevoice-engine         |
| rewritten llama.cpp-based |
| - GGUF model loading      |
| - segment ASR             |
| - optional VAD usage      |
+---------------------------+
```

Future Fcitx5 前端只需要实现同一套 JSON-RPC 2.0 client 协议，不应要求修改后端。

## 5. IPC Transport

- Socket path: `${XDG_RUNTIME_DIR}/vibetype/vibetype.sock`
- Protocol: JSON-RPC 2.0
- Encoding: UTF-8 JSON
- Framing: 使用 xtils JSON-RPC 2.0 IPC 框架默认 framing；若实现阶段需要显式约定，则采用一行一个 JSON message 的 JSON Lines 方式。
- Large payload policy: 不在 JSON 中传大块音频；只传 WAV 文件路径和 metadata。

## 6. JSON-RPC 2.0 API

### 6.1 `vibetype.hello`

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
    "features": ["segment_asr", "partial_result", "final_result"]
  }
}
```

### 6.2 `vibetype.startSession`

开始一个录音/识别 session。

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

### 6.3 `vibetype.transcribeSegment`

提交一个音频分段。前端可以在录音仍在继续时发送 segment；后端可以异步处理并输出 partial。

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

### 6.4 `vibetype.finishSession`

前端停止录音后调用，表示不会再发送更多 segment。后端必须等待已接受的 segment 全部处理完成，然后输出 final result。

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

### 6.5 `vibetype.cancelSession`

取消当前 session。后端应丢弃 session 状态并尽量停止未开始的 ASR 任务。

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

### 6.6 `vibetype.modelStatus`

查询后端模型状态。前端启动、用户触发录音前都可以调用。

Result:

```json
{
  "state": "downloading | loading | ready | error",
  "model_path": "${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf",
  "model_url": "https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/main/sensevoice-small-q8.gguf",
  "message": "human readable status"
}
```

## 7. Server Notifications

### 7.1 `vibetype.partialResult`

后端完成一个或多个 segment 后可以发送 partial。partial 只用于前端显示状态，不允许 commit。

Notification params:

```json
{
  "session_id": "uuid-string",
  "completed_segments": [0, 1],
  "text": "当前累计识别文本",
  "is_final": false
}
```

### 7.2 `vibetype.finalResult`

后端在收到 `finishSession` 且所有 segment 完成后发送。前端收到后一次性 commit。

Notification params:

```json
{
  "session_id": "uuid-string",
  "segment_count": 3,
  "text": "完整识别文本",
  "is_final": true
}
```

如果录音没有超过分段阈值，则 `segment_count` 为 1，后端仍通过同一 final result 输出完整文本。

### 7.3 `vibetype.modelStatusChanged`

模型下载、加载、就绪或失败时发送。前端收到 `state != ready` 时应提示用户“模型仍在下载/加载”，不要开始 ASR commit 流程。

Notification params 与 `vibetype.modelStatus` result 相同。

### 7.4 `vibetype.error`

Notification params:

```json
{
  "session_id": "uuid-string",
  "code": "asr_failed",
  "message": "human readable error"
}
```

## 8. Error Codes

- `invalid_request`: JSON-RPC request 不合法。
- `unsupported_protocol`: 协议版本不兼容。
- `invalid_audio_format`: 音频不是 16 kHz mono PCM16 WAV。
- `file_not_found`: `wav_path` 不存在。
- `access_denied`: `wav_path` 不在允许访问的 runtime 目录内。
- `session_not_found`: session 不存在或已结束。
- `asr_failed`: ASR 推理失败。
- `backend_busy`: 后端当前无法接受更多任务。
- `model_not_ready`: 模型仍在下载或加载，前端应提示用户稍后再试。

## 9. Frontend Behavior: IBus Python

### 9.1 Trigger

- 默认录音键：F12。
- F12 第一次按下：开始录音并调用 `vibetype.startSession`。
- F12 第二次按下：停止录音并调用 `vibetype.finishSession`。
- 触发键必须可配置。
- 如果应用占用了 F12，用户可在配置中换键。

### 9.2 Audio Capture

- MVP 使用 ALSA 默认输入设备。
- 当前阶段不让用户选择输入设备。
- 配置界面预留输入设备选择入口，后续实现。
- 输出必须为：16 kHz, mono, PCM16 WAV。
- Segment 文件存放建议：`${XDG_RUNTIME_DIR}/vibetype/<session_id>/segment-NNN.wav`。

### 9.3 Segmentation

- 前端负责分段。
- 默认最大 segment 时长应作为配置项，建议初始值 20-30 秒。
- 每个 segment 完成后即可调用 `vibetype.transcribeSegment`。
- 停止录音时，前端写完最后一个 segment 后调用 `vibetype.finishSession`。

### 9.4 Commit Rule

- partial result 只显示，不 commit。
- 停止录音后，前端等待后端 `vibetype.finalResult`。
- 收到 final result 后，前端将 `text` 一次性 commit 到当前应用。
- 如果收到 error，不 commit 空文本，显示错误状态。

### 9.5 Recording Test

配置界面需要预留“录音测试”：

- 使用当前默认 ALSA 输入设备录制一小段。
- 验证是否能生成 16 kHz mono PCM16 WAV。
- 显示录音成功/失败状态。
- 后续可扩展为播放测试录音或提交给后端做测试识别。

## 10. Backend Behavior

### 10.1 Process Model

- 后端是独立 daemon。
- 后端不依赖 IBus 或 Fcitx5。
- 后端通过 systemd user service 启动。
- 后端启动后监听 `${XDG_RUNTIME_DIR}/vibetype/vibetype.sock`。
- 后端使用 xtils 提供的 IPC / JSON-RPC 2.0 能力。
- 后端使用 xtils Config 解析命令行和 `--config-file` 配置文件。

### 10.2 SenseVoice Engine

后端内部实现 `sensevoice-engine`：

- 基于参考 llama.cpp SenseVoice runtime 思路重写。
- 加载 SenseVoice GGUF 模型。
- 可加载 FSMN VAD GGUF 模型。
- 输入单个 16 kHz mono PCM16 WAV segment。
- 输出该 segment 的识别文本。

### 10.3 Session Aggregation

后端按 `session_id` 管理状态：

- 接收 segment 后加入 session。
- 每段独立 ASR。
- 按 `segment_index` 排序拼接文本。
- 可以在任意 segment 完成后发送 `partialResult`。
- 收到 `finishSession` 后，等待 `segment_count` 个 segment 全部完成。
- 全部完成后生成完整文本，清理重复标点符号，并发送 `finalResult`。

### 10.4 Future Polish Hook

后续需要对完整内容做润色。MVP 先不实现润色，但后端 final pipeline 应预留位置：

```text
segments -> per-segment ASR -> ordered concatenation -> punctuation cleanup -> future polish -> finalResult
```

## 11. Configuration

### 11.1 Frontend Config

MVP 配置：

- trigger key，默认 `F12`。
- segment max duration，建议默认 20-30 秒。
- backend socket path。

预留配置：

- input device selection。
- recording test UI。

### 11.2 Backend Config

后端使用 xtils Config。命令行参数和配置文件键保持一致：

- `socket`: socket path。
- `model`: SenseVoice GGUF model path，默认 `${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf`。
- `model-url`: 模型缺失时自动下载的 URL。
- `threads`: ASR thread count。
- `fake-asr`: 协议调试开关。
- future `vad-model`: FSMN VAD GGUF model path。
- future `max-jobs`: max concurrent sessions / jobs。

模型缺失时，后端仍应先启动 IPC，然后后台下载模型并通过 `vibetype.modelStatusChanged` 通知前端。前端必须把 `state != ready` 展示为“模型仍在下载/加载”。

## 12. Acceptance Criteria

MVP 完成时需要满足：

1. 后端可以启动，并监听 `${XDG_RUNTIME_DIR}/vibetype/vibetype.sock`。
2. IBus Python 前端可以连接后端并完成 `vibetype.hello`。
3. 默认 F12 可以开始/停止录音。
4. 前端使用 ALSA 默认输入设备生成 16 kHz mono PCM16 WAV。
5. 长录音可以被前端分成多个 WAV segment。
6. 后端可以对每个 segment 做 ASR。
7. 后端可以按 `segment_index` 拼接文本。
8. 后端可以输出 partial result。
9. 停止录音后，后端处理完全部 segment 并输出 final result。
10. 前端只在收到 final result 后一次性 commit。
11. Fcitx5 前端未来可以复用同一 JSON-RPC 2.0 协议。
12. 模型缺失时后端仍可启动，前端能看到 `modelStatus` 非 ready 并提示用户。
13. CMake 可以生成 `.deb` 和 `.tar.gz`；Arch Linux 使用 tar 包安装。

## 13. Open Questions

- xtils JSON-RPC 2.0 IPC 的具体 API 与 framing 细节需要在实现阶段确认。
- ALSA 默认设备在 PipeWire/PulseAudio 环境下的实际兼容性需要测试。
- segment 默认最大时长最终取 20 秒还是 30 秒需要实现前确定。
- 完整文本润色使用本地规则、LLM 还是可选后处理服务，后续单独设计。
