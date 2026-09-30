# BATCHQKT 税总账：成因 / Perfetto 可见性 / 优化前后对照 / 剩余空间（备查）

> 日期：2026-09-30 深夜。基准=最终出货配置 **build8（lib a67920db）+ UNITRACE_DEFERRED_TS=1**，
> node3 trim24L，ITL **38.43/38.43 双 rep = +2.64% vs bare 37.44，PASS（规格 ≤38.57）**。
> 数字全部来自实测闭合（分解和 ≈ ITL 差，闭合精度 2.4µs/步），出处
> `BQ_TAX_HOTSPOT_AND_OPT.md` §4-§7、bqu/bqc/bqj/bql 各臂 trace。
> 用途：备查手册——任何人复查"税有哪些、在 Perfetto 里长什么样、优化改了什么、还剩什么"以此为准。

## 0. 税梯总览（对照用）

| 配置 | ITL (ms) | 税 vs bare | 说明 |
|---|---|---|---|
| bare（无插桩） | 37.44 | 0 | 锚 |
| u0（unitrace 基线，BQ/AT_POLL/META 全关） | 43.41 | +15.95% | 纯采集基线税 +5.97ms/步 |
| v4b（仅 BATCHQKT） | 39.91 | +6.60% | BQ 净回收 4.25ms/步 |
| v4c / build3（BQ+AT_POLL+META） | 39.16 | +4.59% | 战役起点 |
| vde（+DEFERRED_TS 零代码） | 38.67/38.70 | +3.3% | 优化① |
| **vde3 / build8+DEFERRED_TS（出货）** | **38.43/38.43** | **+2.64% PASS** | 优化② |

---

## 第 1 类：设备执行拉长税（device-side）——已被 BATCHQKT 回收

- **数值**：u0 43.41 → v4c 39.16，即 **4.25ms/步**（全场约占税梯大头）。
- **成因**：legacy 路径对每个 kernel 节点单独挂 timestamp 事件并逐个 signal/read，
  GPU 上 kernel 之间被插入事件操作，设备执行被拉长（u0 的 gexp 相位 5.80ms/步 vs v4c 1.53ms/步）。
- **原实现**：per-node timestamp 事件，每节点一次设备侧事件往返。
- **优化后实现**：BATCHQKT——290 个图时间戳事件挂 collector 专用 immediate list 批量执行，
  poll 相位一次性批量读（qkt_batch at="poll"），设备侧从"每节点事件"变"每步一批"。
- **Perfetto 可见性**：✅ **最显眼**。GPU 轨道 kernel 时长/kernel 间距直接变长，肉眼可辨。
- **剩余空间**：无——此类已被净回收，BATCHQKT 不可关（关了退回 u0 口径）。

## 第 2 类：Host 暴露税（延长 ITL，住在步间空隙）——优化主战场

全部住在 `urEnqueueGraphExp` cpu_op 切片内部。优化前（build3）暴露合计 ≈1.72ms/步，
优化后（build8+DEFERRED_TS）残余 ≈**0.99ms/步**。

