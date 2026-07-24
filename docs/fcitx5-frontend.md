# Fcitx5 Frontend

Vibetype 提供 Fcitx5 输入法插件，与 IBus 前端和 CLI 共享同一套后端 JSON-RPC 协议。Fcitx5 前端本身不做任何文本变换——文本规范化、术语纠正和 Qwen3 润色均由后端完成，前端直接提交 `vibetype.finalResult` 中收到的最终文本。

## Architecture

```
┌──────────────────────────────────────────┐
│ Fcitx5                                    │
│  ┌──────────────────────────────────┐     │
│  │ vibetype.so (C++ addon)           │     │
│  │  - key event handling             │     │
│  │  - fcitx5 config / panel UI       │     │
│  │  - ALSA capture via arecord       │     │
│  │  - JSON-RPC client (xtils)        │     │
│  │  - WAV segmentation               │     │
│  └──────────┬───────────────────────┘     │
└─────────────┼─────────────────────────────┘
              │ JSON-RPC 2.0 over Unix socket
              ▼
┌─────────────────────────────┐
│ vibetype-backend (C++)      │
│ - segment ASR               │
│ - text normalization        │
│ - corrections + Qwen polish │
│ - session aggregation       │
│ - final result emission     │
└─────────────────────────────┘
```

`vibetype.so` 通过 xtils `IpcClient` 直接与后端通信，不依赖额外的 Python helper 进程。

## Files installed

| File | Path |
|------|------|
| Fcitx5 addon library | `${FCITX5_LIBDIR}/fcitx5/vibetype.so` |
| Addon registration | `${FCITX5_DATADIR}/addon/vibetype.conf` |
| Input method registration | `${FCITX5_DATADIR}/inputmethod/vibetype-inputmethod.conf` |

## Configuration

通过 `fcitx5-configtool` 或手动编辑 `~/.config/fcitx5/conf/vibetype.conf` 配置：

```ini
# 触发键，默认 F12
# 例如：F12、Control+F12、Alt+space
TriggerKey=F12

# 分段时长（秒），默认 20
SegmentSeconds=20

# 后端 socket 路径（留空使用 $XDG_RUNTIME_DIR/vibetype/vibetype.sock）
SocketPath=

# ALSA 录音设备，默认 "default"
AudioDevice=default

# 以下三项由配置界面同步到共享 text-processing.json
EnableBuiltinCorrections=True
EnableQwenPolish=False
CustomCorrections=错误词=正确词
```

## Text processing and backend config

`fcitx5-configtool` 提供内置词组、Qwen 润色开关和自定义纠正设置。保存这些设置时，插件会原子更新共享的 `text-processing.json` 并尽力通知后端重载；后端负责实际文本处理，Fcitx5 仅提交最终结果。自定义规则每行使用 `错误词=正确词`。

相关配置文件：

| 文件 | 默认路径 |
|------|---------|
| `backend.json` | `~/.config/vibetype/backend.json` |
| `text-processing.json` | `~/.config/vibetype/text-processing.json` |

详细说明见 [`install.md`](install.md)，包括：

- 内置计算机术语词表（默认开启，可通过 `enable_builtin_corrections: false` 关闭）
- 自定义 `custom_corrections` 规则（对象格式或数组格式）
- 可选 Qwen3-0.6B Q4\_K\_M GGUF final 润色（默认关闭，`enable_qwen_polish: true` 开启）
- 配置热重载（mtime 自动轮询、SIGHUP、每次 startSession 检查）

## Build

Fcitx5 插件在 CMake 配置时自动检测，检测到 Fcitx5Core 开发包则自动构建：

```bash
# 安装 fcitx5 开发头文件（Arch Linux）
sudo pacman -S fcitx5

# 或（Debian/Ubuntu）
sudo apt install libfcitx5core-dev

# 构建（自动包含 Fcitx5 插件）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target vibetype -j$(nproc)
sudo cmake --install build --component cli
sudo cmake --install build --component fcitx5
```

如果不想构建 Fcitx5 插件：

```bash
cmake -B build -DBUILD_FCITX5_ADDON=OFF
```

## Usage

1. 启动后端：`systemctl --user enable --now vibetype-backend.service`
2. 重启 fcitx5：`fcitx5 -rd`
3. 在输入法设置中添加 "Vibetype Voice Input"
4. 按触发键（默认 F12）开始/停止录音，录音期间会在当前输入上下文显示动态的 preedit / auxiliary 状态指示
5. 停止录音后等待后端返回最终识别结果，自动提交到当前应用（仅提交 final text，partial 仅用于状态显示）

## Debug

```bash
journalctl --user -u vibetype-backend.service -f
fcitx5 -rd 2>&1 | grep Vibetype
```
