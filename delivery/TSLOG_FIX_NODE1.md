# TSLOG 写路径修复报告（node1，GPU0，2026-10-16）

战役边界：协调者批准的唯一一次修复迭代。commit #4 冻结，全部结果回报后再定推送。

## 0. 结论速览

- 根因实锤：`[TSCONV]`/`[TSLOG]`/`[BATCHQKT]` 诊断行是多 fragment `std::cerr <<` 链
  （每行 ~9 个 fragment），libstdc++ cerr 为 unit-buffered → **每个 fragment 一次
  write() 系统调用**；两条线程（deferred completer 线程 + app 侧 poll sweep）并发写
  → 行内交错 → 粘行/丢行（m1a 实测 1,943 行损毁 ≈ 2%）。
- 一次 canonical bench（~40s）诊断行总量 **272,723 行**（TSCONV 138,495 + TSLOG
  133,825 + BATCHQKT 403）→ **~2.5M 次 write() 系统调用**，全部落在命令完成热路径。
- 修法（按证据选）：**行级锁 + 单次 fwrite**（先在栈上拼完整行，再持锁一次
  fwrite(stderr)）。微基准：2 线程 × 50k 行，legacy 多 fragment cerr **89,942/100,000
  行损毁、0.745s**；修后 **0 损毁、0.027s（28x）**。
- 修后预期：丢行率 → 0；每行 ~9 次系统调用 → 1 次；getenv-per-command → 缓存一次。
  qkt_batch/emit 两项签名有 kernel/驱动依赖的诚实警示（见 §4）。

## 1. 机理（先根因后动刀）

### 1.1 谁在写

| 写者 | 线程 | 行 | 触发频率 |
|---|---|---|---|
| `GetHostTime` → `[TSCONV]` (ze_collector.h ~3381) | completer 线程（eager 命令）+ app 线程（graph clone，BQ on 时 poll sweep 内联消费） | 138,495/窗 | 每命令一次 |
| `[TSLOG] on_event` (~6273) | 同上两条路径 | 133,825/窗 | 每命令一次 |
| `[BATCHQKT] n=... batch_us/poll_read_us` (5309/5589) | app 线程（drain/poll 读） | ~396/窗 | 每步一次 |
| `[BATCHQKT] reset-batch` (5951) | app 线程 | 1/窗 | 一次性 |
| `[TSBAD]` (~6291) | 任一 | 0/窗（本次） | 仅坏读 |

门控：`std::getenv("UNITRACE_DEBUG_TS")` **每次调用都执行**（2 处热站点），
canonical 启动脚本 `launch_vt_node1.sh` 固定 `UNITRACE_DEBUG_TS=1` +
`UNITRACE_DEBUG_BATCHQKT=1` → 全部开火。
`BatchQktDiagEnabled()`/`Tslog2Mode()` 已是缓存式（static），仅 TS 门是裸 getenv。

### 1.2 交错窗口（行损毁的具体机制）

- 每条诊断行是 ~9 个 `<<` fragment：libstdc++ `std::cerr` 构造即 unitbuf（每次输出
  后 flush），所以**每个 fragment 单独一次 write(2) 系统调用**。
- 两个 fragment 之间线程可被抢占 → 另一线程的完整行插进来 → 粘行。实测样本：
  `...shift_ns=5208[TSLOG] on_event inst=758 ...`（TSCONV 尾 + TSLOG 头来自不同
  inst 序列）。粘行导致解析器整行丢弃 → 丢行率 ~2%（1,943/272k）。
- 微基准复现（2 线程 × 50k 行 legacy 9-fragment cerr，写同一文件）：
  good=10,058 / glued-or-mangled=89,942，0.745s（~7.5µs/行）。
  真机 bench 交错率仅 ~2% 而非 90%：两线程各 ~150µs 才写一行，而窗口 ~7µs，
  碰撞概率与实测吻合。
- 附带：getenv 每命令一次；cerr ostream sentry + locale 检查也是每 fragment 开销。

### 1.3 现有锁/缓冲结构

