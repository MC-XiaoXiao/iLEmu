# 伪 XNU 改造记录（2026-11-16 起）

本记录对应分支 `perf/pseudo-kernel-xnu-20261116`。阶段 5 的 Mach IPC 锁细化、阶段 6 的 FileTable/kqueue、阶段 7 的 GuestCopyWindow/radix/fork COW 均已完成，Release/IPO 构建、定向回归和最终六轮 120 秒固件验收通过。最终中位数显示事件处理成本降低，第 151 个请求提前，但早期启动时间略增、RSS 增加；具体数字、工作量差异及语义边界均在下文保留。

代码与测试仅写入 `/home/xxiao/Projects/Project_iLEmu/.worktrees/pseudo-kernel-xnu-20261116/work_repository`，构建及日志保存在其上层工作树目录。最后将本文单独提交到主仓库 `docs/`；用户已有的键盘/输入文件和 dynarmic 子模块改动保持原样。XNU 源码及原始固件只读，未使用 stash、reset 或 push。

## 提交与实现

| 阶段 | 提交 | 结果与边界 |
| --- | --- | --- |
| 0 | `8ea9aa8` | 修复 spawn 参数在异步 image preparation 与 completion capture 之间的 move 求值问题，恢复 launchctl/fsck 启动链。 |
| 1 | `92eae47` | 提前处理时间/ARM fast trap，门控 HID、WiFi、userland HLE；进程 incarnation 在 fork/exec 更新；补齐共享 deadline 生产者失效。每次 SVC 的本地 deadline 失效随后在 3d 删除。 |
| 2 | `70b14fc` | 缩小同步及信号唤醒范围，删除进程级 Mach receive 门控，改按实际 reply port 判断。剩余设备和 fd 的全局 IO generation 随 3c 删除。 |
| 3a | `114727d` | 引入 WaitRecord、对象 waitq、completion queue 和 deadline；迁移 semaphore、psynch、ulock。 |
| 3b | `5e89c98` | 迁移 wait4、flock/record lock、timer、signal wait、file IO/AIO、image preparation、page fault。 |
| 3c | `c0f9763` | 迁移 socket/pipe/select/poll/kevent/Mach receive；删除全局 IO generation、pending-map 轮询和 observation 缓存。 |
| 3d | `ab25bb7` | 清理 deadline 兼容路径；按实际生产者失效，补齐 interval/audio/HID/WiFi/workqueue 来源。 |
| 4 | `6618979` | 信号生产者取消目标可中断等待，对目标 CPU 请求 AST；保留已选中的成功 completion 和 Mach handoff。 |
| 5 | `1ef8dc9` | Mach 名字数组索引、MIG bucket 路由、port-owned queue、设备 observer side table。该提交仍保留全局 IPC 事务锁，不能据此认定锁细化完成。 |
| 3 补充 | `d0ff05e` | 将 timer wait 统一到串行内核边界。 |
| 6 | `4b53db1` | FileTable/FileGlob 所有权、生产者驱动的 KNOTE 活动队列及 fd 调用点完整迁移。 |
| 5 锁细化 | `d241b2b` | task namespace 与 port/mqueue 事务局部化，移除普通 IPC 路径的全局事务锁。 |
| 7 | 本次 VM 提交 | GuestCopyWindow、共享 radix leaf、按 region 标记 fork COW 与 delayed Storage shadow；附最终重复验收。 |

### 阶段 0：启动回归根因

`posix_spawn` 的 image preparation 需要一份参数，异步 completion 也需要保留参数。在同一函数调用的不同参数中同时读取和 move 同一个参数容器，使 image preparation 可能拿到已被搬空的参数。launchctl 因而没有按 `bootstrap -S System` 执行，后续 fsck、launchd fork 链没有发生。

修复先显式复制 `image_arguments`，再分别把 image 参数和 completion 所有权 move 到各自位置。当前实现位于 `src/kernel/bsd/process/spawn.cpp`。这是 C++ 求值和所有权错误；没有通过更改 guest syscall 行为或添加回退路径掩盖。用户复核已经确认根因和修法。阶段 0–2 的原始逐轮数字未在本文引用的现存测量文件中完整保留，以下性能对照从修复后的独立 pristine 基线开始。

### 阶段 1、2：缩短入口并限定失效范围

`mach_absolute_time` 在常规内核入口锁及 HLE 探测之前完成；ARM fast trap 也提前解码，但保留必要的 syscall-return AST 检查。HID、WiFi 只在有待处理事件时探测，userland HLE 按立即数/地址命中条件调度。incarnation 在进程创建或替换镜像时写入，删除每次 SVC 的共享进程表查询。

共享 deadline 的 generation 在 mk_timer arm/cancel/destroy、camera capture、console、backlight、vsync 等实际修改点更新。阶段 1 初始实现仍保留每次 SVC 的本地失效；3d 删除这项调用，使用各本地来源自己的 generation。

Mach RPC 不再因同进程其他线程存在挂起 receive 而一律进入慢路径。目标/provider 校验针对具体 reply port。同步唤醒先缩小到目标线程，随后由阶段 3 的对象事件彻底取代剩余全局 IO generation。

### 阶段 3：生产者完成等待，运行时消费结果