| 子项 | 前 (build3) | 后 (build8+DT) | 成因 | 原实现 → 优化后实现 | 剩余空间 |
|---|---|---|---|---|---|
| d_loop（drain 内 eager 命令 inline 处理） | **798** | **34** | ~40 个 eager 命令/步在 drain 内 inline 走 zeEventQueryStatus+时间戳读+emit（~20µs/条）；其事件步末才 signal，app 末次 poll 看不到，只能等 drain | inline `ProcessCommandSubmitted` → `UNITRACE_DEFERRED_TS=1` 零代码改走 completer 线程（ShouldDeferCommand 现成机制），d_loop 798→36 | 无（已归零水位） |
| d_flush2（drain 出口 flush 屏障） | **261** | **0** | gexp staging drain 等 completer 尾巴；deferred 命令完成时间 > drain 出口 | 每处 drain 都等 `FlushDeferredTimestamps` → build8 `at_gexp` 时跳过出口屏障；正确性卡点=下一 drain **入口**屏障（实测 1.1µs，Fix B 保留）；teardown/finalize/fence 全保留 | 无 |
| d_resets（reset batch） | 541 | **557** | 事件跨代复用：staging 在 poll(N+1) 前 re-signal **同一批**事件，必须先 reset。290×L0 append（~350µs）+1×HostSynchronize（~230µs） | **实现未动**（语义承重：不能挪 poll，除非事件池设计改双池） | **有**：双池事件设计（parity 双池，re-signal 用另一池事件，reset 与设备执行重叠），安全路径已论证，预期 ITL ~38.1 |
| pollread（drain 内读解析） | 70 | 71 | CompletePollRead/Collect+Execute 链 | 未动 | 小（71µs 非杠杆） |
| clone（submissions 克隆） | 80 | 80 | global submissions 容器克隆（单提交线程，outer_n=1） | 未动 | 小 |
| evcollect | 33 | 33 | 事件收集 | 未动 | 小 |
| prepk 溢出（PrepareToAppendKernelCommand 溢出到步隙部分） | ~150-200 | ~150-200 | 稳态 617µs/步大部分隐藏，溢出部分暴露 | 未动 | 中（重尾分布，多数步隐藏；无现成杠杆） |

- **Perfetto 可见性**：✅ 果可见、因不可见——人能看到 *gexp 切片比裸跑长* 和 *步间间隙*，
  但**看不出这 1ms 花在哪**（原本只是一根"莫名长的 gexp 柱子"）。
  **因**靠 meta 插桩暴露：`unitrace.enqueue`（ph=R instant，args 含
  `drain_us/d_loop_us/d_resets_us/d_pollread_us/d_flush1_us/d_flush2_us/d_iter_n/...` 全套）
  + `unitrace.reset_batch`（ph=X 真 slice，dur≈605µs, n=290）。**build5-8 打点战役的目的
  就是把这类税从"不可归因"变"可归因"。**
- **备注**：flush2 的"因"在被消灭前同样不可见（果=gexp +261µs，因=等 completer 唤醒），
  是第 4 类"不可见税"混进第 2 类的实例。

## 第 3 类：Host 隐藏税（trace 里有切片，但不延长 ITL）

| 子项 | 数值 | 成因 | 实现 | Perfetto 可见性 | 备注 |
|---|---|---|---|---|---|
| emit（chrome 记录构造） | 4.73ms/步，**100% 隐藏** | 每条命令构造 chrome 记录（GetZeKernelCommandName 等） | DEFERRED_TS 后在 completer 线程执行，与设备执行重叠 | ✅ 可见（app/completer 线程 cpu_op，与 GPU 轨道重叠） | **陷阱：可见≠是税**。曾误判"全暴露"，需重叠感知分类（间隙桥接成步级设备忙窗）才纠正。O2 名 cache/O1 async emit 实测 ITL 中性（成本全在此隐藏相位）——O2 cache 保留无害，O1 保险项 |
| qkt 批量读 append+sync @poll | ~372µs/步，隐藏 | 批量读命令 append 到 immediate list + sync | BATCHQKT 设计使然（append 在 poll 相位，与设备执行重叠） | ✅ 可见（poll 期切片） | 全场 poll 读总开销仅 143ms，v4b 臂证明非税源 |
| completer 线程工作量 | ~800µs/步，隐藏 | DEFERRED_TS 挪过去的 eager 命令 query+read+emit | 独立线程，与 ~38ms 设备执行重叠 | ✅ 可见（trace 多一条 completer 线程轨道） | 重叠设计的代价，不是税 |

## 第 4 类：真正不可见的税（Perfetto 无任何体现）

