# unitrace vLLM XPU 用户手册（POC 交付版）

> 2026-10-22。适用代码状态：master `c4ab6fa`（含 #1-#10 全部改进）。
> 本手册目标：拿到本 poc 的用户从零完成 **构建 → 部署 → 开新功能 → profile → 读结果**。
> 深度设计文档索引见 §9；开关底层细节见 `BUILD_AND_SWITCHES.md`；验收入口见
> `ENGINEERING_HANDOFF.md`。

---

## 1. 这个版本里有什么

在 upstream unitrace 基础上，本 poc 针对vLLM XPU（xpu-graph decode）场景做了 10 项改进
（#1-#10，详见 `ENGINEERING_HANDOFF.md` §1）：

- **图时间戳修复与加速**（#1-#5）：graph replay 内 kernel 时间戳修复、批量 QKT 采集、
  影子完成事件 W（消除 reset batch）、TS_INTERP 插值摊销、tslog 写路径修复。
- **开销分层标注**（#7/#8，`UNITRACE_TRACE_META=1/2`）：采集自身引入的开销在 trace 里
  可见、可归因、可过滤——host 层（B/D）+ 设备层（C）。
- **双路输出**（#10）：chrome JSON（默认）与 protobuf（pftrace）双格式，设备 mark 两路
  均落设备 track。

设计原则（理解所有开关行为的钥匙）：**时间轴必须真实、不从时间轴上减开销；开销可见 /
可归因 / 可过滤；"净耗时"走查询字段不篡改时间戳**。因此 trace 里看到的时长都是真实
发生的时间，采集开销以标注形式叠加显示，而不是被扣掉。

## 2. 构建

### 2.1 构建环境（与全部已验证产物同链路）

- Linux x86_64 + oneAPI 工具链（icx/icpx）+ Ninja + cmake ≥ 4.x。
- 参考容器：cri2 构建机 `tf_unitrace_v4c2`（含 L0 头缓存与 libxptifw——L0/OpenCL/ITT
  头 configure 期会自动下载，**离线集群必须预缓存**）。
- 构建目标：`ninja unitrace unitrace_tool`（**两个目标都要**——只编 `unitrace` 不会
  重链 `libunitrace_tool.so`，产物仍是旧库）。

```bash
# 容器内 fresh configure（绝不复用陈旧 build 目录）
SDK=<源码根>/unitrace_sdk; B=$SDK/tools/unitrace/build
cd $B && rm -rf CMakeCache.txt CMakeFiles
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja unitrace unitrace_tool
md5sum $B/unitrace $B/libunitrace_tool.so
```

关键 cmake 默认值：`BUILD_WITH_PERFETTO=ON`（protobuf 支持；失败自动降级 JSON-only）、
`BUILD_WITH_L0/ITT/XPTI/OPENCL/OMP` 全 1、`BUILD_WITH_MPI` Linux ON（ITT=0+MPI=1 会
FATAL）。编译器手设 `CC=icx CXX=icpx` 即官方路径。tools/unitrace 无 CRI/集群专用开关
（全树探索结论：代码层面零感知）。

### 2.2 产物自检（每次构建后必做）

```bash
# ① 符号门：新特性已编入（各计数 ≥1）
strings libunitrace_tool.so | grep -c "qkt_at_poll\|GRAPH_BATCH_QKT"
# ② 依赖门：必须为空
ldd libunitrace_tool.so | grep "not found"
# ③ 五库门：libsvml/libimf/libirng/libintlc/libxptifw 共 5 个命中
ldd libunitrace_tool.so | grep -cE "svml|libimf|libirng|libintlc|xptifw"
```

若缺 libxptifw：从构建容器 `/opt/gfx-deps/dpcpp/lib/libxptifw.so.1.0.1` 取出，
目标机建 `.so.1` symlink；**同进程只允许一套 libxptifw**（双套 → phmap assert SIGABRT）。

### 2.3 版本配对门（⚠ 最常见翻车点）

`bin/unitrace`（launcher）与 `libunitrace_tool.so` 必须来自**同一次构建**。
混用旧 launcher 启动即死：

```
[ERROR] Versions do not match
```

部署时两件套一起拷贝。判别方法：`md5sum` 两件套并与构建日志对照。

## 3. 快速上手：标准 profile 流程

### 3.1 SERVER 启动（标准形态）

```bash
VLLM_XPU_USE_CUSTOM_MODEL=1 VLLM_XPU_ENABLE_XPU_GRAPH=1 ZE_AFFINITY_MASK=0 \
NEOReadDebugKeys=1 EnableImplicitConvertionToCounterBasedEvents=0 \
<path>/bin/unitrace --chrome-itt-logging --chrome-sycl-logging \
  --chrome-call-logging --chrome-kernel-logging \
  --output-dir-path <out-dir> --start-paused \
  vllm serve <model-path> --trust-remote-code --tensor-parallel-size 1 \
  --max-model-len 8192 --no-enable-prefix-caching --enable-chunked-prefill \
  --max-num-seqs 128 --profiler-config.profiler xpu --shutdown-timeout 120
```