每线程一个 `WaitRecord` 记录类型、事件键、generation、deadline、结果和 variant continuation。对象 waitq 以 `WaitEvent{kind, object}` 查找 intrusive 等待者；生产者在同一串行内核边界先写结果及 continuation 数据，再入 completion-ready 队列。ulock 按优先级维护候选集合，调度优先级变化直接更新排序。

子进程状态变化唤醒 wait4，解锁唤醒 flock/record-lock 等待者。host worker 只发布保留所有权的 completion token；串行内核边界消费 token、到期 deadline 和 ready continuation。socket、Mach、HLE、graphics、HID 不再依赖全表重判条件。已授予的 send/receive 和 Mach handoff 保持完成优先级。

`WaitQueue`、`WaitCompletionQueue`、线程记录的访问规则统一为串行内核边界：单 CPU 为 Immediate，多 CPU 为 Deferred。`wait_queue.hpp` 明确记载该约束，host worker 不直接修改等待记录。`has_pending_event_locked` 只检查 WaitKind、userland HLE 和 exception delivery。进程级 `waiting_for_events`、pending-map 扫描、全局 IO generation 和 observation 缓存已经删除。

带超时等待登记到 DeadlineQueue，完成/取消精确移除。`timer_deadline_snapshot` 的本地部分仅合并 wait completion 和具体设备 deadline；interval timer 归属进程。ERESTART 的 ARM 4 字节、Thumb 2 字节 PC 恢复只保留在 `bsd/signal/interruption.cpp`。

### 阶段 4：目标线程的信号取消与 AST

捕获信号被选择时，生产者立即清除目标的可中断等待，保留 continuation 到目标边界消费。锁、psynch、ulock 等候选从等待对象解绑，deadline 随之取消；具体等待类型决定中断结果及是否 ERESTART。exception reply 等不可中断等待继续等待。已选中的成功 file completion、Mach MessageReady 和 receive handoff 不被后来的信号覆盖。

运行中的目标通过自己的 runtime CPU 执行 `request_guest_preemption()`；修改 mask 使其他线程的 pending signal 可见时，同样对目标 CPU 请求。发送线程不会代替目标修改寄存器。sigwait 的匹配直接唤醒选择的线程。

### 阶段 5：名字解析、kobject 路由及队列归属

每 task 的 Mach namespace 使用按 name index 直接索引的 entry vector，并验证 generation，保留 alias、rights 计数和名称复用语义。kobject 路由使用静态 256-bucket MIG routine 表；目标只解析一次并传给 provider，由 provider 校验对象类型；包含 context 3228/3229、VM 4800/4801/4802 的别名。

消息及 receive 索引归属 port/port set，删除全局 `mach_queues`。访问队列时持有 port mutex，先选取/移除 receive，再释放队列锁进行 rights copyout，避免重入 notification 时仍持有队列锁。设备字段移动到以消息身份为键的 observer side table；通用 send 不再解释 audio/graphics 协议。

`1ef8dc9` 仍以 `mach_mutex` 保护 task rights、端口拓扑和 receive-ticket 事务，部分 port mutex 在此锁内取得。对象所有权的完成不等于全局串行化已消除。以下记录 `d241b2b` 完成的锁细化、锁顺序和验证结果。

<!-- STAGE5_LOCK_REFINEMENT -->

阶段 5 锁细化补充：`NamespaceTable` 将 registry 和每个 task 的稳定 Space 分开；`LockedSpace` 保留 Space 所有权，多 task 按 PID 排序去重上锁，销毁后旧句柄不会附着到复用同一 PID 的新 Space。rights 的 lookup/change/contains 在同一 namespace 临界区完成，再释放 guard 产生通知或访问其他 namespace。对应 XNU `ipc_space.c:93` 的空间归属。

每个 PortObject 与 IpcMqueue 共享 port mutex，短 registry 锁只取稳定对象；端口元数据、request table、FIFO 发送额度及 receive ticket 各自有明确归属。port set 保有 ready member 索引，生产者按 port→set 顺序发布；set 接收反向 try-lock 失败即释放并重试，不持 set 锁阻塞等待 port。receive 选取、移除和结果先提交，释放队列锁后再发布唤醒、rights copyout 或通知。拓扑校验先快照 ticket，再在队列锁外按 namespace→port 完成，消除旧的 port→namespace 反向路径。重复 deactivate、空 remove 和没有新增额度的 grant 不再递增 generation 或产生唤醒。

普通 Mach send/receive、MIG、task/thread/VM 与名字解析 trap 已移除全局 `mach_mutex` 事务锁。串行内核边界继续拥有跨对象生命周期；宿主生产者只向保留的 mqueue 发布消息。线程 promotion、信号量状态、timer、graphics/device observer 仍使用各自既有保护域，设备观察仅在短作用域内持 `mach_mutex`。删除重复的直接 MIG response 编码及旧 receive-wait 路径，统一使用排队 transport。