无。调用点直接 `std::cerr <<`，无锁、无缓冲、无单写者。既有单写者模型只有
ChromeTraceEmitter 的 `UNITRACE_ASYNC_EMIT=1` ring（chromelogger.h ~930-1100），
canonical 运行不开，且与 protobuf 路径耦合。

## 2. 修法选择（按证据定）

**选：行级锁 + 单次 fwrite**（`TsDiagWrite`），未选单写线程。理由：

1. 主导成本是系统调用数 + 交错，不是锁竞争。无竞争 mutex ~20ns vs 每行 9 次
   write(~2-5µs each) —— 锁版本同路径实测 28x（0.745s→0.027s/100k 行）。
2. 单写线程引入生命周期风险：tslog 必须在 bench 结束、runner 收 footer 前完整落盘；
   我们已踩过"停顿后 flush 竞态假 FAIL"。新增排专线程 + Shutdown 时序是本战役
   （唯一一次迭代、最小 diff 边界）内不该背的风险。
3. 行锁保持逐线程行序与行内容完全不变（驱动评审三铁律：配对不变、无新增异常路径、
   一次性日志语义不变）；热路径无 PTI_ASSERT、无堆分配（栈上 snprintf）。
4. `[TSBAD]` 原为无门控 → 保持无门控（仅换写形），可见性一字不改。
   `[TSLOG2]`（TS2 异常模式，canonical 关）与一次性 `[BATCHQKT]` 通知行不动。

### 2.1 Diff（仅 ze_collector.h，+62/-36 行）

- 新增 include：`<cinttypes>` `<cstdio>`。
- 类内新增 `TsDiagWrite`（static mutex + 一次 `fwrite(stderr)`）与
  `TsDiagEnabled`（缓存 getenv），放在 `BatchQktDiagEnabled` 旁，风格对齐。
- 5 个站点改写：`[TSCONV]`、`[TSLOG] on_event`、`[TSBAD]`、`[BATCHQKT] batch_us`、
  `[BATCHQKT] poll_read_us`。行内容逐字段与 legacy cerr 一致（bool→%d、void*→%p，
  非空指针两者同为 0x…；null 时 %p 打 `(nil)`，cerr 打 `0`，这两条路径 ev 非空）。

### 2.2 本地语法门

- g++ 15.2 `-std=c++17 -Wall -Wextra -Werror -Wformat -fsyntax-only` 于逐字提取的
  编辑区域（stub 类型替身）：PASS。
- 运行时输出与 legacy cerr 逐字节对齐（5 行样本比对）。
- 本地无 level_zero 头，全文件编译以 node1 build 容器（icx）为准。

## 3. 验证闭环（结果区）——全部完成 2026-10-16 04:29–05:04

### 3.0 重编

- build_node1.sh PASS（fresh pair，icx）：lib=`782fd14ee9b6c46b9f5a981df067879c`、
  launcher=`50a3810fe6332776c2f9a86bbba17b9b`（launcher 与旧对相同 md5——只有 lib 变）。
  形态门过（SVML 4 件 + libxptifw.so.1 + not-found=0 + strings）。
  staging：`$BASE/tsfix_stage/bin`（copy-never-delete）；原 lib 备份
  `ze_collector.h.pre_tsdiag`（md5 0a6d1c28…）。

### 3.1 四臂结果（runner_tsfix_node1.sh，全部 PASS，ALL-DONE rc=0）

| 臂 | Mean ITL | Median | P99 | tslog 粘行 | meta | trace B |
|---|---|---|---|---|---|---|
| f1o（meta OFF） | **37.89** | 37.99 | 46.95 | **0** | 0 | 137,111,357 |
| f1or2 | **37.79** | 38.11 | 47.30 | **1** | 0 | 137,111,325 |
| f1a（meta ON） | **37.85** | 37.96 | 46.43 | **0** | 3,625 | 138,001,897 |
| f1ar2 | **37.90** | 38.02 | 47.08 | **0** | 3,626 | 138,003,529 |

对照（同窗、同内核 7.1.0-set-mert-only-on-87+、无重启）：