已取证要点（勿自行裁剪）：
- **四件套 `--chrome-*-logging` 必须全带**（itt/sycl/call/kernel；裁掉 = 未测形态）。
- `EnableImplicitConvertionToCounterBasedEvents=0` 逐字保留（QKT 时间戳路径相关）。
- `--start-paused`：进程起来后采集处于暂停态，等下面 §3.3 的 session 指令开闸——
  避免 warmup 与启动期噪声进 trace。
- **unitrace 验证臂刻意不带客户端 `--profile`**（vLLM 自家 profiler 会在 bench 中段
  插 kernel，污染时间轴）。
- serve/client 模型路径必须一致。

### 3.2 CLIENT 压测

```bash
no_proxy=127.0.0.1 vllm bench serve --base-url http://127.0.0.1:8000 \
  --model <model-path> --dataset-name random \
  --random-input-len 3500 --random-output-len 100 --max-concurrency 1 \
  --num-prompts 2 --request-rate inf --num-warmups 2 --ignore-eos \
  --ready-check-timeout-sec 0
```

### 3.3 Session 控制（采集开闸/关闸）

`--start-paused` 启动后，用 launcher 子命令控制采集窗口（`<session>` 为任意名，
与 `--session` 对应；不给 `--session` 时用默认名）：

```bash
<path>/bin/unitrace --resume <session>   # 开闸：开始采集
...  # 让负载跑想要观察的区间
<path>/bin/unitrace --pause <session>    # 关闸：暂停采集（进程保持运行）
<path>/bin/unitrace --stop <session>     # 结束并落盘（也可随进程退出自动落盘）
```

最小流程：SERVER 起来就绪后 → `--resume` → 跑负载 → `--pause`（或直接结束进程）→
读 `<out-dir>` 产物。

### 3.4 输出格式：JSON（默认）与 protobuf

```bash
# 默认 chrome JSON（.json）
# protobuf（.pftrace，Perfetto 原生）：
<path>/bin/unitrace --output-format protobuf ...   # CLI 形态
# 或等价 env：
UNITRACE_OutputFormat=protobuf <标准 SERVER 命令>
```

- protobuf 需要 `BUILD_WITH_PERFETTO=ON` 构建；未编入时启动会打
  `[WARNING] ... built without ...` 并自动回退 JSON。
- JSON 是验收/对照的标准格式；protobuf 面向 Perfetto UI / trace_processor 分析。

### 3.5 产物清单与多进程布局

`<out-dir>/`（多进程负载下每进程一组文件）：

| 文件 | 内容 |
|---|---|
| `python3*.json` / `python3*.pftrace` | chrome trace / Perfetto trace |
| `unitrace_tslog.log` | 时间戳换算诊断日志（footer 行 `Timeline is stored` 恰 1 为结构健康） |
| 控制台 stderr | `BQ-SUMMARY`、`unitrace.overhead_total`、meta 统计行 |

⚠ vLLM = API server + EngineCore 多进程：**真正的 trace 在 EngineCore 进程**
（最大的那个 python3* 文件）；API server 的近空 trace 是正常现象。做计数统计时
**跨全部 python3* 文件求和**（`grep -c` 多文件会带文件名前缀，用 `cat f* | grep -c`）。

## 4. 环境变量总表

### 4.1 出货特性——默认全开，不设任何 env 即完整功能；`=0` 单独关闭（A/B 用）

| 开关 | 默认 | 作用 |
|---|---|---|
| `UNITRACE_GRAPH_BATCH_QKT` | ON | 图时间戳批量读（290 事件/步一批，净回收 4.25ms/步；关=大回退） |
| `UNITRACE_GRAPH_QKT_AT_POLL` | ON | 批量读武装在设备忙的 poll sweep；关=退回 drain 解析 |
| `UNITRACE_DEFERRED_TS` | ON | eager 命令时间戳处理挪 completer 线程（−0.48ms/步） |
| `UNITRACE_TRACE_META` | ON(=1) | 开销标注 host 层（B+D）；**=2 加设备层（C）；=0 全关（回归工具）**，详见 §5 |
| `UNITRACE_GRAPH_BATCH_QKT_RESET` | ON | 事件批量 reset |
| `UNITRACE_TS_INTERP` | ON | GTS 摊销插值（prepk 472→86µs/步；速率自校准，对 timerResolution 字段单位免疫） |
| `UNITRACE_GRAPH_QKT_SHADOW_W` | ON | 影子完成事件 W（reset batch 整体消除；六计数器健康形态 = pops==released==submissions、其余 0） |