namespace/mqueue/port 定向测试覆盖不同对象并发、反向多空间锁序、销毁/重建与旧句柄、FIFO admission、精确 generation 和回调重入。独立 Release/IPO 构建（`../goal-stage5-build`）及 Mach transport、FileTable、multiplex、workqueue、HID、exception 产品链接回归全部通过；namespace/routes/mqueue/ports 独立套件也通过。日志为 `../runs/goal-stage5-build-{fixed,final}.log`、`goal-stage5-{transport,bundle,mach-suite}.log`。新增的 Release unused 与 mqueue aggregate 告警已修复，其余告警位于既有代码。

`goal-stage5-iphone51-1` 120 秒正常退出，ready 请求 175，fork/vfork=95/4，spawn-setexec=90，svc=480924，cpu-ticks=8526265543，fsck/SpringBoard/151 请求=4.092/15.468/51.845 秒，RSS 峰值 4299712 KiB。对照保存基线的 98/4、93，差额是 CommCenter、backupd 和 mDNSResponderHelper 各一次；没有缺少基线服务名，另有三个 itunescloudd APS 名称。共同的 assistantd SIGKILL 和 fud SIGSEGV 仍在，未把工作量差异解释为确定提速。kernel-event-poll p95 均为 10 us 桶；event-delivery 样本由基线 2940945 降至 58923，说明来源驱动的完成减少了无效尝试。此处为单轮验证，最终统一参数中位数另列。

## XNU 语义对应

以下行号来自只读 `/home/xxiao/Projects/Project_iLEmu/sources/xnu-2050.48.11`，对应 iOS 6。iLEmu 采用宿主串行内核边界和 C++ 对象所有权，借鉴状态转移及归属语义，没有照搬内核线程锁实现。

| 改造 | XNU 源码依据 | 采用的语义 |
| --- | --- | --- |
| 对象等待队列 | `osfmk/kern/wait_queue.c:1268`、`:1817` | assert-wait 与按事件选择等待者。 |
| 完成与超时 | `osfmk/kern/sched_prim.c:1490`、`:1539`、`:772` | `clear_wait` 在 setrun 前发布结果，timer 到期完成等待。 |
| 子进程与文件锁 | `bsd/kern/kern_exit.c:1068`；`bsd/kern/kern_lockf.c:1172` | 子进程转换唤醒父进程；`lf_wakelock` 在释放锁时唤醒候选。 |
| 信号 | `bsd/kern/kern_sig.c:279`、`:2142`、`:2162` | `signal_setast`/`act_set_astbsd` 作用于目标，`thread_abort_safely` 中断安全等待。 |
| Mach 名字 | `osfmk/ipc/ipc_entry.c:93` | `MACH_PORT_INDEX` 索引 entry，再验证 generation。 |
| kobject 路由 | `osfmk/kern/ipc_kobject.c:168`、`:268` | `mig_buckets` 查找 routine，provider 负责对象转换。 |
| port 消息队列 | `osfmk/ipc/ipc_mqueue.c:492` | port-owned queue 与 `ipc_mqueue_post` 的直接 receiver handoff。 |
| fd 所有权（阶段 6） | `bsd/sys/file_internal.h:90`、`:141` | 独立 fileproc flags，共享 fileglob offset/status/资源。 |
| knote（阶段 6） | `bsd/kern/kern_event.c:2399`、`:2624`、`:2051`、`:2503` | 生产者激活活动队列、扫描活动项、按 fd klist detach。 |
| fork COW（阶段 7） | `osfmk/vm/vm_map.c:8483`、`:8586` | 按 entry 标记 needs-copy，写时分离存储。 |

## 固件测量与解释

以下均使用 iPhone5,1 iOS 6 10A405、pristine rootfs 的独立可写副本、120 秒无头启动；测量期间没有并发构建、链接或测试。原 iPhone5,1 rootfs 曾被删除，用户授权重新解压后，完整 pristine 存于 `build/validation/iphone51-pristine/rootfs`。没有将残缺 rootfs 作为功能基线。

三个计时分别是首次记录到的 fsck `vfork parent=2`、SpringBoard `spawn-setexec`、第 151 个不同 bootstrap check-in 请求。计时由 host 读取日志时记录，受日志刷新和主机调度影响。

`checked-in-services` 集合在 bootstrap check-in 请求发送时更新，不能解释成已收到成功 reply 的服务数。`SessionDiagnostics::stopped()` 在运行结束输出 `service-state=ready`，其条件是集合非空；它不是运行中达到 151 的实时成功里程碑。下表保留日志的最终不同请求计数。

