# unitrace vLLM XPU 采集改进 — 工程团队第一波验证交接（handoff）

> 2026-10-22。适用代码状态：`ffac356`（master HEAD）。
> 本文档 = 交付验收入口：改进清单 / 验收标准 / 已有测试结果 / 第一波验证指南。
> **用户手册（构建→部署→开新功能→profile→读结果全流程）= `USER_MANUAL.md`**。
> 细节文档索引：构建与开关 = `BUILD_AND_SWITCHES.md`；时间戳修复 = `TSLOG_FIX_NODE1.md`；
> 影子完成事件 = `BQ_S2_DESIGN.md`；开销标注设计 = `META2_DEVICE_MARKS_DESIGN.md`。

---

## 1. 提交史与改进一览（#1 → #8）

| # | commit | 改进 | 一句话说明 |
|---|---|---|---|
| 1 | `bbe5e6b` | v3 postfix | graph replay 内 kernel 时间戳错乱修复（本系列起点） |
| 2 | `44db6ff` | v4b/v4c batch QKT | 批量 `AppendQueryKernelTimestamps`（AT_POLL 延迟读），替逐 kernel 查询 |
| 3 | `45c2710` | meta 注解 + BATCHQKT 加固 + 出货默认开关 | meta 标注通道、`UNITRACE_GRAPH_BATCH_QKT` 等；**出货特性默认全开，`=<开关>=0` 可单独关** |
| 4 | `78c7c0e` | TS_INTERP + tslog 写路径修复 | 设备时间戳插值摊销 + tslog 行级锁/单 fwrite（emit 21.7→1.59ms/步，粘行 5,073→≤1） |
| 5 | `0fa7330` | BQ-S2 影子完成事件 W | 每 graph replay 流尾 append 影子 signal，消除 reset batch；零新增用户可见 GPU op |
| 6 | `7c0cbc6` | review-fix round | tslog/TS_INTERP/影子 W 加固（评审问题收口） |
| 7 | `4602cff` | B+D 开销标注（host 层） | host 切片注 `collector_us`/`api_net_us` + 生命周期总量 `unitrace.overhead_total`（D 层） |
| 8 | `ffac356` | META=2 设备侧标注（C 层） | `unitrace.shadow_w_signal`/`qkt_read_device`（设备 track instant）+ `shadow_w_append`（host 窗）+ `device_marks_n`；**零新增 GPU op**（从既有 qkt 包派生） |

**核心设计原则**（贯穿 #7/#8，`BUILD_AND_SWITCHES.md` 原则块）：时间轴必须真实、
不从时间轴上减开销；开销可见 / 可归因 / 可过滤；「净耗时」走查询字段不篡改时间戳；
META=0 定位是回归工具。

## 2. 开关语义（验证需知的全部开关）

| 开关 | 行为 | 字节等同性 |
|---|---|---|
| `UNITRACE_TRACE_META=0`（或不设） | 关闭 meta 标注（回归工具定位） | 与基线逐位同 |
| `UNITRACE_TRACE_META=1` | host 层 B+D 标注 | 与 #7 逐位同 |
| `UNITRACE_TRACE_META=2` | = 1 + C 层设备 mark（用户完整模式） | 新增 mark 行/事件 |
| `UNITRACE_GRAPH_BATCH_QKT=1` | 批量 QKT 采集（出货默认开） | `=0` 回退逐 kernel 查询 |

meta 标注在 trace 里的形态：host 切片 args 增量（`collector_us`/`api_net_us`）、
instant 事件（cat=`unitrace_meta`，cname=grey）、生命周期收口 `unitrace.overhead_total`
（args：`collector_total_us`/`meta_records_n`/`device_marks_n`）。

## 3. 验收标准（预注册判据，全部战役同一口径）

结构门（每臂必过）：
1. tslog footer 恰 1（"Timeline is stored"）；completion 正常返回 content
2. **dmesg-bad = 0**——全程地址越界/非法访问/force wake 零命中（这类条目是 wedge
   前兆，测试中出现任一立即停手，不得继续跑）