### 4.2 META 标注档位（#7/#8 新功能，重点）

| `UNITRACE_TRACE_META` | 形态 | 用途 |
|---|---|---|
| 不设 / `=0` | 无任何 meta 记录，其余与基线**逐位等同** | 回归对照、字节等同性验证 |
| `=1` | host 层标注（B+D） | 常规 profile 的推荐档 |
| `=2` | = 1 + 设备层标注（C） | 需要看采集在**设备时间轴**上的操作时用（用户完整模式） |

### 4.3 输出与目录

| 开关 | 作用 |
|---|---|
| `--output-format protobuf` / `UNITRACE_OutputFormat=protobuf` | 输出 pftrace |
| `--output-dir-path <dir>` | 输出目录 |
| `UNITRACE_UseResultDirectory` | 结果目录组织（logger_factory） |

### 4.4 子开关 / 实验 / 诊断（默认关）

| 开关 | 作用 |
|---|---|
| `UNITRACE_TS_INTERP_DELTA` | 插值锚点重同步窗（默认 300000ns） |
| `UNITRACE_DEBUG_TSINTERP` | 每次重同步打一行 |
| `UNITRACE_DEBUG_TS` / `TS2` / `DEFER` / `BATCHQKT` / `EMIT` | 各路诊断（TS2 会强制 legacy 路径，**只用于调试，勿用于测量**） |
| `UNITRACE_TEST_ALLOC_FAIL` / `UNITRACE_TEST_SHADOW_W_FAIL` | 注入失败验证优雅降级（预期无 fault + degrade 计数升高） |
| `UNITRACE_ASYNC_EMIT` / `UNITRACE_TS_EXT` | 实验项 |

## 5. 开销标注读法（META=1/2 在 trace 里看到什么）

### 5.1 host 层（META=1 即有）

- **host API 切片 args**：`collector_us`（该切片内包含的采集子窗之和）与
  `api_net_us`（切片总长 − collector_us）。有债才写字段，干净切片无新字段。
- **instant 事件**：cat=`unitrace_meta`（`unitrace.enqueue` / `unitrace.emit` /
  `unitrace.qkt_batch` / `unitrace.step` / `unitrace.shadow_w_append` 等）。
- **生命周期收口**：trace 末尾 instant `unitrace.overhead_total`，args：
  `collector_total_us`（生命周期采集债累计）、`meta_records_n`、`device_marks_n`
  （META=2 时）。这是"这次 profile 里采集一共花了多少"的一句话答案。

### 5.2 设备层（META=2 新增，三个 mark）

| mark | track | 形态 | 语义 |
|---|---|---|---|
| `unitrace.shadow_w_signal` | 设备 track（与 kernel 同轨） | instant（dur=0） | 每 graph replay 的影子 W signal 设备时刻 = 本步最后结束 kernel 的 end（max end，多 engine 并行安全） |
| `unitrace.qkt_read_device` | 同上 | instant | 每 step 的批量 QKT 读设备时刻；arm/drain 两路都发 |
| `unitrace.shadow_w_append` | host track | 切片窗 | W 注入的 host 成本，自动归属进包含它的 graph host 切片的 `collector_us` |

设计要点（读数时勿误判）：
- mark 的 ts 是**设备锚点时间**（本步成功包 max kernel end），不是 host drain 时刻。
  因此收尾 drain（如 pause 时的最后一条 `qkt_batch` at=poll_sync）发出的 mark 在
  trace 里落在最后一个 kernel end 处，而不是 drain 记录旁边。对账口径：设备读总数
  （`at=drain` + `at=poll_sync` 记录数）= mark 数，逐位精确；勿按位置配对。
- instant 是**诚实的下界表达**——W signal 设备执行真实成本 sub-µs，但"测它需要注入新
  设备 op"违反零新增原则；instant 不表达时长、不把设备 idle 画成开销。
- 设备侧成本**不折算**进 `collector_total_us`（host µs）；它的表达就是 mark 本身
  （可见性）。`device_marks_n` 只计数。
- META=2 的标注税实测 **+0.275%**（vs META=0，mean 主口径，node1 card1 三轮复测 PASS）。

### 5.3 查询示例

```bash
# JSON trace：grep 计数（跨全部 python3* 文件）
cat out_dir/python3*.json | grep -c unitrace.shadow_w_signal

# pftrace：Perfetto trace_processor SQL
trace_processor trace.pftrace -q /dev/stdin <<'SQL'
select s.name, s.ts, s.dur, t.name as track_name
from slice s join thread_track t on s.track_id = t.id
where s.name like 'unitrace.%' order by s.ts;
SQL
```

