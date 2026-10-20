# unitrace v4c-r2+ 构建与环境变量开关手册（build9 出货版）

> 2026-09-30。适用源码状态：ze_collector.h `a651610c`（build9，出货默认全开版）/
> ze_event_cache.h `20478073` / chromelogger.h（meta 版）。
> **2026-10-16 更新**：ze_collector.h 升至 `9382952f`（build11r3 TS_INTERP + tslog
> 写路径修复，出货候选，见 §1.5 与 §4 锚点表）。
> 前置战果：node3 trim24L ITL **38.43/38.43（+2.64% vs bare 37.44，规格 ≤38.57 PASS）**，
> 长收尾 vdelong 18 分钟 Mean 38.43-38.45 无漂移（bqm1）。
> 设计原则：**出货特性默认全开（不设任何 env 即为完整功能），每个开关 `=0` 可单独关闭**
> ——用于 A/B 测试与回归防抵押。

---

## 1. 如何编译

### 1.1 推荐路径：cri2 容器构建（与全部已验证产物同链路）

构建机 = <build-host> (<user>@<build-host>, sudo 密码见内部凭据台账（不入库）)，容器 `tf_unitrace_v4c2`
内做 fresh configure（cmake 4.x 绝不复用陈旧 build 目录）：

```bash
# ① 源文件 scp 到 cri2，再 docker cp 进容器源树
scp tools/unitrace/src/levelzero/ze_collector.h \
    <user>@<build-host>:/mnt/disk3/home/<user>/staging_ze_collector_buildN.h
ssh <user>@<build-host>
sudo docker cp /mnt/disk3/home/<user>/staging_ze_collector_buildN.h \
    tf_unitrace_v4c2:/mnt/disk3/home/<user>/unitrace_sdk_v4c2/unitrace_sdk/tools/unitrace/src/levelzero/ze_collector.h

# ② 容器内 fresh configure + ninja（必须 bash -lc 登录 shell，ninja 在 oneAPI env 里）
sudo docker exec tf_unitrace_v4c2 bash -lc \
 "SDK=/mnt/disk3/home/<user>/unitrace_sdk_v4c2/unitrace_sdk; B=\$SDK/tools/unitrace/build; \
  cd \$B && rm -rf CMakeCache.txt CMakeFiles && \
  cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > cmake_config.log 2>&1 && \
  ninja unitrace unitrace_tool > make.log 2>&1; echo rc=\$?; \
  md5sum \$B/unitrace \$B/libunitrace_tool.so"

# ③ 取出产物（docker cp 出来再 scp；docker cp 出的文件需 sudo chmod 644）
sudo docker cp tf_unitrace_v4c2:$B/libunitrace_tool.so /mnt/disk3/home/<user>/staging_lib_buildN.so
sudo chmod 644 /mnt/disk3/home/<user>/staging_lib_buildN.so
scp <user>@<build-host>:/mnt/disk3/home/<user>/staging_lib_buildN.so <目标机 staging>/buildN/
```

参考脚本：`poc_scripts/build/build_c2.sh`（含 strings 自检：`qkt_at_poll`/`GRAPH_BATCH_QKT` 计数 ≥1）。

### 1.2 快速语法门（本地，无 GPU 无容器）

改动 header 后先过本地语法检查再上构建机：

```bash
bash poc_scripts/build/syntax_check_local.sh   # -fsyntax-only，0 错误才继续
```

### 1.3 CRI 专用编译选项：**不存在**（2026-10-01 全树探索结论）

对 unitrace_sdk 全树（cmake、CMakePresets.json、.github CI、源码 #ifdef、构建脚本、
Dockerfile、文档）做了 word-boundary 排查（cri/netbatch/cluster/lgci/内核版本等）：
**无任何针对 CRI 集群 / netbatch 的 option、cache 变量、preset、条件分支或宏**。
所有 "cri" 命中均为 SCRIPT/CRITICAL 子串巧合；CI 的 `[self-hosted, pti, PVC/BMG]`
是 GPU 农场 runner 标签，非 CRI 配置。**CRI 集群只是构建运行的环境，代码层面零感知**
——本机重编译沿用现有 icx/icpx + Ninja 配置即可，无需（也没有）CRI 开关。

tools/unitrace 构建 key cmake 选项（默认值，探索实锤）：