3. census 对账：meta 记录数与预期精确相等（如 replay 数 × 每步记录数）、
   `tslog-glued`（粘行）≤1、影子 W 事件 exact 校验（BQ-EXACT/BQ-W）通过
4. **md5 配对门**：`bin/unitrace`（launcher）与 `libunitrace_tool.so` 必须同一次构建
   （混用旧 launcher 启动即死 "Versions do not match"）

字节等同性门（开关矩阵）：
5. META=0 / META=1 与各自基线**逐位同**（md5/字节对照）

税判据（mean 主口径；median/P99 记账不判）：
6. 增量 ≤ +0.3pt → PASS；0.3~0.8pt → REPORT-DECISION；> 0.8pt → STOP（停下归因，
   不放行）
7. 诊断无放大：META 各档的 `[TSCONV]` 行数/tslog 字节必须与 META=0 基线**相等**
   （#8 教训：二次转换会重发诊断行，此判据防回归）

## 4. 已有测试结果汇总（内部验证，均可复现）

负载基准：vLLM XPU decode（Qwen3.8-27B trim24L / Qwen3-32B 形态），ITL mean 主口径。

| 战役 | 机器 | 结果 | 判定 |
|---|---|---|---|
| #2/#3 batch QKT（v4c） | 内部测试机 A（BMG） | 税 +2.9% | 生效（BATCHQKT 在 NEO 26.27 起） |
| #2/#3 正式 ×2 | 内部测试机 B | 税 +1.4% | PASS |
| #4 TS_INTERP A/B | node1 | vt1 快 ~1%；tslog 修复后税 +9.0%→+4.0~4.5% | PASS |
| #5 BQ-S2 三 rep | node1 | 税 +2.9~3.8%，均优于上一基线 | PASS |
| #5 BQ-S2 正式 ×2 | 内部测试机 A | 税 +1.4% | PASS |
| 回归定格 rg1 | node1 | nesting 1581/1583 逐位同、税 +4.0%、T8 6/7&5/7 | REGRESSION-PASS |
| N1-T1 复测 | node1 | 税 +2.4% bench / +2.9% trace ≤3% 线 | 关闭 |
| #7 B+D 增量 | node1 | **−0.081%**（理论 ≈0 实证）；总税 +2.19% vs bare | PASS |
| #8 META=2 冒烟 | node1 | C-1=C-2=C-3=32、device_marks_n=64 精确对账、mark 落 gpu_op track | PASS |
| #8 META=2 税复测 | node1 | **增量 +0.275%**、总税 +0.749% vs bare；TSCONV 四臂逐位同 138,696 | PASS |
| #8 META=2 税复测（修复前两轮） | node1 | +1.185% / +1.087% → STOP，根因 = TSCONV 诊断行放大（396 步 × 307 包精确闭合），已修 | 已关闭 |

注：总税 +2.2~2.4% 的主体是 collector 基础采集（非本系列新增），分层归因见
`node1 判别臂` 数据（collector 基础 +9.0% → tslog 修复后 +4.0% 段；meta 标注仅 +2.5%）。

## 5. 第一波验证指南

### 5.1 构建

按 `BUILD_AND_SWITCHES.md` §1：构建机容器 fresh configure + ninja（`ninja unitrace
unitrace_tool`，**必须全目标**——只编 `unitrace` 不重链 `.so`）；产物自检：
`strings` 命中 `qkt_at_poll`/`GRAPH_BATCH_QKT` ≥1、`ldd` 命中 libsvml/libimf/libirng/
libintlc/libxptifw 共 5。header 改动先过本地语法门（§1.2）。

### 5.2 冒烟（最小验收集）

