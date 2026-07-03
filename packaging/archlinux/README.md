# Arch Linux 打包说明

## 安装方式

### 方式一：从 AUR 安装（推送到 GitHub 之后）

```bash
# 依赖
sudo pacman -S --needed base-devel cmake git ninja python alsa-utils curl

# 通过 AUR helper（如 yay）
yay -S vibetype vibetype-ibus

# 或手动构建
git clone https://aur.archlinux.org/vibetype.git
cd vibetype
makepkg -si
```

### 方式二：本地构建（无需推送）

```bash
# 1. 创建临时构建目录
mkdir -p /tmp/vibetype-build/src
cd /tmp/vibetype-build

# 2. 链接源码
ln -s /home/albert/workspace/archlinux/vibetype src/vibetype

# 3. 复制 PKGBUILD
cp src/vibetype/packaging/archlinux/PKGBUILD.local PKGBUILD

# 4. 构建安装
makepkg -si
```

### 方式三：手动 CMake 构建（不打包）

```bash
cd vibetype
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target vibetype-backend -j$(nproc)
sudo cmake --install build --component cli

# 可选：IBus 前端
sudo cmake --install build --component ibus
```

## 启动

```bash
# 启动后端（系统级服务）
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service

# 检查状态
journalctl --user -u vibetype-backend.service -f

# 此时 model 会自动下载到 ~/.config/vibetype/models/
# 等 model 下载完成后再使用前端
```

## 使用 CLI

```bash
# 列出声音设备
vibetype-cli --list-audio-devices

# 按住 F12 录音，松手识别并粘贴
vibetype-cli
```

## 使用 IBus

```bash
# 安装 IBus 前端
sudo cmake --install build --component ibus
ibus restart

# 在输入法设置中添加 "Vibetype Voice Input 🎙"
# 按 F12 触发录音
```

## 包内容

### vibetype（主包）

| 文件 | 路径 |
|------|------|
| 后端二进制 | `/usr/bin/vibetype-backend` |
| CLI 前端 | `/usr/bin/vibetype-cli` |
| 共享 Python 模块 | `/usr/share/vibetype/python/vibetype_client.py` |
| Systemd 用户服务 | `/usr/lib/systemd/user/vibetype-backend.service` |
| 文档 | `/usr/share/doc/vibetype/` |
| License | `/usr/share/licenses/vibetype/LICENSE` |

### vibetype-ibus（IBus 插件包）

| 文件 | 路径 |
|------|------|
| IBus 引擎 | `/usr/bin/vibetype-ibus` |
| IBus 组件声明 | `/usr/share/ibus/component/vibetype.xml` |
| License | `/usr/share/licenses/vibetype-ibus/LICENSE` |

## 依赖说明

| 包名 | 用途 |
|------|------|
| `alsa-utils` | 提供 `arecord` 录音 |
| `curl` | 后端下载 model 文件 |
| `gcc-libs` `glibc` | C++ 运行时 |
| `python` | CLI/IBus 前端脚本 |
| `ibus` | IBus 输入法框架（仅 ibus 子包） |
| `python-gobject` | GI 绑定（仅 ibus 子包） |
| `wl-clipboard` | Wayland 剪贴板粘贴（可选） |
| `xclip` | X11 剪贴板粘贴（可选） |
| `xdotool` | X11 键盘模拟粘贴（可选） |