| 选项 | 默认 | 说明 |
|---|---|---|
| CMAKE_BUILD_TYPE | 未设=Release | 我们恒用 RelWithDebInfo |
| BUILD_WITH_MPI | Linux ON | ITT=0+MPI=1 会 FATAL（联动约束） |
| BUILD_WITH_L0 / ITT / XPTI / OPENCL / OMP | 全 1 | 以 add_compile_definitions 注入 |
| BUILD_WITH_PERFETTO | ON | 失败自动降级 JSON-only |
| 编译器 | tools/unitrace 不自选，继承外部 CC/CXX | sdk preset 用 icpx_toolchain.cmake（icx/icpx）——**容器里手设 icx/icpx 即官方路径** |
| xptifw 链接 | find_library + LD_LIBRARY_PATH HINTS | 找不到时 unitrace_tool 仍构建（BUILD_WITH_XPTI 只控编译定义） |
| L0/OpenCL/ITT 头 | configure 期自动下载 | L0 头找不到 FATAL；**离线集群需缓存**（容器内已含） |
| 32 位检查 | void_p!=8 → FATAL | — |

tools/unitrace 自身无任何 sanitizer 选项（asan/tsan/fuzz 只在 sdk preset，不影响工具构建）。

### 1.4 部署目标机前必查（node3 教训）

```bash
ldd libunitrace_tool.so | grep "not found"   # 必须为空；build3 曾缺 libxptifw.so.1 全灭
```

若缺：从 cri2 容器 `/opt/gfx-deps/dpcpp/lib/libxptifw.so.1.0.1` 取出，放目标机目录并建
`.so.1` symlink；注意**同进程只允许一套 libxptifw**（双套 → phmap assert SIGABRT）。

### 1.5 诊断日志写路径修复（tslog fix，2026-10-16 收口）

`[TSCONV]`/`[TSLOG]`/`[BATCHQKT]` 诊断行原为多 fragment `std::cerr <<` 链（每行
~9 fragment，cerr unit-buffered → 每 fragment 一次 write() 系统调用），completer
线程与 app/poll 线程并发写时行内交错 → 粘行/丢行（实测 ~2%，且 ~2.5M 次 write()
系统调用落在命令完成热路径）。修复：类内 `TsDiagWrite`（栈上 snprintf 拼整行 →
静态 mutex → 单次 `fwrite(stderr)`）+ `TsDiagEnabled`（getenv 缓存，替代每命令
裸 getenv）；5 个站点改写，**行内容逐字段不变**、TSBAD 保持无门控、无 PTI_ASSERT、
无堆分配（+62/−36 行）。微基准：2 线程×50k 行损毁 89,942→0，0.745s→0.027s（28x）。
真机验证（node1 GPU0，同窗同内核）：collector 税 +9.0%→+4.0%，tslog 丢行
4,670→0（138,696==138,696 逐命令精确配对），emit slice 21.7→1.59ms/步，meta 专属
贡献归零。未选单写线程：主导成本是系统调用+交错而非锁竞争，且新增专线程引入
footer 前 flush 时序风险（详见 TSLOG_FIX 报告 §2）。

---

## 2. 环境变量开关总表（build9 默认值）

### 2.1 出货特性——**默认全开**，`=0` 关闭（A/B 用）