| 运行标签（均位于 `build/runs/`） | 最终请求数 | fork/vfork/spawn-setexec | fsck / SpringBoard / 151 请求（秒） | 解释 |
| --- | ---: | --- | --- | --- |
| `clean-baseline-iphone51-1` | 171 | 92/4/87 | 2.626 / 11.278 / 41.856 | 修复后基线；缺三个 APS 注册。 |
| `clean-baseline-iphone51-2` | 174 | 92/4/87 | 2.526 / 11.015 / 39.924 | 完整工作量基线样本。 |
| `profile-baseline-iphone51-3` | 171 | 92/4/87 | 2.598 / 11.312 / 42.509 | 保留的启动波动，不能当完整通过样本。 |
| `clean-stage3a-iphone51-1` | 174 | 92/4/87 | 2.456 / 10.723 / 38.516 | 单轮功能样本。 |
| `clean-stage3b-iphone51-1` | 174 | 92/4/87 | 2.606 / 11.211 / 38.648 | 单轮功能样本。 |
| `clean-stage3c-iphone51-1` | 174 | 92/4/87 | 2.446 / 11.964 / 40.792 | SpringBoard 时间存在波动。 |
| `clean-stage3d-iphone51-1` | 171 | 92/4/87 | 2.484 / 10.649 / 38.631 | 同样缺三个 APS 注册。 |
| `clean-stage3d-iphone51-2` | 174 | 92/4/87 | 2.516 / 10.948 / 36.466 | 保持 92/4 工作量。 |
| `clean-stage3d-iphone51-3` | 174 | 90/4/85 | 2.617 / 11.045 / 36.387 | launchd 少两次 retry，非相同工作量。 |
| `profile-stage3d-iphone51-4` | 174 | 92/4/87 | 2.497 / 10.866 / 38.244 | 保持 92/4 工作量。 |
| `profile-stage4-iphone51-1` | 174 | 92/4/87 | 3.043 / 13.002 / 46.290 | 保持原工作量。 |
| `profile-stage4-iphone51-2` | 175 | 94/4/89 | 2.791 / 13.175 / 46.226 | 含额外 retry。 |
| `profile-baseline-iphone51-4` | 175 | 100/4/95 | 2.893 / 13.561 / 52.569 | 含更多 retry，不能直接配对比较。 |
| `profile-stage4-iphone51-3` | 156 | 91/4/86 | 2.854 / 22.112 / 106.615 | 120 秒内工作量未完成。 |
| `profile-baseline-iphone51-5` | 140 | 86/4/81 | 4.480 / 31.276 / 未达到 | 同时段基线也明显变慢。 |
| `profile-stage5-iphone51-2` | 174 | 92/4/87 | 2.964 / 12.020 / 45.985 | 独立运行。 |
| `profile-stage5-iphone51-3` | 174 | 92/4/87 | 2.485 / 10.171 / 39.125 | 队列锁归属修改后。 |
| `profile-stage5-iphone51-4` | 174 | 92/4/87 | 2.772 / 13.685 / 45.605 | failed-copyout 清理后的最终运行。 |

`profile-stage5-iphone51-4.log:201469` 确实输出 `service-state=ready checked-in-services=174`；阶段 5 提交说明中“没有 emitted ready milestone”的说法不准确。该行仍受上述结束汇总语义限制。其前一行 display attribution 记录 629 submitted frames；首次记录的 scanout 是 frame 262，不能当真正第一帧。

关键 perf 汇总如下。`scheduler-wake` 保留原始双计数格式，未将其合并为单一数字。表内计数属于整轮 perf 汇总，后文 profile window 的采样数量属于另一统计窗口。

| 运行 | svc | cpu-ticks | dispatch | wake | block |
| --- | ---: | ---: | ---: | --- | ---: |
| baseline 1 | 467834 | 8503570928 | 127361 | 68147/96 | 68602 |
| 3a | 490392 | 8455639967 | 130747 | 69770/96 | 70226 |
| 3b | 472535 | 8480682368 | 131639 | 68493/99 | 68956 |
| 3c | 471588 | 8492440843 | 132103 | 68565/76 | 69042 |
| 3d 2 | 479624 | 8475240507 | 128405 | 68836/88 | 69309 |
| 3d 4 | 472169 | 8493982232 | 130083 | 68774/79 | 69253 |
| 4 1 | 470452 | 8486090718 | 137927 | 67836/86 | 68309 |
| 4 2 | 472481 | 8553538803 | 138709 | 68650/85 | 69143 |
| baseline 4 | 513187 | 8838826970 | 149114 | 72983/94 | 73506 |
| 4 3 | 360411 | 5515969002 | 151111 | 47836/76 | 48285 |
| baseline 5 | 225031 | 3842380799 | 141187 | 31946/49 | 32380 |
| 5 2 | 470655 | 8483082710 | 135763 | 68180/81 | 68654 |
| 5 3 | 483914 | 8466705680 | 130944 | 68932/82 | 69407 |
| 5 4 | 468049 | 8474713466 | 138567 | 68013/86 | 68481 |

阶段 3d 提交说明将三个早期基线与四个 3d 运行汇总后，SpringBoard 中位数为 11.278 → 10.907 秒，151 请求为 41.856 → 37.355 秒；svc 为 467834 → 474618.5，cpu-ticks 为 8477090225 → 8484611369.5。该汇总混有 171 请求和 90 forks 样本，只是全部观测值的描述，不能作严格相同工作量性能验收。

profile baseline 3 与 3d 4 的 event-delivery 样本为 3040931 → 59007，host-event p95 为 0.21 → 0.11 ms，CPU execution p95 为 1.61 → 1.54 ms；JIT demand translation 仍各约 10 秒。这表明移除了大量等待条件重判，guest 指令和 syscall 工作量没有同步减少，JIT 仍占主要启动开销。baseline 的 itunesstored worker 还出现 Foundation `ISUniqueOperationLock` 与 Mach RPC 等待；3d 1 在 mDNS handshake 和 Mach timer reply 已交付后仍在 psynch 等待，三个 APS 注册的波动未被删出数据。