| 臂 | Mean ITL | vs bare 36.39 | P99 |
|---|---|---|---|
| bare（01:39 锚） | 36.39 | — | 37.15 |
| m1off 修前 | 39.68/39.32 | +9.0% | 46.86/47.41 |
| **f1o 修后** | **37.89/37.79** | **+4.1%/+3.9%** | 46.95/47.30 |
| m1a 修前 | 40.60 | +11.6% | 48.35 |
| **f1a 修后** | **37.85/37.90** | **+4.0%/+4.1%** | 46.43/47.08 |

- **collector 税 +9.0% → +4.0%，落在预期的 +3~4% 带上沿**（<5% 停止线，未触发）。
- meta 专项差（f1a − f1o）= +0.04ms（0.1%）→ 修前 +0.92ms（+2.5%）的 meta 专项
  差异也随 cerr 系统调用消失（TSCONV/TSLOG 写位于 meta/poll 消费路径内）。
- P99 修后 46.4~47.3 ≈ 修前 m1off 水平；BQ poll P99 尾（~47 vs bare 37.15）仍在，
  与判别臂结论一致（BQ poll 为 P99 驱动，meta/tslog 不是）。

### 3.2 三签名裁决

- **S1 tslog 丢行率 → 0：PASS，且比预期更干净。**
  同口径粘行计数（标签出现在行首以外）：修前 TSCONV 201 + TSLOG 4,871 + BATCHQKT 1
  = 5,073；修后 f1o=0、f1or2=1、f1a=0、f1ar2=0（残差 1 条为未持锁的冷路径 cerr 碎片
  与我方 fwrite 的碰撞，272k 行里 1 条，量级忽略）。
  **更强证据：TSCONV 与 TSLOG 总数修前 138,495 vs 133,825（差 4,670 = 丢失行）；
  修后四臂全部恰好 138,696 == 138,696，逐命令配对精确。**TSBAD=0。
- **S2 qkt_batch ~1.5ms/步：未达（如预警）。** 同口径 MetaRecord 求和：
  修前 6.45ms/步 → 修后 6.25/6.76ms/步（未变）。构成 qkt_append_us 6.25ms/步 +
  qkt_sync_us 1.2µs/步 —— append（zeCommandListAppendQueryKernelTimestamps 轮询侧
  挂钩）是驱动侧成本，cerr 修复原理上就不触及。max_batch_us 波动 14~26ms。
- **S3 emit ~2ms/步：按同口径 PASS（1.59ms/步）。** 修前 m1a 同口径
  emit=21.70ms/步、handoff=8.52ms/步；修后 emit=1.59、handoff=0.49。
  诚实说明：10-01 报的 emit 22.50ms/步就是本口径（MetaRecord slice 求和），即
  **cerr 系统调用一直藏在 emit/handoff slice 内**——本修把它们拉回 1.59/0.49ms；
  两 slice 合计 −28.1ms/步，但 step 墙钟只降 3.0ms（大部分原来与 device-busy 重叠，
  at="poll"），与 ITL −2.75ms 的实测吻合。

### 3.3 t14 收口（协调者指令 4；判定线一字不改）

对 f1a 重跑 t14_accept.py（**不带 --allow-tslog-loss/--flags-json 旁路**——tslog
已完整，1:1 前提恢复，T8 由 NOT EVALUABLE 变为可真判）：