| 子项 | 本 workload 实测 | 成因 / 为什么不可见 | 剩余空间 |
|---|---|---|---|
| 锁等待 | d_lock≈0 | 单提交线程；多线程场景会藏在**持锁者**切片里，人眼无法归因（只能外部打点） | 若未来多线程 workload 出现锁税，需新增打点 |
| 原子计数器/UniPhaseTimer 自身开销 | ≈0（四代打点臂 ITL 全 39.1x 不动） | relaxed atomic + RAII，ns 级 | 无 |
| completer 唤醒延迟 | 原 261µs（=flush2），已消灭 | 藏在屏障等待里，无独立切片 | 已处理 |
| XPTI 分发/拦截固定开销 | 摊在每条 API 切片内部 | 无独立切片，不可分离 | 属 u0 基线税的一部分（+5.97ms/步口径） |

## 第 5 类：一次性/周期性尖刺

| 子项 | 数值 | Perfetto 可见性 | 状态 |
|---|---|---|---|
| capture 期 prepk 尖刺 | 283ms ×1（step 0） | ✅ 超长切片一眼可见 | 已定性（capture 期一次性） |
| 周期性 prepk 尖刺 | ~16.7ms @ 每 ~99 步（99/198/297） | ✅ 周期尖刺可见 | **归因未定案**（疑似事件池补充/logger flush）——战役遗留，若要再压 p99 需先查这个 |

---

## 汇总表（速查）

| 类 | 量级 | 优化前→后 | Perfetto 可见？ | 主要杠杆 |
|---|---|---|---|---|
| 1 设备拉长 | 4.25ms/步 | 已被 BATCHQKT 回收（不可逆） | ✅ GPU 轨道肉眼可见 | BATCHQKT（已完成） |
| 2 暴露（住 gexp 切片内） | 1.72→0.99ms/步 | -0.73ms | ✅ 果可见（gexp 变长/步隙）；因靠 unitrace.enqueue/reset_batch meta 记录 | DEFERRED_TS + build8（已完成）；**双池事件设计（reset 557，预期 ~38.1）= 唯一剩余大杠杆** |
| 3 隐藏 | emit 4.73ms/步等 | 重叠设计，不进 ITL | ✅ 有切片但与设备重叠（易误判） | 无需（O1/O2 已证中性） |
| 4 不可见 | ≈0（本 workload） | — | ❌ 完全不可见 | 无 |
| 5 尖刺 | 283ms×1 + 16.7ms×4 | 未动 | ✅ 尖刺可见 | 周期尖刺归因未定案 |

## 关键结论（一句话版）

1. **5 种税**；第 1/2/3/5 类在 Perfetto 里**有形可见**（但可见≠可归因），第 4 类完全不可见。
2. 只有第 2 类直接对应 ITL 指标；其"因"必须靠 meta 记录（`unitrace.enqueue` 全套 d_* 字段）
   才能读出——裸眼只能看到"gexp 比裸跑长 1ms"。
3. 第 3 类是"看得见但不是税"的重叠工作，逐 kernel 窗或逐 gexp 窗分类都会给假象，
   必须用间隙桥接成步级设备忙窗做重叠感知分类。
4. 出货配置下残余暴露税 0.99ms/步里，**reset batch 557µs 占过半**，唯一值得做的下一步
   =双池事件设计（预期 ~38.1，安全路径已论证，未实施）。

## 证据锚点

- 税梯与各臂 ITL：`BQ_TAX_HOTSPOT_AND_OPT.md` §6-§7.6；node3 `bq_results/{bqu1,bqc1,bqj1,bql1,bql2}`
- 暴露预算闭合表：同文件 §7.4（resets 578/flush2 261/pollread 76/loop 36 + clone 113 ≈ 1.23ms ✓）
- build8 代码锚点：ze_collector.h 0469e260，`ProcessAllCommandsSubmitted(..., bool at_gexp)`，
  `if (!at_gexp) { FlushDeferredTimestamps(); }` 出口屏障门控
- meta 记录形态（本报告 Perfetto 列的依据，bql1_trace.json 实测）：
  `unitrace.enqueue`=ph **R** instant 带 args；`unitrace.reset_batch`/`qkt_batch`/`emit`/
  `handoff`/`final_flush`=ph **X** 真 slice（有 dur）；`unitrace.step N`=ph R
- 隐藏/暴露判别方法论：§5（间隙桥接 + 重叠感知分类），u0/old-lib 双判别实验
