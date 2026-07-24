# Vibetype

[🇬🇧 English](README.en.md) | 🇨🇳 简体中文

Vibetype 是一个面向 Linux 的语音输入法项目，核心目标是：**本地、离线、快速、准确地把语音变成文字**。它使用 SenseVoice Small GGUF 模型在 CPU 上运行，通过 ALSA 采集麦克风音频，并通过 CLI / 输入法前端输出最终文本。

> 当前优先级：先重点打磨中文和英文识别效果，同时保留中英粤日韩混合识别能力。

## 特性亮点

- **本地离线 ASR**：模型下载完成后，语音识别在本机完成，音频不需要上传到云端。
- **无 GPU 依赖**：基于 `ggml` / `llama.cpp` 组件做 CPU 推理。
- **多语言混合识别**：支持中文、英文、粤语、日语、韩语。
- **适合中英混说**：后端统一做标点规范化；含中日韩文字时优先使用全角标点，纯英文等场景使用半角标点；正确处理数字中的小数点。
- **内置计算机术语纠正**：内置词表（`computer_terms.json`）默认开启，自动修正常见 ASR 术语大小写（如 `github` → `GitHub`、`javascript` → `JavaScript`）。
- **自定义纠正规则**：在 `text-processing.json` 中配置 `custom_corrections`，支持对象格式或数组格式，优先级高于内置词表。
- **可选 Qwen3 final 润色**：Qwen3-0.6B Q4_K_M GGUF，默认关闭；开启后对最终文本做进一步润色，失败时自动回退到规则纠正结果。
- **配置热重载**：修改 `backend.json` / `text-processing.json` 后，后端在 5 秒内自动重载，或在下次录音会话开始时重载，无需重启。
- **后端与前端解耦**：后端通过 Unix Socket + JSON-RPC 对外服务，使用 `xtils::IpcServer` 实现。
- **拆分包**：
  - `vibetype`：后端 + CLI + 共享 Python 代码 + systemd user service。
  - `vibetype-ibus`：IBus 前端插件包。
  - `vibetype-fcitx5`：Fcitx5 前端插件包。

## 前端状态

| 前端 | 状态 | 说明 |
| --- | --- | --- |
| CLI | 可用 | ALSA 录音、可选声卡、按键触发、剪贴板粘贴输出。 |
| IBus | 可用 | 只提交最终识别文本，partial 仅用于状态/预编辑展示。 |
| Fcitx5 | 可用 | C++ addon 前端，只提交最终识别文本。 |

## RTF 和准确率参考

默认目标模型是 **SenseVoiceSmall Q8 GGUF**。下面的数据来自上游和公开 benchmark，用来说明模型能力和大致性能，**不代表每个麦克风、口音、桌面环境下都完全一致**。Vibetype 当前重点是本地 CPU ASR，后续还会继续加入 VAD、识别效果优化和领域微调。

| 来源 / 设置 | 指标 | 结果 |
| --- | --- | --- |
| SenseVoice README | 延迟 | SenseVoice-Small 官方描述约 **70 ms 处理 10 s 音频**，约 **15× 快于 Whisper-Large**。 |
| SenseVoice llama.cpp / GGUF benchmark，普通话，CPU 8 线程，不含模型加载 | 速度 | SenseVoiceSmall 约 **20× real-time**，等价于 **RTF ≈ 0.05**。 |
| 同一 benchmark，184 条普通话音频，`normalize_zh` micro-CER | 准确率 | SenseVoiceSmall **7.81% CER** fp32 reference / **8.17% CER** Q8 runtime。 |
| 同一 benchmark，whisper.cpp 对比 | 准确率 | whisper.cpp base **31.33% CER**，small **22.12% CER**，large-v3-turbo **23.15% CER**。 |

说明：

- **CER/WER 越低越好**；**RTF 越低越快**。RTF 0.05 大约表示 1 秒计算可处理 20 秒音频。
- 上面 GGUF 普通话 benchmark 使用了 VAD 分段流程。Vibetype 当前后端还没有在 ASR 前接入 VAD，所以长音频、静音较多场景下的实际效果可能会有所不同；后续会补 native VAD。
- Vibetype 会优先优化**中文和英文**识别效果，后续计划做微调 / 领域适配，用来改善人名、术语、项目词、技术词汇、中英混说等场景。

参考：

- SenseVoice README：<https://github.com/FunAudioLLM/SenseVoice>
- GGUF CPU benchmark：<https://github.com/FunAudioLLM/SenseVoice/blob/main/runtime/llama.cpp/BENCHMARKS.md>

## 模型

### SenseVoice 模型

默认模型路径：

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf
```

如果模型不存在，后端会先启动 IPC socket，然后在后台下载模型，并通过 `vibetype.modelStatusChanged` 通知前端当前状态。

### Qwen3 润色模型（可选）

可选使用 **Qwen3-0.6B Q4_K_M GGUF** 对最终识别文本做进一步润色。默认关闭，不自动下载。

默认路径（开启时）：

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/qwen3-0.6b-q4_k_m.gguf
```

系统默认配置位于 `/usr/share/vibetype/text-processing.json`，包含基础提示词；用户配置 `~/.config/vibetype/text-processing.json` 只需覆盖要修改的字段：