阶段 4 的 JIT translation 总时长从前两轮 12.267/12.401 秒升到第三轮 18.207 秒；相邻 baseline 4/5 为 14.158/18.437 秒，CPU execution p95 同时变差。主机负载及内存压力能解释部分漂移，但这些数据不足以证明性能等价、稳定提速或首帧不退化。需要在资源稳定、相同工作量条件下重复对照。

<!-- STAGE6_RESULTS -->

## 阶段 6：fd 与 kqueue

FileTable 用 vector 索引 FileProc；FileProc 保有独立的 descriptor flags 和 guard，FileGlob 共享 offset、status flags、资源 variant 与 fd knote klist。dup、fork、SCM_RIGHTS 和 fileport 保留同一 FileGlob，关闭一个别名不会关闭其他别名或排队中的权利。按 XNU `kern_descrip.c` 的 fdcopy 语义，fork 不继承普通 kqueue，进程私有 workqueue 也不继承。标准输入输出现在是实际 FileProc，关闭后可复用 0–2。原来的 duplicated_descriptors_ 及各资源平行 fd map 已删除；read/write、fcntl、ioctl、mmap、文件锁、socket 和进程清理均经 FileTable。

KnoteQueue 用 (ident, filter, udata-specific/udata) 哈希注册项，链表保持注册身份；来源的 IoEventSource/HostSocket subscription 在真实变化时将对应 key 放入活动队列。kevent 扫描活动项，按 EV_CLEAR、EV_DISPATCH、EV_ONESHOT 和 deferred-delete 转移状态。fd close 沿 FileGlob 的 klist 精确 detach 当前进程、当前 fd 的 knote；dup 的其他 fd 不受影响。timer 和 QoS bucket 分别维护 deadline 索引，嵌套队列通过源事件传递变化，并拒绝环。对应 XNU `file_internal.h:90/141`、`kern_event.c:2399/2624/2051/2503`。

`4b53db1` 保存了阶段 6 的完整迁移；结构、fd 调用点和活动队列相互依赖，作为一个可构建提交保存。附带 guarded-open 的异步 namespace 等待修复：保留原始 guarded syscall 和参数，完成等待后重新执行 guarded-open，避免以普通 open 的中间寄存器安装 guard。产品链接回归覆盖独立 flags、共享 offset/status、传递与关闭、fd 复用、guarded-open 等待、EV_*、process/timer、嵌套队列、select/poll 和 pthread workqueue。

2026-11-18 收尾验证：上层 `../build` Release/IPO 构建成功，三个产品链接回归 `tests/kernel/{file_table_kevent,multiplex_waits,workqueue_events}.cpp` 均 exit 0。日志位于 `../runs/goal-stage6-build.log`、`goal-file-table-kevent.log`、`goal-multiplex-waits.log`、`goal-workqueue-events.log`。原有 graphics、clock、psynch 和 fmt IPO 警告仍存在，未据此声称全项目零警告。

`../runs/goal-stage6-iphone51-1` 使用工作树内 pristine 10A405 的独立副本，120 秒正常退出，最终 ready 172；fork/vfork=96/4、spawn-setexec=91、svc=458779、cpu-ticks=8448662759，fsck/SpringBoard/151 个不同 check-in 请求为 5.659/19.194/59.285 秒。相邻 `goal-baseline-iphone51-1`（保存的 8ea9aa8）也到达相同的 172 个服务名，98/4、93、494728、8574189825，时间为 3.371/17.691/67.147 秒；基线多启动 backupd 和 CommCenter 各一次。两者都有 assistantd SIGKILL 和 fud SIGSEGV，基线另有 backupd SIGILL。这些是实际工作量差异，不能宣称 fork 计数完全相等或据单轮证明提速。

首轮阶段 6 只启用 perf-summary，没有 kernel-event-poll 延迟样本；基线启用 perf-cpu-phases 后 p95 为 10 us（直方图桶）。因此这两轮只作功能/差异记录，不作延迟或内存的配对性能结论。下文最终验收已统一参数、记录 RSS、各重复 3 次取中位数；请求集合时间不等同于服务成功 check-in 的精确时刻。首选主仓库 iPhone5,1 rootfs 缺 launchd/SystemVersion.plist，未修改它；本次使用的是工作树内已有完整 pristine 快照。

阶段 6 提交后复测 `goal-stage6-iphone51-2` 正常退出：175 请求、96/4 forks/vforks、91 次 spawn-setexec、svc=475004、cpu-ticks=8553844869、fsck/SpringBoard/151 请求=2.711/15.437/50.793 秒、RSS=4380092 KiB；统一启用 phases 后 event-poll p95=10 us 桶。

阶段 5 锁细化提交 `d241b2b` 后复测 `goal-stage5-iphone51-2` 也正常退出：174 请求、92/4、87 次 spawn-setexec、svc=473448、cpu-ticks=8524708743、fsck/SpringBoard/151 请求=2.593/12.579/43.732 秒、RSS=4425132 KiB，event-poll p95=10 us 桶。本轮没有 assistantd SIGKILL；相较保存基线，少了 assistantd/ReportCrash 和一次 securityd，以及 CommCenter、backupd、mDNSResponderHelper 各一次启动。缺少的服务名只有 ReportCrash.SimulateCrash，另有三个 APS 名称；fud SIGSEGV 仍在。未把单轮较少的 fork 当作优化效果。