| 开关 | 默认 | 开启效果 | `=0` 关闭后形态 | 实测税贡献（node3 trim24L） |
|---|---|---|---|---|
| `UNITRACE_GRAPH_BATCH_QKT` | **ON** | 图时间戳批量读：290 事件挂 collector 专用 immediate list 每步一批读 | legacy 每节点 timestamp 事件（v4b 形态） | 净回收 **4.25ms/步**（关=大回退） |
| `UNITRACE_GRAPH_QKT_AT_POLL` | **ON** | 批量读武装在设备忙的 poll sweep（设备侧等待） | 读解析挪到 staging drain（v4b 形态） | 与 BQ 联动；关=退回 drain 解析 |
| `UNITRACE_DEFERRED_TS` | **ON** | eager 命令的时间戳 query+read+emit 挪 completer 线程（与设备执行重叠） | drain 内 inline 处理（+0.48ms/步） | **−0.48ms/步** |
| `UNITRACE_TRACE_META` | **ON** | `unitrace.*` meta 记录（enqueue/emit/qkt_batch/reset_batch/step/final_flush） | 无 meta 记录，其余字节等同（逃生门） | ≈0（纯 host 侧） |
| `UNITRACE_GRAPH_BATCH_QKT_RESET` | **ON** | 事件批量 reset（290×append + 1×sync） | 逐事件 fallback reset（`reset_fallback` 计数升高） | 批量版更省 |
| `UNITRACE_TS_INTERP` | **ON** | prepk 高频 GTS 摊销插值：thread_local 锚点（每线程每 device 单槽）+ 300µs 窗内 MONO_RAW 插值（PTI-336 忠实移植）；submit/replay 真读喂槽。**r3：速率首用自校准**（每线程每 device 一次性：双 GTS 对 ~1.5ms 间隔实测真实速率，32.32 定点）。timerResolution 字段仅作校准失败时的回退猜值——**字段单位由 props.stype 决定（unitrace 恒走 1_2 stype=Hz 形态），自校准对字段完全免疫** | 逐调用真 `zeDeviceGetGlobalTimestamps`（build9 形态） | collector CPU：prepk GTS **472→86µs/步**（6 worker 线程 × 每步 1 次重同步地板）、capture 尖刺步 **261→37ms**；ITL 中性（node3 −0.13%；node1 GPU0 正式臂 vt1 vs vt0 Mean −1.04% ±3% 内，82.5% GTS 事件走插值、pk_gts_us_med 降 4×）；保真度门全绿（负 duration=0、哨兵=0、err=0）；r3 额外收益：消除整数速率 +1672ppm 截断（node1 card2 实测校准速率 19.1986MHz 与 POC 逐位吻合） |
| `UNITRACE_GRAPH_QKT_SHADOW_W` | **ON** | **BQ-S2 影子完成事件 W**（设计 `BQ_S2_DESIGN.md`）：每次成功 graph replay 在同 list in-stream append 1×signal(W)，armed read wait 集 {W} 替代 {307×E}，共享事件按设计保持 latched、reset batch 整体消除（557µs/步→0）。pop 时 QueryStatus 门（R7：陈旧 re-signal 自愈，destroy+recreate 另取）；乱序 list 不注入（mode-b 预防）；六计数器 w_pops/w_latched/w_inject_fail/w_degrade/w_released/w_abandoned（健康形态=pops==released==submissions、其余全 0） | =0 回 v4c 形态（reset batch 回归） | node3 税 +2.64%→**+1.4%**（38.43→37.8x）；node1 税 +4.0%→**+2.9~3.8%**（37.9x→37.55~37.88）；capture 代残差 reset 批一次性 551-683µs（登记偏差） |

**开销标注原则**（时间轴真实性的设计约定）：

- **时间轴必须是真实的**：不从时间轴上把采集开销减掉——开销是真实发生的（node3 +1.4%、node1 +2.4%），减掉会造成异步 GPU 场景下的因果倒置（与已修复的错误代时间戳同风险类），且隐藏开销等于放弃自校验（reset batch 尖刺、cerr 爆量都是靠开销可见才发现的）。
- **开销要可见、可归因、可过滤**：时间轴保持真实，「净耗时」通过查询字段提供，不通过篡改时间戳实现。分层：
  - **A GPU kernel 时长**：设备时间戳，可信，不加不减。
  - **B host API 切片**：被采集拉伸的部分以 args 标注——`collector_us`（本切片窗口内完全包含的 `unitrace.*` meta 子窗口之和，直接父归属、不重复计）与 `api_net_us`（切片总长 − collector_us）。有债才写字段，干净切片字节不变（META=0 仍字节等同）。
  - **C 设备侧插入操作**：影子 W 刻意不 emit chrome op（对 kernel 时长统计正确）；设备侧标注轨道（UNITRACE_TRACE_META=2）是独立后续战役。
  - **D 端到端**：trace 末尾一条 instant `unitrace.overhead_total` meta 记录（`collector_total_us` = 生命周期 meta 债累计、`meta_records_n`），meta 记录自身的开销不虚报（instant 不入窗口）。bench ITL 差值仍是外部端到端标尺。
- **META=0 的定位是回归工具**，不是用户模式：META=0 下开销仍在（BQ 在跑）但无解释——那是最差状态；带 meta 的标准形态才是默认。
- **最终目标是降开销**（BQ-S2 路线），标注只是让税可观测、可归因、可过滤。

**build11 子开关**（默认值即出货形态）：

| 开关 | 默认 | 说明 |
|---|---|---|
| `UNITRACE_TS_INTERP_DELTA` | 300000 (ns) | 锚点重同步窗（PTI kSyncDeltaDefault）；0/非法值回落默认 |
| `UNITRACE_DEBUG_TSINTERP` | OFF | 每次重同步打一行（锚点年龄/delta/rate/校准形态） |

验收仪表：meta enqueue dump 的 `pk_gts_n`（真 GTS 次数，预期 ~线程数/步；
**首步 ~2×线程数** = 每线程一次性速率校准读）、`pk_interp_n`（插值次数）、
`pk_gts_err_n`（**必须 = 0**）。