UI：pftrace 拖入 https://ui.perfetto.dev ；JSON 拖入 chrome `chrome://tracing` 或
ui.perfetto.dev。设备 mark 与 kernel 同轨（同 track_id），在设备时间轴上直接可读；
同刻双 mark 在 UI 里按名字纵向堆叠。

## 6. 结果判读

1. **结构健康门**（每次 profile 先查）：tslog footer `Timeline is stored` 恰 1；
   completion 正常返回；`BQ-SUMMARY` 行 `batches>0、reset_fallback=0、collected==seen`；
   chrome/pftrace 有 `urEnqueueGraphExp`。
2. **ITL/吞吐**：以 `vllm bench serve` 输出的 Mean ITL 为主口径（median/P99 记账不判）。
   与同窗裸跑（不挂 unitrace）对比即采集税；规格线 ±3%，当前实测总税 +2.2~2.4%
   （主体是 collector 基础采集，分层归因见 `ENGINEERING_HANDOFF.md` §4）。
3. **税二分**：某开关 `=0` 后 ITL 不升 = 该开关无税贡献；显著升高 = 承重优化
   （如 `UNITRACE_GRAPH_BATCH_QKT=0` → +4.25ms/步大回退）。逐项二分法见
   `BUILD_AND_SWITCHES.md` §3.3。
4. **红线**：dmesg 出现地址越界/非法访问/force wake = wedge 前兆，**立即停手**
   （这类条目出现任一，不得继续跑）。

## 7. 排障 FAQ

| 症状 | 原因与处置 |
|---|---|
| `Versions do not match` 启动即死 | launcher 与 lib 不同次构建 → 两件套一起重拷（§2.3） |
| `[WARNING] ... built without ...` 回退 JSON | 构建时 BUILD_WITH_PERFETTO 失败 → 重构建并查 configure 日志 |
| `ldd` 有 not found / 启动崩 | 缺 libxptifw 或双套 libxptifw → §2.2 处置 |
| trace 里没有新特性记录 | 确认 md5 配对门 + strings 符号门（§2.2）；确认 env 档位（§4.2） |
| tslog 粘行/丢行 | 本版已修（行级锁+单 fwrite）；若复现请带 tslog 文件回报 |
| GPU 楔死前兆 | dmesg 地址越界/非法访问 → 立即停手并回报（勿继续跑） |
| vLLM 启动 onecclCommInitRank code 11 | 容器需 `-v /dev/dri/by-path:/dev/dri/by-path`（`--device /dev/dri` 不带 symlink 不够） |
| trace 尾部有一大段空白（最后活动之后到 trace 结束） | 真实空闲，非丢数据：负载结束后引擎待命，直到 pause/退出才收尾（最后一批 QKT 读的 drain 落在此刻）。判据：空白区三 track 零事件、未闭合切片 0、读/mark 对账精确 |
| protobuf trace 在 Perfetto UI 里 Start 显示 1970-01-01 时刻 | 显示约定，非数据错误：trace_processor 把 proto trace 归一到 BOOTTIME 主时钟（无纪元锚点），UI 按 time-of-day 渲染。事件本身携带 epoch ns + REALTIME 声明 + ClockSnapshot（数据正确）；相对时序不受影响 |
| `.pftrace` 无法当文本读取/上传工具报"无法处理" | 正常：pftrace 是 protobuf 二进制流（单包 Trace 逐包拼接）。用 ui.perfetto.dev 拖入、trace_processor SQL（§5.3）或二进制解析脚本查看；校验可用 trace_processor 正常加载为准 |

## 8. 税预算与当前实测（node1 card1 / 内部测试机 A/B，Qwen 形态）

| 配置 | 实测 |
|---|---|
| 总税（默认全开 vs 裸跑） | +2.2~2.4%（mean 主口径） |
| META=1（B+D）增量 | −0.081%（噪声级） |
| META=2（+C 层）增量 | +0.275% |
| protobuf 输出 | 结构门全过（32/32/32 mark 精确对账 + 官方 trace_processor 复证同轨） |

验收判据全文见 `ENGINEERING_HANDOFF.md` §3（结构门/字节等同性门/税判据/无诊断放大）。

## 9. 文档索引

| 文档 | 内容 |
|---|---|
| `ENGINEERING_HANDOFF.md` | 验收入口：改进清单/判据/已有结果/第一波验证清单 |
| `BUILD_AND_SWITCHES.md` | 开关底层细节、历史臂复现、产物 md5 台账 |
| `META2_DEVICE_MARKS_DESIGN.md` | 设备侧标注（C 层）设计 + 税复测复盘 |
| `BQ_S2_DESIGN.md` | 影子完成事件 W 设计 |
| `TSLOG_FIX_NODE1.md` | tslog 写路径修复报告 |
| `viz_mt2p_marks_zoom.png` + `mt2p_enginecore.pftrace` | 设备 mark 可读性样例（放大图 + 原始 trace） |
