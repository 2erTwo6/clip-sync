# Clip Sync

一个用 C 写的 Android ↔ PC 文本剪贴板同步工具。

- Android 端：aarch64 静态二进制，可作为 KernelSU 模块服务运行。
- PC 端：x86_64 静态二进制，支持 Wayland（`wl-clipboard`）和 X11（`xclip`）。
- 传输：TCP，默认端口 `52345`，只同步 UTF-8 文本。
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
poll_ms=300
```

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
- PC 端通过 `wl-paste` / `wl-copy`（或 `xclip`）读写剪贴板。
- 两端轮询本地剪贴板，变化后通过简单 TCP 协议发送 `TEXT` 消息；
  收到远端 `TEXT` 后写回本地，并通过“最近发送/最近接收”去重避免回环。
- 剪贴板读取失败、PC 重启、网络短暂断开后会自动重试。

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