### 2.2 可选实验项——默认关，`=1` 开

| 开关 | 默认 | 效果 | 备注 |
|---|---|---|---|
| `UNITRACE_ASYNC_EMIT` | OFF | chrome 记录构造挪独立 emitter 线程 | 实测 ITL 中性（emit 本就 100% 隐藏）；保险项 |
| `UNITRACE_TS_EXT` | OFF | 扩展 kernel 时间戳源 | 实验性 |

### 2.3 诊断/调试——默认关

| 开关 | 效果 |
|---|---|
| `UNITRACE_DEBUG_TS` | 时间戳换算逐条打印（TSCONV） |
| `UNITRACE_DEBUG_TS2` | inline 顺序诊断模式；**强制 legacy 逐事件查询路径**（BQ 被忽略并打一行 notice） |
| `UNITRACE_DEBUG_DEFER` | deferred-ts 诊断输出 |
| `UNITRACE_DEBUG_BATCHQKT` | BATCHQKT 生命周期日志（one-shot 门控） |
| `UNITRACE_DEBUG_EMIT` | async-emit 诊断输出 |

### 2.4 注入测试——默认关

| 开关 | 效果 |
|---|---|
| `UNITRACE_TEST_ALLOC_FAIL` | 强制每个 BATCHQKT 分配失败 → 验证优雅降级（预期 batches=0 + `qkt_skip_alloc_fail>0` + 无 fault） |
| `UNITRACE_TEST_SHADOW_W_FAIL` | 强制每次 graph 提交的 W 注入"失败" → 端到端验证无 W 降级（arm 跳过、步走 v4b drain 读、w_inject_fail/w_degrade 计数升高、无 fault） |

### 2.5 launcher 侧

| 开关 | 效果 |
|---|---|
| `UNITRACE_UseResultDirectory` | 结果目录组织（logger_factory） |

---

## 3. 使用示例

### 3.1 默认使用（零 env，全部出货特性已开）

```bash
# SERVER（标准形态：四 chrome flag 必带，--start-paused 控采集）
VLLM_XPU_USE_CUSTOM_MODEL=1 VLLM_XPU_ENABLE_XPU_GRAPH=1 ZE_AFFINITY_MASK=0 \
NEOReadDebugKeys=1 EnableImplicitConvertionToCounterBasedEvents=0 \
<path>/unitrace --chrome-itt-logging --chrome-sycl-logging --chrome-call-logging --chrome-kernel-logging \
--output-dir-path <out-dir> --start-paused \
vllm serve /models/<model>/ --trust-remote-code --tensor-parallel-size 1 \
--max-model-len 8192 --no-enable-prefix-caching --enable-chunked-prefill \
--max-num-seqs 128 --shutdown-timeout 120

# CLIENT（压测；注意：本体系不用 --profile，采集由 session 控制驱动）
no_proxy=127.0.0.1 vllm bench serve --base-url http://127.0.0.1:8000 \
--model /models/<model>/ --dataset-name random \
--random-input-len 3500 --random-output-len 100 --max-concurrency 1 \
--num-prompts 2 --request-rate inf --num-warmups 2 --ignore-eos \
--ready-check-timeout-sec 0
```

要点：四个 `--chrome-*-logging`（itt/sycl/call/kernel）是被验证配置必须全带；
`--profiler-config.profiler xpu` 与客户端 `--profile` 刻意不用；模型路径两端一致。

### 3.2 验证默认特性已生效（冒烟）

trace/summary 里应有（grep 判据）：
`BQ-SUMMARY` 行 `batches>0`、`reset_fallback=0`、`collected==seen`；chrome trace 有
`urEnqueueGraphExp` 与 `unitrace.enqueue` 记录、footer 存在。

### 3.3 A/B 回归测试（防退化）

一次只关一个开关，与默认臂对比 Mean ITL（±3% 规格线，锚=同窗裸跑）：

```bash
# 全开基线（默认）
<标准命令>                                   # 期望 38.43 量级

# 逐项关闭二分（每次只改一个）
UNITRACE_GRAPH_BATCH_QKT=0 <标准命令>        # 期望显著回退（~39.9 量级，legacy 每节点事件）
UNITRACE_GRAPH_QKT_AT_POLL=0 <标准命令>      # 期望小回退（读挪 drain）
UNITRACE_DEFERRED_TS=0 <标准命令>            # 期望 +0.48ms/步（~38.9 量级）
UNITRACE_TRACE_META=0 <标准命令>             # 期望 ITL 不变（meta≈0，仅记录消失）

# 全关参考（≈u0 基线口径， sanity 下界）
UNITRACE_GRAPH_BATCH_QKT=0 UNITRACE_GRAPH_QKT_AT_POLL=0 UNITRACE_DEFERRED_TS=0 <标准命令>
```

