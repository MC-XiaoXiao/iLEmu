# iLEmu

**用户态 iPhone OS 模拟器** —— 在 Linux 主机上直接运行早期 iPhone 固件的原生 ARM 用户态二进制，不虚拟 SoC、不加载 XNU 内核镜像，而是用 C++ 重新实现了一整套 Darwin 内核 ABI 与 iPhone OS 系统框架。

`launchd`、`SpringBoard`、`lockdownd`、`CommCenter` 等守护进程都是从真实固件根文件系统里加载的**原版 ARM 二进制**；模拟器提供的是它们脚下的那一层：BSD 系统调用、Mach 陷入与 IPC、IOKit 驱动服务，以及 OpenGL ES / CoreSurface / LayerKit / GraphicsServices 等图形与输入框架。

内核兼容层基于 Apple 开源的 [**XNU `rel/xnu-1228`**](https://github.com/apple-oss-distributions/xnu/tree/rel/xnu-1228) 编写 —— 该版本属于 Darwin 9 系列，与 iPhone OS 2.x–3.x 同源。系统调用语义、结构体布局与错误码行为均以这份源码为准绳并在注释中标注出处。详见 [ABI 依据与参考实现](#abi-依据与参考实现)。

> **当前状态**：可基础引导至 iPhone OS 3.0，支持 GPU 加速与 JIT 翻译缓存，并已能运行部分第三方应用（*Angry Birds* 已实测可运行）。项目仍处于早期阶段，详见[已知限制](#已知限制)。

---

## 目录

- [特性](#特性)
- [架构概览](#架构概览)
- [ABI 依据与参考实现](#abi-依据与参考实现)
- [环境要求](#环境要求)
- [构建](#构建)
- [准备固件根文件系统](#准备固件根文件系统)
- [运行](#运行)
- [命令行参考](#命令行参考)
- [交互控制台](#交互控制台)
- [调试与性能分析](#调试与性能分析)
- [目录结构](#目录结构)
- [已知限制](#已知限制)
- [路线图](#路线图)
- [法律声明](#法律声明)
- [许可证](#许可证)

---

## 特性

### CPU 与内存

- 基于 **Dynarmic** A32 动态二进制翻译器，模拟 **ARM1176JZF-S**（ARMv6KZ + Thumb），对应 Samsung S5L8900（iPhone1,1 / M68AP），400 MHz / 128 MB RAM / 320×480。
- **JIT 代码缓存**大小可调（`--jit-cache-mib 8..128`，默认 64 MiB），支持按范围失效以处理自修改代码与 `sys_icache_invalidate`。
- **跨进程、跨运行的翻译位置缓存**：`JitTranslationProfile` 把此前成功生成过宿主代码的 A32 位置描述符持久化到宿主缓存目录，下次启动可预编译热点块，缩短冷启动时间。缓存文件只含位置描述符，**不含**任何生成的机器码。
- 完整的 32 位客户机地址空间模型：页级权限、共享内存、文件页缓存（`file_page_cache`）、`vm_map` 区域管理。
- 多虚拟核心（`--cores N`）用于压力测试与调度器验证；真实设备档案为单核。
- 客户机线程由 `XnuScheduler` 统一调度，支持大量客户机线程复用少量执行槽。

### 内核 ABI

模拟器对外声明的最高契约是 **Darwin 9.4.0 / RELEASE_ARM**，语义以 Apple 开源的 [**xnu-1228**](https://github.com/apple-oss-distributions/xnu/tree/rel/xnu-1228)（Darwin 9 系列，对应 iPhone OS 2.x–3.x 时期）为准绳；结构体布局、错误码语义与标志位处理都对着这份源码逐一核对，源码位置在注释里直接标注（例如 `statfs64` 的 2168 字节布局出自 `bsd/sys/mount.h`）。

- **BSD 系统调用层**：文件 I/O、目录、xattr、文件锁、`mmap`/`mprotect`/`madvise`、进程与信号、`posix_spawn`、资源限制、`kqueue`/`kevent`、`select`、`sysctl`、`proc_info`、AIO、BPF、socket、共享区域（shared region）等，并处理 Darwin 9 为 pthread 取消点引入的 `*_nocancel` 变体入口。未实现的调用会记录诊断并返回 `ENOSYS`，不会直接崩溃。
- **Mach 层**：`mach_msg` 完整消息路径（含 OOL 内存与端口权限传递）、端口与端口集、任务/线程管理、异常端口、`vm_*` 全家桶（allocate / deallocate / protect / remap / copy / read / region / purgable / memory entry）、信号量、host / clock / processor set。
- **MIG 线格式元数据自动生成**：`tools/mig` 里的 `ilemu_mig_id_gen` 直接消费 Apple 开源的 `.defs` 文件（XNU、launchd-257、configd-137.3），生成 ARM32 的子系统 ID 与消息布局，避免手写魔数。Mach 子系统的线格式在 Darwin 8/9 之间保持稳定，因此这部分以 `xnu-792.24.17` 的 `.defs` 为基准（生成代码位于 `ilemu::xnu792::mig` 命名空间），而 ARM 相关的 `machine_types.defs` 取自 xnu-1228 检出。
- **IOKit**：显示与 VSync、MBX 图形、音频、摄像头、JPEG 编码器、基带、电源管理、MobileFileIntegrity。
- **固件版本自适应**：面向客户机的 ABI 不写死在某一版固件上。例如 GraphicsServices 的 GSEvent 结构布局有 `darwin9.0` / `darwin9.3` / `darwin9.4` 三套档案，由 `GraphicsServicesInputProfile::detect()` 在加载客户机 GraphicsServices 二进制时，量取 `_GSEventGetHandInfo`、`_GSEventGetPathInfoAtIndex` 等调用点的结构体拷贝尺寸来自动判别，从而在不同固件版本上都能正确投递触摸与按键事件。

### 图形

- iPhone OS 的 `OpenGLES` / `EAGL` 框架采用 **HLE（高层模拟）**：直接建模 EGL/GLES 状态机，而不是去模拟 PowerVR MBX 的内核命令流。
- 两条渲染后端：
  - **软件光栅器**（`gles_rasterizer` / `gles_renderer`），无外部依赖，用于确定性测试与无 GPU 环境；
  - **Vulkan 后端**（`--gpu` 或 `--gles-backend vulkan`），使用 Vulkan Memory Allocator，着色器由 `glslc` 在构建期编译内嵌，并带宿主管线缓存。
- 合成链路：`CoreSurface` → `SurfaceStore` → `LayerKit`（CoreAnimation 之前的合成器）→ `IOMobileFramebuffer` → 宿主窗口，`PresentationTracker` 与 `SceneCoordinator` 负责帧序与前后台场景归属。
- 输出方式：SDL2 窗口（`--display sdl`）、无头（`--display headless`）、逐帧写文件（`--frame-output`）、运行时截图（控制台 `snapshot`）。

### 输入

- 通过 `GraphicsServices` 的 GSEvent ABI 注入触摸与物理按键：触摸按下/移动/抬起/取消、Home、锁屏、音量键、静音拨片。
- 支持触摸回放脚本（`--touch-replay`）与标准输入实时控制（`--control-stdin`）。

### 音频

- IOKit 音频服务 + CoreAudio / AudioToolbox HLE + Celestial 音量协议。
- 宿主输出经 SDL2，解码经 FFmpeg（`libavcodec` / `libavformat` / `libswresample`）。

### 网络与设备状态

- BSD socket、路由 socket、BPF、内核控制（kernel control）、虚拟 UDP 栈、DNS-SD/mDNS IPC、Apple80211 Wi-Fi 状态机、SystemConfiguration（configd）协议。
- 三种网络策略：`isolated`（完全隔离）、`loopback`（仅回环）、`host`（透传宿主，默认）。
- CoreTelephony HLE 与基带字符设备；默认为"离线基带"档案，使原版 `CommCenter` 能完成启动而不会误报无线电故障，也支持基带流量的录制与回放（`--baseband-input` / `--baseband-output`）。
- 设备状态持久化：Lockdown 激活记录（`--activation`）、网络偏好设置 plist、HFS+ 卷档案。

### 文件系统

- 宿主目录直接作为客户机根文件系统，配合 HFS+ 元数据模拟：扩展属性、资源分支（resource fork）、文件标志、时间戳、`flock`/`fcntl` 文件锁、所有权。

---

## 架构概览

```
                    ┌──────────────────────────────────────────────┐
   客户机（原版      │  SpringBoard / 第三方 App / lockdownd / ...   │
   ARM 二进制）      │  UIKit · CoreGraphics · OpenGLES · CoreAudio  │
                    │  libSystem.B.dylib · dyld                     │
                    └───────┬────────────────────────┬─────────────┘
                            │ SVC 0x80               │ SVC 0xfa____
                            │（Darwin 系统调用）      │（框架 HLE 拦截）
   ┌────────────────────────▼────────────────────────▼─────────────┐
   │  CompatibilityKernel                                          │
   │  ├─ BSD 系统调用    ├─ Mach 陷入 / IPC / MIG                   │
   │  ├─ IOKit 服务      └─ UserlandHleRegistry（框架高层模拟）      │
   ├───────────────────────────────────────────────────────────────┤
   │  Dynarmic A32 JIT  ·  AddressSpace  ·  XnuScheduler           │
   ├───────────────────────────────────────────────────────────────┤
   │  宿主后端：SDL2 · Vulkan · FFmpeg · BSD socket · 宿主文件系统   │
   └───────────────────────────────────────────────────────────────┘
```

**框架 HLE 的工作方式**：`UserlandHleRegistry` 在 dyld 把镜像映射进客户机内存后，将目标符号的入口指令改写为一条位于私有命名空间（`0x00fa0000`）的 `SVC`。命中时由宿主处理函数接管，并且可以选择性地回退到固件原实现（`resume_original`）、尾调用另一个客户机函数，或在客户机函数返回后接续宿主逻辑。符号可按名字、按 Mach-O 虚拟地址，或按 Objective-C 1.x 的 类名 + selector 定位——因此不依赖某一版固件的硬编码地址。

---

## ABI 依据与参考实现

iLEmu 不是靠黑盒试错拼出来的兼容层：每一处内核语义都尽量追溯到 Apple 公开发布的源码，并在注释里标注出处，方便后来者复核。

| 参考来源 | 用途 |
| --- | --- |
| [**XNU `rel/xnu-1228`**](https://github.com/apple-oss-distributions/xnu/tree/rel/xnu-1228) | **主要内核参考。** Darwin 9 系列内核，与 iPhone OS 2.x–3.x 同源。BSD 系统调用语义、结构体布局（`stat64`、`statfs64`、`rlimit`、`proc_info` 等）、错误码与标志位处理均以此为准。 |
| [XNU `xnu-792.24.17`](https://github.com/apple-oss-distributions/xnu/tree/xnu-792.24.17) | Mach MIG 子系统的 `.defs` 基准（`mach_port`、`task`、`thread_act`、`vm_map`、`clock`、`semaphore`、`host_priv`、`processor_set`、`device`）。Mach 线格式在 Darwin 8/9 间保持稳定。 |
| launchd-257 | bootstrap / vproc 协议的 MIG 定义，用于与固件原版 `launchd` 通信。 |
| configd-137.3 | SystemConfiguration 的 MIG 定义，用于与网络配置栈通信。 |
| 固件二进制本身 | 部分私有框架（CoreSurface、LayerKit、GraphicsServices、IOMobileFramebuffer、MBX）没有开源对应物，其 ABI 由对固件二进制的分析得出，注释中会写明"confirmed from …"及具体固件版本。 |

需要强调的是：**参考的是内核语义，而不是内核代码**。iLEmu 没有移植、编译或链接任何 XNU 代码，它是一个从零编写的用户态兼容层——XNU 源码的作用相当于一份权威规格说明书，外加构建期用来生成 MIG 元数据的 `.defs` 文件。

---

## 环境要求

**平台**：Linux（代码使用 POSIX 接口，如 `poll(2)`；辅助脚本为 bash/sh）。

**工具链**：

- C++20 编译器（GCC 或 Clang）
- CMake ≥ 3.24
- pkg-config

**必需的系统库**：

| 依赖 | 用途 |
| --- | --- |
| libpng | 帧与纹理编解码 |
| Threads | 调度器与执行池 |

**可选但强烈建议的系统库**（缺失时对应功能会自动降级）：

| 依赖 | 用途 |
| --- | --- |
| SDL2 | 窗口显示、键鼠输入、音频输出 |
| Vulkan SDK（含 `glslc`） | GPU 加速的 GLES 后端 |
| FFmpeg（avcodec / avformat / avutil / swresample） | 客户机音频解码 |
| libplist-2.0 | 读写固件与设备状态 plist |

**需要手动放置的外部源码**：

| 路径 | 来源 |
| --- | --- |
| `external/dynarmic` | <https://github.com/MC-XiaoXiao/dynarmic> |
| `external/ext-boost` | <https://github.com/MC-XiaoXiao/ext-boost> |
| `external/vulkan-memory-allocator` | <https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator>（启用 Vulkan 时必需） |
| `sources/xnu` | [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu) @ `rel/xnu-1228` —— 主要内核参考，并提供 ARM 的 `machine_types.defs` |
| `sources/xnu-792.24.17` | 同一仓库的 `xnu-792.24.17` tag —— Mach MIG `.defs` 基准 |
| `sources/launchd-257` | Apple 开源 launchd（`protocol_jobmgr.defs` / `protocol_job.defs`） |
| `sources/configd-137.3` | Apple 开源 configd（`SystemConfiguration.fproj/config.defs`） |

> Dynarmic 与 ext-boost 必须使用上表中的 fork：主线 Dynarmic 不包含本项目所需的 ARMv6 相关改动。
>
> `sources/` 下的 Apple 开源代码只在**构建期**被 `ilemu_mig_id_gen` 读取 `.defs` 文件以生成 MIG 线格式元数据，一行 Apple 代码都不会被编译进产物。两份 XNU 检出缺一不可：`sources/xnu` 提供 ARM 类型定义，`sources/xnu-792.24.17` 提供 Mach 子系统定义。

---

## 构建

```bash
git clone <this-repo> iLEmu
cd iLEmu

# 外部依赖
mkdir -p external sources
git clone --depth 1 https://github.com/MC-XiaoXiao/dynarmic.git external/dynarmic
git clone --depth 1 https://github.com/MC-XiaoXiao/ext-boost.git external/ext-boost
git clone --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git \
    external/vulkan-memory-allocator

# 构建期需要的 Apple 开源定义文件
git clone -b rel/xnu-1228 --depth 1 https://github.com/apple-oss-distributions/xnu.git sources/xnu
git clone -b xnu-792.24.17 --depth 1 https://github.com/apple-oss-distributions/xnu.git \
    sources/xnu-792.24.17
# 再把 launchd-257 与 configd-137.3 解压到 sources/ 下

# 配置与编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j"$(nproc)"
```

产物为 `build/ilemu`。

可用的 CMake 选项：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `ILEMU_ENABLE_SDL2` | `ON` | 构建 SDL2 显示 / 输入 / 音频后端 |
| `ILEMU_ENABLE_VULKAN` | `ON` | 构建 Vulkan GLES 渲染后端 |
| `ILEMU_DYNARMIC_DIR` | `external/dynarmic` | Dynarmic 检出路径 |
| `ILEMU_BOOST_DIR` | `external/ext-boost` | ext-boost 检出路径 |
| `BUILD_TESTING` | `ON` | 构建测试套件（当前请置为 `OFF`，见[已知限制](#已知限制)） |

---

## 准备固件根文件系统

模拟器需要一份**已解密**的 iPhone OS 根文件系统。仓库不提供、也不会帮你下载或解密任何 Apple 固件——你必须自备合法获取的镜像。

先获取并构建两个第三方提取工具（它们不随仓库分发，需自行放到 `tools/` 下）：

```bash
# dmg2img —— 解出 DMG 中的 Apple_HFS/HFSX 分区
git clone --depth 1 https://github.com/Lekensteyn/dmg2img.git tools/dmg2img
make -C tools/dmg2img

# hfsfuse —— 带 resource fork / xattr 支持的 HFS+ 打包器
git clone --depth 1 https://github.com/0x09/hfsfuse.git tools/hfsfuse
make -C tools/hfsfuse WITH_LZVN=none hfstar
```

然后提取：

```bash
tools/extract_firmware.sh /path/to/decrypted-rootfs.dmg build/rootfs
```

脚本会自动定位唯一的 Apple_HFS/HFSX 分区（分区不唯一时可用第三个参数显式指定），并在解包时保留扩展属性与资源分支（资源分支以 `.ilemu-rsrc` 后缀落盘，由 HFS+ 元数据层还原给客户机）。

---

## 运行

```bash
# 无头引导，跑固定的指令预算，用于冒烟验证
./build/ilemu boot --rootfs build/rootfs --ticks 110000000

# 带窗口 + GPU 加速 + 交互控制台
./build/ilemu boot --rootfs build/rootfs \
    --display sdl --gpu --control-stdin
```

引导成功后 `launchd` 会按固件自身的 LaunchDaemons 配置拉起整套系统守护进程，最终由 `SpringBoard` 接管屏幕。此后可以在控制台里 `tap` 桌面图标来启动应用——App 的启动、前后台切换、场景归属与触摸路由都走固件原本的 SpringBoard 生命周期协议。

---

## 命令行参考

```
ilemu profile   [--output FILE]
ilemu inspect   --rootfs DIR [--binary /sbin/launchd] [--symbols SUBSTRING] [--output FILE]
ilemu disasm    --rootfs DIR --binary PATH (--symbol NAME | --address ADDR) [--count N] [--thumb]
ilemu boot      --rootfs DIR [选项...]
ilemu smoke     [--cores N] [--jit-cache-mib 8..128] [--perf-summary] [--output FILE]
ilemu benchmark arm [--iterations N] [--jit-cache-mib 8..128] [--perf-summary] [--output FILE]
```

| 子命令 | 作用 |
| --- | --- |
| `profile` | 打印当前模拟的设备档案（型号、SoC、主频、内存、屏幕尺寸） |
| `inspect` | 解析根文件系统里某个 Mach-O 的段、加载命令、入口点、动态链接器与符号 |
| `disasm` | 按符号名或地址反汇编客户机代码，支持 Thumb |
| `boot` | 启动模拟器（主要入口） |
| `smoke` | 不需要固件的自检，验证 CPU / JIT 链路是否正常 |
| `benchmark arm` | ARM 翻译执行吞吐基准测试 |

### `boot` 常用选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `--rootfs DIR` | *必填* | 客户机根文件系统目录 |
| `--binary PATH` | `/sbin/launchd` | 首个客户机进程 |
| `--ticks N` | 不限 | 客户机指令预算上限；省略则一直运行 |
| `--cores N` | `1` | 虚拟核心数（>1 为压力/开发模式） |
| `--jit-cache-mib N` | `64` | JIT 代码缓存大小，范围 8–128 |
| `--display headless\|sdl` | `headless` | 显示后端 |
| `--gpu` | 关 | 启用 GPU 加速，等价于 `--gles-backend vulkan` |
| `--gles-backend auto\|software\|vulkan` | `auto` | 显式选择 GLES 后端 |
| `--display-size WxH` | 设备档案值 | 覆盖客户机屏幕分辨率 |
| `--network isolated\|loopback\|host` | `host` | 网络策略 |
| `--activation activated\|unactivated\|preserve` | `activated` | Lockdown 激活状态 |
| `--host-cache DIR` | rootfs 同级的 `.ilegacysim-cache/<name>` | JIT 翻译档案与 Vulkan 管线缓存的存放位置 |
| `--control-stdin` | 关 | 从标准输入读取交互命令 |
| `--touch-replay FILE` | 关 | 回放触摸脚本 |
| `--frame-output FILE` | 关 | 把每一帧写入文件 |
| `--baseband-input / --baseband-output FILE` | 关 | 基带流量回放 / 录制 |
| `--gdb PORT` | 关 | 启动 GDB 远程调试服务 |
| `--watch-address ADDR` | 关 | 客户机内存写监视点 |
| `--perf-summary` | 关 | 退出时打印性能统计 |
| `--output FILE` | stdout | 日志输出目标 |

---

## 交互控制台

以 `--control-stdin` 启动后，可逐行输入命令（`#` 开头为注释）：

| 命令 | 说明 |
| --- | --- |
| `touch down\|move\|up\|cancel X Y` | 单个触摸事件 |
| `tap X Y [HOLD_MS]` | 一次完整点击 |
| `drag X1 Y1 X2 Y2 [DURATION_MS] [STEPS]` | 拖拽手势 |
| `unlock` | 执行锁屏解锁滑动 |
| `home` / `lock` | Home 键 / 锁屏键 |
| `volume-up` / `volume-down` | 音量键 |
| `ringer ring\|silent` | 静音拨片 |
| `snapshot PATH` | 立即截图 |
| `snapshot-sequence PREFIX INTERVAL_MS COUNT` | 定时连续截图 |
| `perf-begin LABEL` / `perf-end` | 划定性能采样窗口（需配合 `--perf-summary`） |
| `status` | 打印帧数、进程数、线程数、可运行线程数、前台进程、屏幕电源状态 |
| `help` / `quit` | 帮助 / 退出 |

---

## 调试与性能分析

- **GDB 远程调试**：`--gdb PORT` 启动 RSP 服务端，支持多进程多线程、断点、单步、寄存器与内存读写。
- **静态分析**：`inspect` 查看 Mach-O 结构与符号，`disasm` 按符号或地址反汇编。
- **内存监视**：`--watch-address ADDR` 在客户机写入指定地址时触发回调。
- **性能计数**：`--perf-summary` 输出整体统计；配合控制台的 `perf-begin` / `perf-end` 可以只统计某个交互窗口（例如"从点击图标到首帧"）。
- **确定性回放**：`--touch-replay` + `--frame-output` + `--ticks` 组合可以得到可复现的图形回归用例。

---

## 目录结构

```
app/            命令行前端、SDL 音频输出、实时控制、实时节拍器
include/ilemu/  公共头文件（ABI 定义、各子系统接口）
src/
  foundation/   CPU、地址空间、Mach-O 与进程加载、JIT 档案、用户态 HLE 框架
  mach/         Mach 命名空间、XNU 调度器、MIG 适配层
  kernel/
    bsd/        BSD 系统调用（文件、进程、信号、socket、事件、AIO、BPF…）
    mach/       Mach 陷入、消息、端口权限、任务/线程、VM、GraphicsServices 输入
    iokit/      IOKit 设备服务（显示、MBX、音频、摄像头、JPEG、基带、电源…）
  graphics/     GLES HLE、软件光栅器、Vulkan 后端、CoreSurface、LayerKit、SDL 显示
  media/        CoreAudio / AudioToolbox / Celestial HLE、JPEG 编码
  network/      宿主 socket 桥接、路由 socket、Apple80211、DNS 配置
  telephony/    CoreTelephony HLE
  filesystem/   HFS+ 元数据与卷档案
  device_state/ Lockdown、网络偏好、内核档案
  debug/        GDB RSP 服务端
tools/
  mig/          从 Apple .defs 生成 MIG 线格式元数据
  guest/        客户机侧探针程序（需要 ARMv6 交叉工具链）
  *.sh          固件提取、ARMv6 ld64 引导脚本
tests/          Mach / 内核 / 图形 / 网络 / 文件系统 单元与集成测试
```

---

## 已知限制

这是一个进行中的项目，以下是目前明确的短板：

- **系统调用覆盖不完整**。BSD 调用表只实现了固件启动与已测应用实际用到的部分；未实现的调用会打印诊断并返回 `ENOSYS`，因此依赖冷门系统调用的程序可能功能缺失或直接退出。
- **图形界面路由不完善**。多场景、后台应用、方向旋转、状态栏与部分合成路径的归属判定仍有缺口，可能出现画面错位、丢帧或前后台切换后不刷新。
- **音频系统不完善**。播放链路已经跑通，但格式覆盖、时序精度、混音与录音路径都还不完整。
- **网络模块不完善**。socket 与 DNS 的常见路径可用，更完整的 SystemConfiguration 状态机、Wi-Fi 关联流程与部分协议族仍在补齐。
- **应用兼容性有限**。目前只有少量第三方应用验证过（*Angry Birds* 可运行）。
- **仅支持 Linux 宿主**。代码直接使用 POSIX 接口，尚未做 Windows / macOS 适配。
- **设备档案单一**。内置档案为 iPhone1,1（320×480、128 MB、单核 ARM1176）；多设备/多分辨率支持尚未成型。
- **Darwin 契约封顶在 9.4.0**。模拟器向客户机声明的内核版本上限是 `darwin9.4/RELEASE_ARM`；更晚的固件所需的系统调用与 ABI 变更尚未实现。
- **测试套件暂时无法构建**。`tests/` 仍引用项目改名前的 CMake 目标 `iLegacySim::core`，配置阶段会失败；构建时请加 `-DBUILD_TESTING=OFF`。

---

## 路线图

- 补全 BSD 系统调用与 Mach MIG 子系统覆盖
- 完善合成器与场景路由，稳定多应用前后台切换
- 打通完整音频管线（格式、时序、录音）
- 完善 SystemConfiguration / Wi-Fi 状态机与协议族支持
- 扩展设备档案，支持更多机型与固件版本
- 修复测试套件目标名，恢复 CI

---

## 法律声明

iLEmu **不包含、不分发任何 Apple 固件、系统框架或专有二进制**。运行本模拟器需要用户自行提供合法获取并已解密的 iPhone OS 根文件系统。

构建期读取的 XNU、launchd、configd 源码均来自 Apple 公开发布的开源代码（[apple-oss-distributions](https://github.com/apple-oss-distributions)，以 Apple Public Source License 2.0 发布），且仅被 `ilemu_mig_id_gen` 用于从 `.defs` 文件生成 ABI 元数据——这些源码不会被编译进产物，也不随本仓库分发。

本项目面向操作系统与二进制翻译方向的研究、复古计算与互操作性实践。使用者需自行确保其行为符合所在司法辖区的法律以及相关授权条款。

---

## 许可证

*待定 —— 请在此补充项目的开源许可证。*