<!-- STAGE7_RESULTS -->

## 阶段 7：VM

`GuestCopyWindow` 在作用域内保有 AddressSpace::ExclusiveAccess，checked copy 按页 memcpy，并复用已持有的 gate；保留权限、缺页、COW、写代数和 user-copy 的成功前缀。Mach 24 字节头一次导入，iovec 一次导入后按窗口输出，stat/stat64/statfs 与 IOKit reply 先编码字节缓冲再拷贝。kevent 变化和 receipt 在一个窗口内逐项处理；普通事件/workqueue 输出共用一个窗口，但每条事件仍先成功 copyout 再执行 EV_CLEAR、EV_DISPATCH、EV_ONESHOT 转移。`kern_event.c:1409/1418` 的逐项故障边界保留，避免先消费整批事件后才发现输出地址无效。

resident 页表替换为 32 位 1024×1024 两级 radix；occupied bitmap 跳过空槽，leaf 元数据本身也能共享。fork 按 VmMap region 标记 Copy 区间，只撤销已有 JIT 写入口；子进程共享 radix leaf 和 backing，第一次修改 leaf 时才分离元数据，第一次写页时才分离字节。VM_INHERIT_SHARE 保留逻辑对象，VM_INHERIT_NONE 从子进程移除；私有 file mapping 保持未缺页状态。

既有共享逻辑对象需要与其别名继续共享写入，又要与 fork 的私有快照隔离，因此 GuestPageBacking 将逻辑对象和 Storage 分开。fork、vm_map/remap(copy=TRUE) 和 VM_PROT_COPY 建立共享 Storage 的 delayed shadow；源对象或 shadow 第一次写时复制 Storage，同一次快照内的别名复用同一 shadow。共享写跟踪 epoch 撤销所有相关地址空间的旧 JIT 指针，checked/instruction 访问持有物理锁。对应 `vm_map.c:8483/8586` 的 needs-copy，以及 `vm_map.c:7929`、`vm_object.c:3475–3507/3817` 的 delayed copy。没有按 true_share 标记直接深拷贝整页。

共享文件区间在 fork 前仍需 page-in，以保留其他 mapping/descriptor 写入前的内容；这一步不复制 snapshot 字节，但不是完全 lazy 的共享 vnode 快照。去掉这些 page-in 还需要 vnode 版本快照或在宿主写前拦截；本次不声称实现了该机制。

定向测试覆盖 radix 边界、128 个私有页跨 leaf 的零写入 fork、首次/重复写入复制数、LDREX/失败 CAS 不复制、私有文件懒缺页、多代快照、共享内外部别名、descriptor 可见性、JIT 指针撤销、named-memory copy 和 VM_PROT_COPY。完整 Release/IPO 产品构建通过（`../runs/goal-stage7-build{,-final}.log`）；Mach transport、FileTable、multiplex、workqueue、kevent copy fault、radix fork、GuestCopyWindow、HID、exception 九组产品库回归全部通过（`goal-stage7-bundle-final.log`）。新增改动行无编译告警，既有 shared-region signedness、ProcessRecord/DarwinWorkqueueItem aggregate 与 fmt IPO 告警仍在。最终重复固件结果见下文。

## 最终重复验收（2026-11-18）

基线为保存的 `8ea9aa8` 可执行文件 `../ilemu-baseline`，最终版本为验收时构建的阶段 7 `../build/ilemu`。按 final-1、baseline-1、final-2、baseline-2、final-3、baseline-3 交替运行，每轮使用同一 pristine 10A405 rootfs 的新副本。六轮均在 120 秒后经 control-stdin 请求退出，exit 0，完成 fsck、SpringBoard exec 和至少 151 个不同 check-in 请求；结束汇总为 ready。所有轮次没有并发模拟器、构建、链接或回归测试，swap 均为 0，最低 MemAvailable 为 9488 MiB，没有触发 memory-stop。

参数统一为 `--device iPhone5,1 --activation activated --network isolated --display headless --gles-backend vulkan --control-stdin --jit-profile-mode off --jit-catalog-warming no-enqueue --perf-summary --perf-cpu-phases`。驱动在约第 1/119 秒发出 perf-begin/perf-end；下表 latency 与 phase 总时长取该采样窗口，svc、cpu-ticks 和最终请求数取整轮汇总。RSS 通过 `/proc/<pid>/status` 采样，匿名/文件分类每秒保留一次；分类样本取峰值附近，未将它们假定为同一时刻的精确峰值。

