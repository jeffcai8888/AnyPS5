# Townfall 兼容性攻坚 — Fork 开发文档

> fork: [jeffcai8888/AnyPS5](https://github.com/jeffcai8888/AnyPS5) · upstream: [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5)
> 目标游戏: Silent Hill Townfall (PPSA33286, UE5) · 测试环境: RTX 4060 Ti 8GB / Win10 / WinLibs GCC 15.2.0
> 状态(2026-10-14,分支 `sync/upstream-20261013`): 已变基到 upstream `5d50886ee`;越过 compaction
> 着色器崩溃点后可连续运行 14 分钟以上。崩溃点之前 ~7–8 FPS,之后掉到 <1 FPS,原因待查(详见 §9)

---

## 1. 概述

本 fork 以 Townfall 为探针,对 AnyPS5 模拟器层做了一系列通用性修复。这些缺口虽然由 UE5
大作首先触发,但本质上是模拟器的通用缺口(RDNA 指令集覆盖、PS5 SDK ABI 容忍、Vulkan
显存管理、呈现管线),对其他 PS5 游戏同样有价值。

**成果时间线**(游戏日志深度,越深越远):

```
最初:      PSO 预编译即崩溃(~100 行)
+swappc融合: konami 影片阶段
+呈现链四门: 帧可以上屏(此前全为黑帧)
+显存策略:  3252 行 / 可见片头画面、连续运行 12 分钟以上
+上游同步:  ~7–8 FPS(旧运行时 2.5–2.9 FPS),~3.5 分钟后在 compaction 着色器崩溃
+ordered append / 内存钩子调用 / ds_ordered_count: 越过崩溃点,运行 14 分钟以上
```

fork main 相对 upstream 共 8 个提交;同步分支变基后的提交见 §10。

## 2. 环境与目录

| 路径 | 用途 |
|---|---|
| `E:\Work\AnyPS5` | 主工作树(fork main,开发在此) |
| `E:\Work\AnyPS5\build` | 主构建目录(ninja;`BUILD_TESTING=ON` 才有单元测试靶子) |
| `E:\Work\AnyPS5-play` | 体验工作树(git worktree,挂在过去提交上,打体验补丁) |
| `E:\Work\AnyPS5-play-build` | 体验构建目录 |
| `E:\Townfall-AnyPS5` | 旧游戏运行时(体验树 prx,~2.5 FPS;保留作对照,不再更新) |
| `E:\Townfall-AnyPS5-sync` | 同步分支运行时(新 relink 的 app.exe + 主构建 prx;`start-game.bat`) |
| `E:\Townfall-AnyPS5\shader_cache` | 着色器磁盘缓存(换构建会全量作废,属正常) |
| `E:\Townfall-AnyPS5\启动游戏.bat` | 带环境变量的启动器 |
| `E:\PPSA33286 Silent Hill Townfall\...\decrypted` | 解密游戏转储(eboot/prx) |
| `E:\Tools\winlibs\mingw64` | GCC 15.2.0 工具链(需在 PATH) |

工具链前置:configure 时需 `SSL_CERT_FILE="C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt"`(FFmpeg 下载)。

## 3. 构建与部署流程

```bash
# 1) 同步上游(每次开发会话开始)
git fetch upstream && git rebase upstream/main   # 冲突:保留上游改名+我方增量

# 2) 构建(按需选择目标)
cd build && export PATH="/e/Tools/winlibs/mingw64/bin:$PATH"
ninja core/libs/libs/libSceAgcDriver.prx   # GPU 驱动模拟(最常改)
ninja core/libs/libs/libSceAgc.prx         # Agc CPU 侧接口
ninja libkernel.prx libSceVideoOut.prx     # 内核/呈现
ninja external_call_fuser_tests            # 单元测试

# 3) 部署(必须先验证再拷贝,见 §11)
grep -ac "<新代码中的唯一字符串>" core/libs/libs/libSceAgcDriver.prx
cp core/libs/libs/*.prx E:/Townfall-AnyPS5/libs/

# 4) 测试
ctest -R "external_call_fuser|scalar_swappc$|scalar_saveexec|scalar_bitcount64|agc_driver_scalar_swappc$"

# 5) 游戏验证(带 dump)
cd /e/Townfall-AnyPS5 && APS5_DUMP_SHADERS=1 ./启动游戏.bat

# 6) 推送
git push origin main
```

新建运行时目录(上游 relinker 变化后旧 app.exe 不可复用):

```bash
relinker.exe --windows "<decrypted>/eboot.bin" E:/Townfall-AnyPS5-sync/app.exe   # 生成 app0/sce_module、app0/prx
# 再放入 libs/*.prx(build/core/libs/libs)、3 个 mingw DLL,
# app0 下 junction engine / sce_sys / townfall,以及 uecommandline.txt
```

测试注意:单元测试要在**不带** `APS5_PROFILE_*` 的环境里跑(GPU 计时需要 mock 设备没有的
`vkCreateQueryPool`);测试进程会锁住 `libSceAgcDriver.prx`,重链前先结束 `agc_*`/`ctest`。

离线回放工具(不需要游戏):

```bash
cd /e/Townfall-AnyPS5 && PATH="$PWD/libs:$PATH" \
  /e/Work/AnyPS5/build/core/libs/prx/libSceAgcDriver/agc_shader_replay.exe \
  --fuse --asm|--code|--mem <shader_*.req>
```

## 4. 修复目录(按子系统)

### 4.1 着色器重编译器(core/shader/recompiler)

| 修复 | 原理 | 位置 |
|---|---|---|
| **外部调用融合**(同步分支已由上游 `FindUserDataCalls`/`LinkUserDataCalls` 取代,fork 融合器仅剩测试与 replay `--fuse`) | Townfall 有 104 个 compute 着色器通过 `s_swappc_b64` 调用"地址在 user data 里"的外部代码(UE5 的 GPU 运行时链接)。检测器识别两种拷贝形状(`s_mov_b32`×2 / `s_mov_b64`),融合器把调用点重写为静态 `s_branch` 进被调代码私有副本,被调的 `s_setpc_b64` 返回改写回跳。全程只合成 nop/branch/endpgm 三种指令字,GraphBuilder 零改动 | 新文件 `ControlFlow/{include,src}/ExternalCallFuser.*`;注册期延迟 + `InvocationFor` dispatch 期融合(ShaderRegistry.cpp) |
| **SDWA 字节选择器** | `v_cvt_f16_u16/i16` 的字节源选择器不在白名单;翻译层的位域提取本就通用(零扩展)。**刻意不**给 f16 源的操作码开字节选择(浮点位无意义) | `RdnaDecoder/src/RdnaVectorOpDecoder.cpp` vop1SdwaRules |
| **SRT 空指针读 = 0** | 游戏用"可选指针"模式:SRT 前 8 字节为 null 时着色器自带 fallback 分支;GCN 硬件对空地址标量读返回 0。同步分支:空根改用上游 `_nullRoots`;fork 的"任意读失败=0"已撤销(会破坏"缺快照不得读活内存"测试),只保留**固定地址根**(地址是代码里的立即数)读到未映射内存时=0,视作空钩子表 | `Optimization/src/SrtWalker/SrtEvaluator.cpp` EvaluateRawRead |
| **ds_ordered_count** | GDS 有序计数(offset0[7:2]=计数器下标,offset1:wave_release/done、bits 5:4 操作 0 add/1 swap、GFX10 bits 7:6 dword 数-1)。实现为首个活跃 lane 对 GDS 做一次原子 add/swap,旧值广播到所有 lane。**不保证 wave 发射顺序**:compaction 总数和区间互不重叠都正确,但各 wave 拿到的区间顺序可能不同;wrap/sub/多 dword 仍拒绝 | `Translation/src/MemoryInstructions/SharedMemoryInstructions.cpp` dsOrderedCount |
| **顶点阶段 s_barrier** | 不访问 LDS/GDS 的顶点程序直接丢掉 barrier;有共享内存通信时发 `OpControlBarrier`,不再报错 | `SpirvBackend/src/SpirvFlowEmitter.cpp` EmitBarrier |
| **堆容量** | 存储堆按"每绑定组 1 个动态 mip 图像"设计,游戏用 2 个(2×4 mip 槽=8>4)→ `StorageHeapRoots=16`;采样堆 16→32(材质着色器单绑定 18 槽) | `RuntimeAbi.hpp`(容量常量是全局契约,一处改动验证/分配全生效) |
| **未映射读优雅降级** | SRT 走查追到空描述符时,ShaderMemory 回调原来直接抛异常杀死分发;改为返回 false,走查按设计降级到动态路径 | 驱动 `Execution/src/ShaderMemory.cpp` |

### 4.2 驱动 ABI 容忍(core/libs/prx/libSceAgcDriver)

| 修复 | 原理 | 位置 |
|---|---|---|
| **类型系统容忍** | type 8 (Fs)=独立顶点 fetch 着色器,绘制解码从不绑定它——日志+跳过即正确行为;type 4/5(GsFront/HsFront)无程序地址寄存器对,注册期延迟到图形 ABI | `ShaderRegistry.cpp` PrepareRegistered switch |
| **临时头拷贝** | UE5 PSO 预编译用**栈上补丁拷贝**的 Shader 头调插值映射 API;三条解析路径(矩形/图形/静态 ABI)按 **code 身份**接受头拷贝 | ShaderRegistry.cpp 三处 lookup |
| **呈现链四道门** | ① 3D 体积颜色目标接受基础 mip 切片视图(State.cpp);② 已注册着色器的未知入口点 dispatch 期按需编译(InvocationFor);③ 矩形路径对延迟着色器现场补编(注入入口地址+图元类型+清几何路由位 0x20/0x4);④ 矩形顶点/像素配对按需构建(PreparedRectangle 改收产物+target) | State.cpp / ShaderRegistry.cpp / Draw.cpp |
| **ORDERED_APPEND 分发位** | DISPATCH_INITIATOR bit 0x8 只影响 ds_ordered_count 的顺序;直接分发掩码 0xa066→0xa06e,间接 0xa064→0xa06c | `Execution/src/Pm4.cpp` Validate |
| **内存加载的调用目标** | compute 着色器从固定地址 `0xF_E0040000` 读描述符,`s_buffer_load` 读出函数指针,判空后 `s_swappc_b64`(UE 的 GPU 钩子)。dispatch 期在调用前的直线代码段(段内不能有分支目标落点)里求值 `s_mov`/`s_load`/`s_buffer_load` 链:目标为 0 时把 swappc 换成 `s_nop`(有判空守卫保护,硬件不会跳进 0 地址);非 0 时按上游 user-data callee 方式链接;无法求值时打日志并保留原错误。该表在模拟器里未映射,按"零=未安装钩子"处理并每个着色器记一次日志 | `ShaderRegistry.cpp` MemoryCallTarget / LinkUserDataCallees |
| **注册期准备异常上下文** | snapshot 重载的 Plan+Prepare 包在 try 里,异常追加"preparing registered shader 0x.. type .." | ShaderRegistry.cpp |
| **DMA_DATA 变体** | dst_sel=2 + 0 字节(围栏)与 src==dst(缓存区段刷新)均为 no-op;验证器/执行器/StoreWrite 三处一致 | `Execution/src/Pm4.cpp` |

### 4.3 显存管理(Graphics)

| 修复 | 原理 | 位置 |
|---|---|---|
| **缓冲视图钳制** | 游戏 V# 按分配桶超额声明(256KB 桶 vs 192KB 真实分配);按 **direct 导入终点**(不是 region.end!)钳制,OOB 读硬件返回零 | `Graphics/src/GuestBufferMemory.cpp` Descriptor |
| **detiler 宿主路径** | 4K 多层阵列的转换缓冲达 3.98GB(124MB×32 层);≥64MiB 的 detile 全程宿主内存(staging 兼作计算源,HOST→COMPUTE barrier 替代拷贝),显存占用归零 | `Graphics/src/Texture.cpp` 上传块 |
| **DeviceBuffer 宿主回退** | 同步分支:阈值改为 ≥1GiB(4MiB 会让 buffer_pool 测试失败);回退缓冲带 `hostFallback` 标记,释放时不回池;池 usage 统一为 `BufferPool::Usage(usage, DEVICE_LOCAL)`,Take 用 `this->usage`(否则 bda_device 测试的设备本地缓冲不被保留) | `Graphics/src/Resources.cpp`、`Resources.hpp` |
| **字节预算淘汰** | ResourceCache 条目钉住的纹理越过纹理缓存自身预算;条目上报 `PinnedBytes`(共享存储纹理按持有数分摊),LRU 淘汰至 `APS5_RESOURCE_CACHE_MIB`(默认 4096)以下 | `ShaderResources.{hpp,cpp}` |
| **共享纹理路由**(体验树) | ≥16MiB 采样图像优先落共享 GPU 内存(类型 2/3,32GB),显存留给渲染目标。注意:**存储图像(渲染目标)NVIDIA 驱动拒绝放共享内存**——已验证不可行 | 体验树 `Texture.cpp` SharedTextureBytes |

### 4.4 内核与呈现(libkernel / libSceVideoOut)

| 修复 | 原理 | 位置 |
|---|---|---|
| **SyncOnAddress 分桶** | 64 个按地址哈希的分片锁替代全局互斥(UE5 worker 并发等待不再串行化);移植自 SoS-PS5 | `libkernel/SyncOnAddress/src/SyncOnAddress.cpp` |
| **帧率上限** | `APS5_FPS_CAP=<30..1000>` 呈现循环节流;移植自 SoS-PS5 | `libSceVideoOut/src/VideoOutDriver.cpp` |

## 5. 显存管理架构(8GB 卡)

显存消费者与对策的总览("挤气球"模型——每个消费者都要有预算且总和 < 8GB):

```
消费者                        对策                      状态
─────────────────────────────────────────────────────────────
着色器磁盘缓存                磁盘,不占显存            ✅
缓冲暂存(detiler/回写)       宿主路径 + ≥4MiB 回退     ✅ 已收口
采样纹理缓存                  上游 VK_EXT_memory_budget ✅ 上游已做
存储图像缓存                  上游自适应预算            ✅ 上游已做
ResourceCache 钉住            本 fork 字节预算          ✅ 本 fork
采样纹理(≥16MiB)            共享内存路由              ⚠️ 体验树
4K 目标拷贝缓冲(124MB×2/绑定) 未预算                   ❌ 下一步
在途批次保留                  未预算                   ❌ 下一步
纹理图像工作集                需 LRU 字节预算淘汰       ❌ 下一步(8GB 最后一墙)
```

## 6. 环境变量参考

### 用户可调(启动器)

| 变量 | 默认 | 说明 |
|---|---|---|
| `APS5_RESOURCE_CACHE_ENTRIES` | 1024 | 资源缓存条目数;8GB 卡跑 Townfall 用 **64**(稳定但慢) |
| `APS5_RESOURCE_CACHE_MIB` | 4096 | 字节预算淘汰阈值(0 关闭) |
| `APS5_STAGING_POOL_MIB` | 512 | 设备暂存池;8GB 卡建议 256 |
| `APS5_FIRST_SLICE_ONLY` | 关 | 多切片颜色视图只渲首切片(侦察模式) |
| `APS5_FPS_CAP` | 关 | 帧率上限(60/120/…) |

### 诊断开关

| 变量 | 作用 |
|---|---|
| `APS5_DUMP_SHADERS` | dispatch/失败请求 dump(shader_<addr>.req / shader_fail_<addr>_<n>.req) |
| `APS5_DUMP_IR=<addr\|all>` | 翻译管线各 pass 后的 IR |
| `APS5_TRACE_AGC` | Agc CPU 侧 API 线程级调用跟踪 |
| `APS5_TRACE_SHADER_REGS=all` | 每次着色器注册的 sh/cx 寄存器 dump |
| `APS5_TRACE_ALLOC` | ≥16MiB 分配打印(热路径,默认关) |
| `APS5_TRACE_FUSER` | 融合检测器逐调用点跟踪 |
| `APS5_SKIP_FS` | 丢弃 type-8 注册(判别实验用) |
| `APS5_PROFILE_DRAW` / `APS5_PROFILE_GPU` | `[present]`/`[gputime]` 每 10 秒统计(present 数、GPU 忙/空闲);**跑单元测试时必须关** |

## 7. 调试工具链

- **agc_shader_replay**:离线回放 .req(含 `--fuse` 合成被调方验证编译链;`--asm/--code/--mem` 检查,`--prepare --spv` 验证能否编出 SPIR-V);需 `build/core/libs/libs/unpatched` 与 mingw 在 PATH。`ninja agc_shader_replay` 构建。.req 是 base64 二进制:离线验证补丁可先解码、替换指令字(如 swappc→`0xbf800000` s_nop)再编码回放
- **失败请求 dump**:recompileReporting 落盘带序号的完整请求(注册态 dump 为 `_pre` 后缀)
- **eboot 静态分析**:Python + capstone;exe RVA→eboot 偏移 `-0xc008`;NID 反查用 `tools/nid_names.compute_nid` 扫源码 `APS5_VABI` 定义(可解析 ~842/1997 个 PLT 槽)
- **呈现计数器**(体验树):`[videoout] presented N frames` 直接回答"有没有帧上屏"
- **UB 甄别**:同输入不同错误 = 悬垂内存(见 §11 replay 悬垂案例)

## 8. 测试

- 无设备单元测试 5 项:`external_call_fuser`(检测/解码/融合字节级断言+全管线)、`scalar_swappc` 系列、`scalar_saveexec`、`scalar_bitcount64`
- 主构建目录需 `cmake -DBUILD_TESTING=ON` 一次
- 批量验证:`agc_shader_replay --fuse shader_fail_*.req`(104 个 dump 全融合,11 个全编译,其余为注册态 dump 的预期伪影)
- 每次改动的回归底线:5 项测试全绿 + 游戏日志深度不回退
- 同步分支新增/改动的相关测试:`agc_driver_pm4_tests`(0x49 直接/间接分发可通过,0x59 仍拒绝)、`agc_wave_control_decode_tests`(ds_ordered_count add 能编译,wrap 被拒)、`agc_shader_memory_tests`(快照严格性)、`VertexBarrier`(要求出现 OpControlBarrier)
- 完整 ctest 未跑完(中途中断),有 11 个超时项尚未排查

## 9. 已知问题与当前前沿

0. **崩溃点之后掉帧(同步分支,当前首要)**:每 10 秒 present 数为 `75 74 78 80 62` → `1 1 1 2 2 3 …`,正好从 compaction 着色器首次运行开始(日志 `game-log-sync12.txt` 约 9928 行)。之后 GPU 大部分时间空闲(每次 present 最多约 60 s 空闲),说明是 CPU 侧在等待。三个可能原因:着色器缓存版本变化后在全量重编;游戏进入了以前从没到达过的新场景;无序的 ds_ordered_count 结果不对(比如游戏重置 GDS 计数器的方式没有被模拟,计数一直累加)。先看画面卡在哪,再用 `DMA_DATA` 到 GDS 的写入核对计数器有没有被重置。
1. **亚帧率(<1 FPS,旧运行时)**:64 条目缓存下 UE5 资源集轮转导致 StorageTexture 每秒 ~7 次全量重建(建图+分配+上传)。根修 = 渲染目标图像的 LRU 字节预算淘汰(图像支持重上传)。
2. **间歇性 UE5 worker 空指针写**(app.exe+0xa52e76):时序敏感,已证明是其它失败的下游受害者(APS5_SKIP_FS 判别实验);频率随修复递减。
3. **上游着色器磁盘缓存往返 bug**:PackedAncillary 折叠冷缓存成功、热缓存失败(序列化丢折叠);规避 = 删 `shader_cache` 重编。值得报上游。
4. **未预算显存消费者**(§5 表):4K 目标拷贝缓冲是在建的最大一块。
5. **体验树与 main 的分叉**:共享纹理路由、存储缓存旋钮、呈现计数器在体验树未移植;移植时**严禁整文件拷贝**(§11)。

## 10. 提交索引

### 同步分支 `sync/upstream-20261013`(变基到 upstream `5d50886ee`,自新到旧)

| 提交 | 内容 |
|---|---|
| (未提交) | 顶点 barrier、ShaderRegistry 异常上下文、DeviceBuffer usage/1GiB 回退、SrtEvaluator 回到上游+固定根零读、ORDERED_APPEND、内存加载调用目标、ds_ordered_count、缓存键审核名单 |
| `805d93af` | 稳定的资源缓存计数 + 有界的设备压力淘汰 |
| `14f636b8` | 设备压力感知的纹理缓存淘汰 |
| `d18190f7` | 本文档 |
| `8ce01bcb` | SyncOnAddress 分桶锁 + 帧率上限 |
| `755b7ff9` | ResourceCache 字节预算淘汰 |
| `fa5888b0` | 巨型 detile 路径保留上游纹理统计 |
| `74e9e8d1` | 超大 GPU 缓冲/detile 的宿主内存回退 |
| `2acd0d50` | 呈现链容忍 + VRAM 诊断 |
| `4f2e077f` | 堆/解码/包/资源兼容修复 |
| `6e047cd0` | dispatch 期融合与 ABI 变体容忍 |
| `e849741a` | user-data 外部调用融合 |

`main` 未动;同步分支验证完后再决定是否合并。

### fork main(自新到旧)

| 提交 | 内容 |
|---|---|
| `cb56c6bc` | SyncOnAddress 分桶锁 + APS5_FPS_CAP(移植自 SoS-PS5) |
| `451ed896` | ResourceCache 字节预算淘汰(PinnedBytes + APS5_RESOURCE_CACHE_MIB) |
| `c63b8c72` | 修正 8afcfd98 误删的上游纹理内存统计 |
| `8afcfd98` | detiler 宿主路径 + DeviceBuffer 宿主回退 |
| `09336492` | 呈现链四道门 + VRAM 诊断 |
| `e8ead46c` | 堆容量/SDWA/DMA/SRT 零填充/缓冲钳制等 |
| `7dbeeff2` | dispatch 期融合集成 + ABI 容忍 + 跟踪仪表 |
| `6c9ec9fc` | 外部调用融合器(核心库+测试+replay --fuse) |

参考项目:[SoS-PS5](https://github.com/OverkillLabs3/SoS-PS5)(战神:斯巴达之子,60FPS)。其 umtx 分桶与帧率上限已移植;寄存器摊平上游已独立实现;AMD 顶点修复、干净退出、unwind 缓存未移植(N 卡不触发,留作兼容储备)。

## 11. 经验教训(踩过的坑)

1. **陈旧 prx 部署**:ninja 对象比源码旧时游戏白跑 5 分钟——部署前 `grep -ac "<新唯一串>"` 验证构建产物;
2. **整文件拷贝移植**:从体验树拷整个文件会回退上游独立演进(两次踩坑:纹理内存统计、CTest 逮住)——正确做法是 git checkout 上游版 + 逐块应用我方编辑;
3. **悬垂 span**:replay 的融合向量声明在内层作用域,块结束销毁 → 后续解码读垃圾,表现为"同输入不同解码错误"——随机错误先查生命周期;
4. **s_branch 立即数基准**:`branchTarget = pc + 4 + imm*4`,以**下一条指令**为基准;
5. **heredoc 写 C 转义**:`\n` 会被 shell 展开成真实换行——C 代码字符串一律用 Edit 工具写;
6. **NVIDIA 驱动边界**:最优平铺图像(渲染目标)拒绝宿主内存类型——图像回退只对采样图像可行的结论已实测;
7. **"上传成功"≠"上屏"**:StorageTexture upload 计数与 presented 计数是两回事,验证呈现必须看 present 计数器;
8. **判别实验先行**:APS5_SKIP_FS 一轮实验把"嫌疑共性"证伪为"下游受害者",避免了对着错误症状继续修。
9. **着色器缓存键审核**:驱动里新用到 `ShaderRecompiler::X`(调用或 using 声明)时,`ShaderCacheVersion.cmake` 会让构建失败;确认它不会绕过 RecompileRequest 影响编译结果后,把名字加进 `core/shader/recompiler/CMakeLists.txt` 的 `shaderCacheReviewedNames`。改了重编译器源码会让磁盘缓存版本变化,下次进游戏要全量重编(会变慢)。
10. **诊断要记下全部失败**:调用目标求值器起初只记最后一次失败("base it cannot follow"),把真正的根因(第一次读的是未映射地址)盖住了,多跑了两轮游戏。求值链诊断要把每一步失败都拼接起来。
11. **边界检查别套用错**:`vcc` 作为 SMEM 基址是 s106:107,4 dword 描述符才需要的 `base+3 < 108` 检查会把它误拒。
12. **毒化守卫≠零值**:上游 SRT 毒化槽在运行时被读到就记 fault 并停掉该 invocation;对每个线程都会无条件读的钩子表,这意味着整个分发作废,所以固定根走"零值"而不走毒化。
13. **测试进程锁 prx / PROFILE 环境变量**:见 §3 末尾。

## 12. 路线图

0. 查清同步分支崩溃点之后的掉帧(§9 第 0 条);提交 6+ 个未提交修复;跑完完整 ctest,排查 11 个超时项;
1. **渲染目标 LRU 字节预算**(8GB 最后一墙)→ 可玩帧率;
2. 4K 目标拷贝缓冲"用完即还";
3. 移植体验树残余(共享纹理路由等)并合并双树;
4. 上游报磁盘缓存往返 bug;
5. 移植 SoS-PS5 干净退出 + unwind 缓存(通用质量);
6. 16GB 卡验证(预期可直接玩)。