```json
{
  "enable_qwen_polish": true
}
```

用户配置中的 `qwen_system_prompt` 可覆盖默认提示词，修改后重载配置即可，无需重新编译。若默认配置和用户配置均未提供有效提示词，后端会报告配置异常、跳过 LLM 并保留规则处理结果。开启后若模型不存在，后端会自动后台下载；推理失败或超时时同样回退。

模型参考：

- SenseVoice 项目：<https://github.com/FunAudioLLM/SenseVoice>
- SenseVoice Small GGUF：<https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF>
- Qwen3-0.6B GGUF：<https://huggingface.co/Qwen/Qwen3-0.6B-GGUF>

## 文本处理配置

后端支持两个可选配置文件（均可热重载，无需重启）：

| 文件 | 默认路径 |
|------|---------|
| `backend.json` | `~/.config/vibetype/backend.json` |
| `text-processing.json` | `~/.config/vibetype/text-processing.json` |

`text-processing.json` 的文本处理字段优先级高于 `backend.json`。

### 内置计算机术语词表

默认开启，自动修正常见 ASR 术语大小写。关闭方式：

```json
{ "enable_builtin_corrections": false }
```

### 自定义纠正规则

`custom_corrections` 支持对象格式或数组格式，优先级高于内置词表：

```json
{
  "custom_corrections": [
    { "from": "维博太普", "to": "Vibetype" },
    { "from": "吉特哈布", "to": "GitHub"  },
    { "from": "k8s",      "to": "Kubernetes" }
  ]
}
```

最多 500 条规则；key 和 value 各最多 256 字节。

### 配置热重载

后端通过以下方式自动重载配置，**无需重启**：

- **每次 startSession**：检查文件 mtime，有变化时自动重载。
- **每 5 秒轮询**：main loop 自动检测 mtime 变化。
- **SIGHUP**（100 ms 内响应）：`pkill -HUP vibetype-backend`
- **`vibetype.reloadConfig` RPC**：测试/诊断用。

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target package -j$(nproc)
```

CMake 会自动获取相关依赖：

- `xtils`：来自 <https://github.com/lingzolabs/xtils>
- `llama.cpp` / `ggml`：来自 source release zip

如果想使用系统已安装的 `xtils`：

```bash
cmake -B build -DVIBETYPE_USE_SYSTEM_XTILS=ON
```

## 包拆分

CPack 会生成互斥的独立包：

```text
vibetype-*.tar.gz
vibetype-ibus-*.tar.gz
vibetype-fcitx5-*.tar.gz

vibetype_*.deb
vibetype-ibus_*.deb
vibetype-fcitx5_*.deb

vibetype-*.rpm
vibetype-ibus-*.rpm
vibetype-fcitx5-*.rpm
```

这三个包是互斥的。每个包都是整包，都会自带 backend 和运行时文件。

安装一个 tar 包变体：

```bash
sudo tar -C / -xzf build/vibetype-*.tar.gz
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
```

安装 IBus / Fcitx5 变体：

```bash
sudo tar -C / -xzf build/vibetype-ibus-*.tar.gz
ibus restart

sudo tar -C / -xzf build/vibetype-fcitx5-*.tar.gz
fcitx5 -rd
```

在 RPM 系发行版上，安装对应的 `.rpm` 变体即可。

## CLI 用法

查看可用 ALSA 声卡：

```bash
vibetype-cli --list-audio-devices
```

使用默认麦克风录音，按 Ctrl+C 结束：

```bash
vibetype-cli --audio-device default --input-method auto
```

选择指定声卡：

```bash
vibetype-cli --audio-device plughw:1,0 --input-method auto
```

按键触发模式：每次按下到松开，是一次独立识别，只输出当次结果，不会累加。

```bash
vibetype-cli --hold-key F12 --audio-device default --input-method auto
```

`auto` 会先把识别结果写入剪贴板，再尝试自动发送 Ctrl+V 粘贴。这样避免用按键模拟逐字输入多语言文本，更适合中文、日文、韩文等场景。

## 后端协议

后端通过 Unix Socket 暴露 JSON-RPC 方法：

- `vibetype.hello`
- `vibetype.startSession`
- `vibetype.transcribeSegment`
- `vibetype.finishSession`
- `vibetype.cancelSession`
- `vibetype.modelStatus`
- `vibetype.reloadConfig`（测试/诊断用）
- `vibetype.configStatus`（测试/诊断用）

通知：

- `vibetype.partialResult`
- `vibetype.finalResult`
- `vibetype.error`
- `vibetype.modelStatusChanged`
- `vibetype.statusChanged`

## 文档

- CLI：[`docs/cli.md`](docs/cli.md)
- IBus：[`docs/ibus-frontend.md`](docs/ibus-frontend.md)
- 安装：[`docs/install.md`](docs/install.md)
- 规格：[`docs/vibetype-spec.md`](docs/vibetype-spec.md)
- Fcitx5：[`docs/fcitx5-frontend.md`](docs/fcitx5-frontend.md)

## License

MIT License. See [LICENSE](LICENSE) for details.