| 指标（各 3 轮中位数） | 基线 `8ea9aa8` | 最终版本 | 变化 |
| --- | ---: | ---: | ---: |
| svc | 477459 | 474929 | -0.53% |
| cpu-ticks | 8545539719 | 8545332128 | -0.0024% |
| fsck | 2.778 s | 2.831 s | +1.92% |
| SpringBoard exec | 13.150 s | 13.304 s | +1.17% |
| 第 151 个不同请求 | 49.540 s | 46.011 s | -7.12% |
| kernel-event-poll p95 | 10 us | 10 us | 同一桶 |
| kernel-event-poll 总时间 | 4.343 s | 2.319 s | -46.59% |
| kernel-event-delivery 总时间 | 3.741 s | 0.868 s | -76.79% |
| runtime-host-events 总时间 | 11.889 s | 5.854 s | -50.77% |
| CPU execution 总时间 | 57.810 s | 59.104 s | +2.24% |
| JIT translation 总时间 | 11.721 s | 11.934 s | +1.81% |
| process-clone-memory p95 | 510 us | 100 us | -80.39% |
| RSS 峰值 | 4168.445 MiB | 4361.863 MiB | +4.64% |
| RSS 匿名部分（峰值附近） | 4039.906 MiB | 4237.824 MiB | +4.90% |
| JIT shared committed | 1083.801 MiB | 1083.605 MiB | -0.02% |
| JIT cache peak | 1319.492 MiB | 1327.762 MiB | +0.63% |

| 运行 | fork/vfork/spawn-setexec | 请求数 | svc | cpu-ticks | fsck / SpringBoard / 151 请求（秒） | RSS（MiB） |
| --- | --- | ---: | ---: | ---: | --- | ---: |
| final-1 | 94/4/89 | 175 | 474329 | 8624009935 | 2.831 / 13.304 / 46.011 | 4357.0 |
| baseline-1 | 95/4/90 | 175 | 478493 | 8545539719 | 2.811 / 13.679 / 49.617 | 4190.7 |
| final-2 | 94/4/89 | 175 | 474955 | 8545332128 | 2.859 / 13.698 / 48.137 | 4361.9 |
| baseline-2 | 94/4/89 | 175 | 477459 | 8555704668 | 2.778 / 12.947 / 47.552 | 4132.2 |
| final-3 | 92/4/87 | 174 | 474929 | 8491190145 | 2.645 / 13.097 / 44.810 | 4364.1 |
| baseline-3 | 95/4/90 | 172 | 469505 | 8456945227 | 2.686 / 13.150 / 49.540 | 4168.4 |

除 ReportCrash、assistantd、securityd 外，六轮各镜像的启动次数完全一致。第二对的服务集合、镜像集合与各镜像次数完全一致，均为 94/4/89；但第 151 个请求最终版本为 48.137 秒，基线为 47.552 秒，单对并未提速。第一对基线多一次 securityd 正常重启（退出 status 0）；第三对基线也多一次 securityd，且最终版本没有 assistantd 的 SIGKILL/重启，因而也没有 ReportCrash 与 `ReportCrash.SimulateCrash` 请求。assistantd 在基线为 [2,2,2] 次、最终为 [2,2,1]；ReportCrash 为 [1,1,1] / [1,1,0]；securityd 为 [3,2,3] / [2,2,2]。fud SIGSEGV 在两版本仍存在。基线第三轮另少三个 itunescloudd APS 注册。保留全部轮次及这些时间窗口差异，不宣称 fork 数普遍相等，也不把较少的重启归因于确定的修复。

来源驱动的等待完成减少了无效 delivery/host-event 尝试，实际 poll 与 delivery 总开销显著降低；p95 都落在直方图的 10 us 最小桶，无法据此量化桶内变化。CPU execution 与 JIT translation 仍占主要时间，且本次略增；phase 之间可能嵌套，不能相加当作独立 wall time。第 151 个请求中位数下降只描述这六轮观测，不能证明所有启动节点普遍改善或服务成功 check-in 提前。fsck 和 SpringBoard 的中位数略慢，与第二对结果一并保留。

RSS 中位峰值增加 193.418 MiB，匿名部分增加 197.918 MiB，而 JIT shared committed 基本持平、cache peak 仅增约 8.271 MiB；这些计量不能直接相减作归因，但不支持将全部 RSS 增量解释为 JIT cache 增长。布局探针 `../runs/goal-vm-layout.cpp` 对阶段 5 与阶段 7 的同编译器头文件输出：Page 均为 48 B；std::map 对象为 48 B，单条节点分配 96 B；radix 根为 16392 B，单个 leaf 为 65808 B，首次插入的 heap 增量为 65840 B。该探针未计旧的独立 page_lookup 索引，不能直接当作完整地址空间占用对比。radix 为含少量 resident 页的 4 MiB 区间也分配完整 leaf，元数据修改按 leaf 分离，因而稀疏度和 fork 后修改分布会影响占用。GuestPageBacking 从内联 4352 B 变为 232 B 逻辑对象加独立 4096 B Storage，另有共享所有权/分配器布局成本。它们解释了可能的内存来源；当前没有按真实进程统计 leaf、shadow 与 allocator 分布，不能定量声称哪项造成了多少 RSS。COW 改善了 clone 延迟，本次没有实现整体内存下降。

运行依据保存在上层 `../runs/`：`goal-accept-driver.log`、`goal-accept-{final,baseline}-iphone51-{1,2,3}.{log,stdout,timings.json,summary.json}`、`goal-accept-medians.json`、`goal-accept-report.md`、`goal-vm-layout.log`。驱动/汇总脚本为 `goal-final-acceptance.py`、`goal-boot-profile.py`、`goal-summarize.py`、`goal-accept-report.py`。`goal-accept-manifest.json` 保留这六轮实际使用的可执行文件身份（后续重建会更新内嵌 build timestamp，因此哈希可变化）：

