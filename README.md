# Clip Sync

一个用 C 写的 Android ↔ PC 文本剪贴板同步工具。

- Android 端：aarch64 静态二进制，可作为 KernelSU 模块服务运行。
- PC 端：x86_64 静态二进制，支持 Wayland（`wl-clipboard`）和 X11（`xclip`）。
- 传输：TCP，默认端口 `52345`，只同步 UTF-8 文本；图片等非文本剪贴板会被忽略。
- 无认证 / 不加密，请仅在可信任局域网使用。

## 目录

```
dist/clipsync_android                 # Android arm64 静态二进制
dist/clipsync_pc                      # PC x86_64 静态二进制
dist/clipsync-module-0.1.0.zip        # KernelSU 模块卡刷包
module/                               # 模块源码文件
android/clipsync.c                    # Android 端 C 源码
pc/clipsync.c                         # PC 端 C 源码
common/net.*                          # 简单 TCP 消息协议
```

## PC 端使用

当前系统是 Fedora Kinoite / Wayland，已安装 `wl-clipboard`，直接运行：

```bash
./dist/clipsync_pc --listen 0.0.0.0:52345
```

可选参数：

```bash
./dist/clipsync_pc --listen 0.0.0.0:52345 --poll-ms 300
./dist/clipsync_pc --get          # 打印当前剪贴板
./dist/clipsync_pc --set "hello"  # 设置剪贴板
```

## Android / KernelSU 端安装

把 `dist/clipsync-module-0.1.0.zip` 复制到手机，用 KernelSU Manager 安装并重启；
或者手动复制到模块目录（已调试通过）：

```bash
# PC 上
adb push dist/clipsync-module-0.1.0.zip /data/local/tmp/
# 手机 root shell 中
cd /data/adb/modules
unzip /data/local/tmp/clipsync-module-0.1.0.zip -d clipsync
chmod 755 /data/adb/modules/clipsync/service.sh /data/adb/modules/clipsync/bin/clipsync
```

编辑配置（PC 的局域网 IP 和端口）：

```bash
vi /data/adb/modules/clipsync/clipsync.conf
```

```ini
host=192.168.31.92
port=52345
# 手机剪贴板轮询间隔（默认 3000 ms）
poll_ms=3000
```

> 升级旧模块后，如果已有 `clipsync.conf` 里是 `poll_ms=300`，`service.sh` 会自动改成
> `poll_ms=3000`。PC 端和 Android 端需要一起更新；新协议使用 `TEXT_TS`，旧 PC 端
> 不会理解手机发来的带时间戳文本。

重启，或手动启动：

```bash
su -c '/data/adb/modules/clipsync/service.sh >/dev/null 2>&1 &'
```

日志：

```bash
tail -f /data/local/tmp/clipsync.log
```

Android 端也支持单次调试：

```bash
su -c '/data/adb/modules/clipsync/bin/clipsync --get'
su -c '/data/adb/modules/clipsync/bin/clipsync --set "hello from phone"'
```

## 原理简述

- Android 端以 uid 2000 (`com.android.shell`) 的身份直接读写 binder `clipboard` 服务，
  因此能稳定读取 Android 15/16 的剪贴板。
- PC 端通过 `wl-paste` / `wl-copy`（或 `xclip`）读写剪贴板；读取时只请求
  `text` / `UTF8_STRING` 类型，图片不会被当作文本同步。
- Android 端会检查剪贴板 MIME 类型，只处理包含 `text/*` 类型的内容；
  纯图片剪贴板会被跳过。
- 两端轮询本地剪贴板（Android 默认 3 秒，PC 默认 300 ms），变化后通过简单 TCP 协议
  发送带时间戳的 `TEXT_TS` 消息；收到远端消息后先比较时间戳，较新的内容才覆盖本地，
  再通过“最近发送/最近接收”去重避免回环。
- 同步状态会跨 TCP 会话保留，避免手机重新连上 WiFi 时两边盲目互发旧剪贴板。
- 双方每 5 秒发送一次应用层心跳，15 秒收不到任何数据就主动断开重连；同时已启用
  `SO_KEEPALIVE`，用于兜底检测半开连接。
- 手机端连接失败时按 1/2/4/8/16 秒退避重试，之后固定每 60 秒重试一次；
  PC 重启、网络短暂断开后会自动恢复。

## 让 PC 端可靠地自动恢复

这个项目本身不会帮你保证 PC 的 IP 不变，也不会自动把 PC 端进程拉起来。要做到
“回家连上 WiFi 后自动同步”和“第二天开机后自动同步”，建议：

1. 在路由器里给 PC 设置 **DHCP 保留地址**，或给 PC 配固定局域网 IP；
2. 把手机配置里的 `host` 改成这个固定 IP；
3. 让 `clipsync_pc` 随桌面会话自动启动。Wayland 下它需要继承图形会话环境变量。

Wayland 用户级 systemd 示例（放在 `~/.config/systemd/user/clipsync.service`）：

```ini
[Unit]
Description=Clip Sync PC
After=graphical-session.target

[Service]
ExecStart=%h/clip-sync/dist/clipsync_pc --listen 0.0.0.0:52345 --poll-ms 1000
Restart=always
RestartSec=3

[Install]
WantedBy=graphical-session.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now clipsync.service
```

如果 PC 的 IP 会变，当前协议没有局域网发现能力，手机端会一直尝试旧 IP，无法自动找到
新地址。

## 构建

如果本机没有交叉编译器，可用 `./build.sh` 通过 podman + Zig 重新构建。
脚本会生成 `dist/clipsync_android`、`dist/clipsync_pc` 和模块 zip。

```bash
./build.sh
```

直接使用 gcc/clang 的 PC 构建示例：

```bash
cc -O2 -Icommon -o clipsync_pc pc/clipsync.c common/net.c
```