1. META=0 冒烟：与基线版本 trace 逐位对照（字节等同性门）
2. META=2 冒烟：trace 中 grep 三个 mark 名 + `device_marks_n` > 0；mark 的 tid 落在
   gpu_op track 集合内；META=1 臂三个 mark 名计数全 0
3. 每次测试实时监控 dmesg（红线见 §3.2）
4. chrome trace 必须能 grep 到 `urEnqueueGraphExp`（因果检查 host 侧锚点存活）

### 5.3 标准 profile 命令（工程师人工 profile 基准形态）

SERVER：

```bash
VLLM_XPU_USE_CUSTOM_MODEL=1 VLLM_XPU_ENABLE_XPU_GRAPH=1 ZE_AFFINITY_MASK=0 \
NEOReadDebugKeys=1 EnableImplicitConvertionToCounterBasedEvents=0 \
<path>/unitrace --chrome-itt-logging --chrome-sycl-logging --chrome-call-logging --chrome-kernel-logging \
--output-dir-path <out-dir> --start-paused \
vllm serve <model-path> --trust-remote-code --tensor-parallel-size 1 \
--max-model-len 8192 --no-enable-prefix-caching --enable-chunked-prefill \
--max-num-seqs 128 --profiler-config.profiler xpu --shutdown-timeout 120
```

CLIENT：

```bash
no_proxy=127.0.0.1 vllm bench serve --base-url http://127.0.0.1:8000 \
--model <model-path> --dataset-name random \
--random-input-len 3500 --random-output-len 100 --max-concurrency 1 \
--num-prompts 2 --request-rate inf --num-warmups 2 --ignore-eos \
--ready-check-timeout-sec 0
```

已取证要点：**四件套 `--chrome-*-logging` 必须全带**（裁掉=未测形态）；unitrace 验证臂
**刻意不带客户端 `--profile`**（采集由 `--start-paused` + session resume 控制，避免
vLLM 自家 profiler 在 bench 中段插 kernel）；`EnableImplicitConvertionToCounterBasedEvents=0`
逐字保留（QKT 时间戳路径相关）；serve/client 模型路径必须一致。

## 6. 已知边界与未结项（如实声明）

1. **protobuf 设备 mark = instant 形态已实现**（2026-10-22：`--output-format=protobuf`
   下 shadow_w_signal/qkt_read_device 落 device track，实迹 wire 解析验证通过；
   UI 视觉渲染待复核——若渲染不佳回退 1µs thin slice，设计稿 §5/§10.4）
2. 驱动在 graph 内部对物理事件每代 re-signal（S3）**无锚点可标**——属驱动侧改进项
3. shadow-W 的真实设备执行时长不可直接测（测它需要注入新设备 op，违反零新增原则），
   instant 是诚实的下界表达
4. round-27 附近长跑停顿（≈3.77GB trace）已复现 4 次，Tina 裁决暂不处理
5. 税数据均为单机基准（node1 card1 / 内部测试机 A/B，特定模型形态）；跨机型/跨负载
   外推需第一波验证补充
6. 税判据 mean 主口径为 2026-10-21 裁决；median/P99 记账不判

## 7. 第一波验证建议清单

| 项 | 内容 | 优先级 |
|---|---|---|
| V1 | META=0 字节等同性（对基线 md5）——回归安全性 | 高 |
| V2 | META=2 冒烟 + census 对账（§5.2） | 高 |
| V3 | 税复测（同 §5.3 命令形态，META=2 ×2 vs META=0 ×2 + bare 锚，§3.6 判据） | 高 |
| V4 | 跨机型（非 node1 的研发/生产机型）税与稳定性 | 高 |
| V5 | Perfetto UI 上 mark 可读性（设备 track instant 与 kernel 切片同 track 渲染） | 中 |
| V6 | protobuf 输出复验（instant 已实现并 wire 验证；UI 视觉渲染确认，不佳则 thin slice 回退） | 中 |
| V7 | 长跑 soak（trace 体积、drift、round-27 停顿是否复现） | 中 |