判读：某开关关闭后 ITL **不升**=该开关无税贡献（符合预期）；显著升高=该开关是承重优化
（如 BQ=0 → +4.25ms/步）。若默认臂相对裸跑超出 ±3% 规格，用上表逐项二分定位退化来源。

### 3.4 历史臂复现对照（如需对齐旧数据）

| 旧臂 | 复现 env（在默认全开基础上） |
|---|---|
| v4b（仅 BQ） | `UNITRACE_GRAPH_QKT_AT_POLL=0 UNITRACE_DEFERRED_TS=0 UNITRACE_TRACE_META=0` |
| v4c（build3 形态） | `UNITRACE_DEFERRED_TS=0` |
| vde（DEFERRED_TS 单开） | （默认即 vde 形态，另需 build8 前的 lib） |
| u0（基线） | `UNITRACE_GRAPH_BATCH_QKT=0 UNITRACE_GRAPH_QKT_AT_POLL=0 UNITRACE_DEFERRED_TS=0 UNITRACE_TRACE_META=0` |

---

## 4. 产物锚点（备查）

| 产物 | md5 | 位置 |
|---|---|---|
| **bqs2 源 ze_collector.h（build11r3 + tslog fix + BQ-S2 影子 W，出货候选）** | `fbee9b26360a7a00d0be09826c879e1a` | 本地树 + build11/tslogfix_src/ + git commit #5 |
| **bqs2 源 ze_event_cache.h（+ReleaseAll teardown backstop）** | `ab293b3c195f3cc52dfc3988d792c393` | 同上 |
| bqs2 lib（node1 编） | `5e3a243f…` | node1 bqs2_stage/bin/ + rg1s2/s2 各臂门实测 |
| tsfix 源 ze_collector.h（build11r3 + tslog 写修复，被 bqs2 取代） | `9382952fce5567242aceed9243f0147b` | 本地树 + build11/tslogfix_src/ + git commit #4 |
| **tsfix lib** | `782fd14ee9b6c46b9f5a981df067879c` | node1 tsfix_stage/bin/ + build11/m1fix_raw/ 记录 |
| build11r3 源 ze_collector.h（TS_INTERP 定标版，被 tsfix 取代） | `0a6d1c2822dc6993a3d61cfa80a0bdf9` | 本地树 + build11/（备份 `ze_collector.h.pre_tsdiag`） |
| build11r3 lib（icx 工具链） | `7a3a6b139f9a747203d4decff7019560` | 本地 build11/ + node1 容器 staging /root/staging_build11/ |
| build11r3 launcher（与 r2 同一只） | `0ca390f7d4c71963508c653285314548` | 同上 |
| build11r2 源 ze_collector.h（TS_INTERP 版，被 r3 取代：timerResolution 单位陷阱） | `42472d5fdf6424ae8f437d6b66931f69` | 本地树 + build11/ |
| build11r2 lib（icx 工具链，node3 PASS——仅 Hz 形态平台有效） | `1010f9679768b1cdcd58036ff7fb0651` | 本地 build11/；node3 staging 现驻 r3（r2 备份为 *.r2_bak） |
| build11r2 launcher（与 r2 lib 配套，版本门绑定） | `0ca390f7d4c71963508c653285314548` | 同上 |
| build11 首建（GNU 工具链，**弃用**，node3 崩） | lib `e089b608…` / launcher `292c5625…` | 已改名 `.GNU_REJECTED` |
| build9 源 ze_collector.h（默认全开版） | `a651610ca1849a7eeccf3def1cae3508` | git + cri2 staging |
| build9 lib（出货默认全开版） | `903bad204865f92dbde791bf849e6109` | node3 `bq_staging/build9/` + repro_node3 ULIB |
| build8 lib（上一版，需 env 开 BQ/AT_POLL/DT） | `a67920db`（lib）/ `0469e260`（源） | node3 `bq_staging/build8/` |
| build3 lib（V2/V3 加固版） | `9f4b8db4` | node3 `bq_staging/build3/` |
| launcher（unitrace bin） | `4f33542a` | 随构建 |
| 验收判据报告 | — | `BQ_TAX_HOTSPOT_AND_OPT.md` / `BQ_TAX_LEDGER_PERFETTO_VISIBILITY.md` |