```text
final    81734290223dd9d1717695d57fbb14fdf9377badcf0b3f6e6e4d062b8f25febb
baseline 021d6598551906d250d55af1940562c476c57aa5d47ab49fd0782235375be00f
```

共享 vnode snapshot 的预先 page-in 限制、结束 ready 的请求集合语义和既有 guest 崩溃仍如前述。代码没有为这些限制增加 fallback 或配置开关；后续优化应分别针对 vnode 版本快照、稀疏 leaf/allocator 占用和剩余 CPU/JIT 成本测量。

## 构建、回归与复跑

所有命令在工作树中执行，产物也留在该树内。历史验证曾因内存压力限制为 `-j2`/`-flto=2`；本轮在内存充足时以 `-j10` 构建上层 `../build` 和独立 `../goal-stage5-build`，Release/IPO 均通过。构建、产品链接测试与固件测量顺序执行，测量期间没有编译、链接或第二个模拟器。

```sh
cd /home/xxiao/Projects/Project_iLEmu/.worktrees/pseudo-kernel-xnu-20261116/work_repository
cmake -S . -B build/validation/product-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DILEMU_ENABLE_IPO=ON \
  -DCMAKE_EXE_LINKER_FLAGS=-flto=2 \
  -DILEMU_WORKSPACE_DIR=/home/xxiao/Projects/Project_iLEmu
ninja -C build/validation/product-build -j2 ilemu
```

阶段 3 记录的 focused checks 覆盖 precise deadline、HID、HLE、exception、completion ownership、synchronization、host readiness、socket receive、multiplex、workqueue。阶段 4 增加信号 producer/target AST 分离、mask、sigsuspend/sigwait、ARM/Thumb restart 及成功 completion 优先级。阶段 5 覆盖 namespace generation/alias、MIG 路由、receive-wait 所有权、HID、signal、multiplex、exception，以及 `MACH_RCV_LARGE` 不消费消息、body copyout 失败时保留已安装 rights 并释放 message-held references。

已有阶段的 Release/IPO 构建通过；日志中仍有未修改位置的 graphics、aggregate 初始化和 fmt IPO 等原有警告。阶段 4/5 的记录表明改动行没有新增编译警告，不能据此称整个项目零警告。阶段 6/7 的最终编译诊断见对应结果。

长期回归可独立运行：

```sh
tests/mach/run.sh build/validation
python3 tests/mach/run-transport.py build/validation/product-build build/validation
python3 tests/mach/run-transport.py build/validation/product-build build/validation tests/kernel/file_table_kevent.cpp
python3 tests/mach/run-transport.py build/validation/product-build build/validation tests/kernel/multiplex_waits.cpp
python3 tests/mach/run-transport.py build/validation/product-build build/validation tests/kernel/workqueue_events.cpp
```

transport runner 使用产品的编译/链接设置并启用 assert，会链接库；不能与测量重叠。阶段 6 和 kevent 故障边界的测试说明见 `tests/kernel/README.md`，阶段 5 见 `tests/mach/README.md`，radix/COW/GuestCopyWindow 见 `tests/vm/README.md`。测试均作为长期回归提交；临时布局探针、链接 bundle 与固件驱动保留在未跟踪的 `../runs/`，不作为产品代码提交。

每轮使用一个未使用过的标签及新 rootfs 副本。以下示例标签应在实际复跑时改为新名字；不要覆盖或复用已运行目录。`--reflink=auto` 在文件系统支持时减少复制开销，guest 修改只落到副本。

```sh
cp -a --reflink=auto build/validation/iphone51-pristine/rootfs build/validation/iphone51-clean-recheck-1
python3 build/validation/boot-profile.py profile-recheck-iphone51-1 \
  build/validation/product-build/ilemu iPhone5,1 \
  build/validation/iphone51-clean-recheck-1 120 \
  > build/validation/profile-recheck-iphone51-1-driver.log 2>&1
python3 build/validation/summarize-boot.py profile-recheck-iphone51-1
```

profile 脚本保留 command、退出码、events 和 elapsed；当前版本另记录 peak RSS、最低 MemAvailable 和 memory-stop。可用内存低于 3 GiB 时通过 control-stdin 请求退出，避免继续压迫主机；因此中止的运行不能作为 120 秒验收样本。历史轮次没有这些内存字段，不能倒推出当时峰值。

启动参数为 activated、isolated network、headless、Vulkan、control-stdin、JIT profile off、catalog warming no-enqueue、perf summary/CPU phases。不用 `--ticks` 做功能或性能验收，该模式的 clock RPC 空转不能代表正常启动。scanout 只记录“首次日志记录”，没有真正第一帧计时。

早期阶段原始依据包括 `build/validation/stage3{a,b,c,d}-commit-message.txt`、`stage4-commit-message.txt`、`stage5-commit-message.txt`、`historical-profile-summary.jsonl`，以及 `build/runs/<label>.{log,stdout,timings.json}`。构建和测试日志位于 `build/validation/`。这些构建/运行产物通常被 git 忽略，文档表格保留关键数字，复查时应保留相应本地产物。