| 线 | 修前 m1a | 修后 f1a |
|---|---|---|
| M1 meta census | FAIL（2 条 benign 嵌套例外，step-396 后 shutdown 相位） | FAIL（同样 2 条 benign；handoff 1066→853、meta 3838→3625） |
| A1 形状对照 | SHAPE-DIFF-EXPECTED（replay ops 118,001） | SHAPE-DIFF-EXPECTED（replay ops **121,572 = 396×307 精确**；ITL 结构 4×98 True，修前 False） |
| ITL ±2% vs bare | FAIL +11.57% | **FAIL +4.52%**（trace 口径；bench 口径 +4.0%）——仍是 letter FAIL，但税已过 5% 停止线以下 |
| 完整性（TSBAD/footer/dmesg） | letter FAIL（dmesg 14 行 C6 噪声） | letter FAIL（dmesg 18 行 C6 噪声，同归类） |
| A5 unexplained | 2（首图 instantiate） | 2（同样两条） |
| **T8** | **NOT EVALUABLE**（tslog 丢失打破前提，exit=2 summary=None/7） | **真判 5/7**（exit=0）：CHECK1 FAIL=税本身（trace ITL +4.52% vs bare）；CHECK3 FAIL=1 处 7.75µs 同队列 eager memcpy ts 重叠，raw tick 证实为**真实设备并发**（非追踪伪影）；CHECK2/4/5/6/7 全 PASS，trace↔tslog 138,696↔138,696 1:1，漂移 0.25µs（=量化步长） |

OVERALL 仍 FAIL（letter 线：ITL ±2% 未达、dmesg C6 噪声、A5 两条 instantiate）——
与修前同类，但税从 +11.6% 降到 +4.0~4.5%，T8 从不可判变为 5/7 且两 FAIL 均有
明确定性（一为税本身、一为真实设备并发非伪影）。

### 3.4 边界与遗留

- 停止线（修后税 >5%）：未触发（+4.0~4.5%）。
- 单写线程选项：按证据未采用（§2），无扩战。
- 遗留给后续（不在本战役内）：qkt_append 驱动侧 6.25ms/步（append_us 主导，
  修前修后不变，S2 未达的根因）；BQ poll P99 尾（~47 vs bare 37.15）；
  冷路径 cerr 未持锁（272k 行 1 条碰撞，无实际影响）。

## 4. 机器内核更换（单独一节，不混入 unitrace 税账）

**非我方代码可归因域，建议机器组/IFWI 关注。**

- 10-01 GPU0 正式 A/B（vt1 32.78 / vt0 33.10）运行在
  `7.1.0-lgci-xe-po-cri-int-80-062c1b46fde-debug+`；10-02 19:30 起机器换为
  `7.1.0-set-mert-only-on-87+`（#88，10-02 05:00 CST 安装，`last reboot` 佐证）。
- 换核后 device 侧内核时长整体 +16~24%（gemm p50 0.212→0.262ms；prefill
  attention 101→114.7ms），频率同为 2500MHz —— 慢在设备侧，非 host 干扰。
- 本窗所有 ITL 数字（bare 36.39、m1a 40.60、m1off 39.68）都建立在新内核上；
  与 10-01 的对比（+23.9%）属跨窗环境漂移，不应记在任何 unitrace 改动头上。

## 5. raw 留档

node1（原样保留，copy-never-delete）：
- `$BASE/tsfix_stage/bin/`（lib=782fd14e… + launcher=50a3810f…）
- `$BASE/src/unitrace_src/tools/unitrace/src/levelzero/ze_collector.h`（修后，
  md5 9382952fce5567242aceed9243f0147b）+ `ze_collector.h.pre_tsdiag`（原版 0a6d1c28…）
- `$BASE/results/{f1o,f1or2,f1a,f1ar2}/`（trace 137~138MB、tslog、bench/watcher/
  server 日志、dmesg inc、t14 产出 a5_f1a_accept.json/A5_f1a_ACCEPT.md/t8 json+log）
- `$BASE/results/verdicts_tsfix.txt`、`tsfix_session.log`（ALL-DONE rc=0）
- `$BASE/logs/build_20261016_043008.log`（重编全记录）

本地镜像：`/home/tina/DEV/unitrace_debug/results/step2_meta_impl_20260929/build11/`
- `TSLOG_FIX_NODE1.md`（本报告）
- `tslogfix_src/src/levelzero/ze_collector.h`（修后源）+ `tslogfix_src/ze_collector.orig.h`（原版）
- `node1_scripts/runner_tsfix_node1.sh`
- `m1fix_raw/`（verdicts、session log、bench_full ×4、t14 json/report/t8 log）
- traces 不拉回（node1 本地留档，按既有纪律）
