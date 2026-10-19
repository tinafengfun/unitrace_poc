# BQ-S2 设计文档：影子完成事件 W（"双池"战役的设计定稿稿）

> 日期：2026-10-19。状态：**实施完成 + 双机验证全绿 + rep3 收口（§10.1-10.3）：
> node3 正式 ×2 PASS（税 +1.4%）；node1 三 rep 税 +2.9~3.8%（均优于 rg1 +4.0%），A1
> census 逐位同、meta 差额 404 全对账为结构性；s2ar2 A5 三条 startup 区 unexplained 经
> rep3（A5==0）定性确认=首图提交区一次性抖动，停止项关闭；t8 CHECK7 已 W-aware 化并
> 四 trace 验证；§8 残差偏差结案。commit #5 冻结待 diff review。**
> 评审历史：有条件批准（R7 + POC(e) 补强已并入）→ POC ALL-PASS。
> 源基线：GitHub `78c7c0e`（commit #4，tsfix 已是基线），本战役 diff 在其上做。
> 源头依据：`BQ_TAX_LEDGER_PERFETTO_VISIBILITY.md` line 42（"双池事件设计…reset 与设备执行
> 重叠…预期 ITL ~38.1"）+ `BQ_TAX_HOTSPOT_AND_OPT.md` §O4。代码锚点全部为 78c7c0e
> 时点 `tslogfix_src/src/levelzero/ze_collector.h`（md5 9382952f）与 `ze_event_cache.h`
> 本地实读，非记忆引用。
> 目标机数据锚：node3 出货配置 build8+DEFERRED_TS ITL 38.43/38.43（税 +2.64%）；
> node1 rg1 基线 rg1a 37.92/rg1ar2 37.90（税 +4.06%/+4.00%）。

---

## 0. 目标与诚实定界

| 项 | 承诺 | 不承诺 |
|---|---|---|
| reset batch（d_resets） | 557µs/步 → ~0（消除，非隐藏） | — |
| node3 ITL | 38.43 → **37.9±0.1**（ledger 保守口径 ~38.1；两者均 PASS 规格 ≤38.57） | 不承诺 <37.9 |
| node1 ITL | rg1 37.92/37.90 → **~37.4-37.5**（税 +4.0% → ~+2.7%） | — |
| BQ poll P99 尾 | **观察项**：rg1a 证据（13 个尾步 39-44ms wall，drain_us 542-823 vs 正常 519-581，仅 +50-250µs）表明**尾步主要不是 reset batch**（reset p50 314µs/p99 618/max 2668@step356 与尾步不成比例）。预注册为"归因 pass from existing traces"，不承诺尾变好 | 尾步剩余部分（prepk 溢出、16.7ms 周期尖刺）不在本战役 |
| qkt_append 驱动侧 6.25ms/步 | **明确排除（驱动域）**。设计中如发现绕开路径，单独提报，不自行扩战 | — |

---

## 1. 现状机制（代码实读锚点，78c7c0e）

1. **事件烤入**：capture 期 app 传 `signal_event==nullptr` 时，collector 用自己的事件替换
   （`PrepareToAppendKernelCommand` :7150 `signal_event = event_cache_.GetEvent(context)`），
   该事件被烤入被捕获的 graph（`StoreCommandInGraph` :8065 `graph_command_=true`）。
   **每一次 replay 重放 re-signal 同一批物理事件**（本 workload 307 个/步，`max_n=307`）。
2. **drain 批量读（v4b 路径）**：`BatchQktExecute` :5249-5336 — 把本步 clones 的事件一次性
   `zeCommandListAppendQueryKernelTimestamps` 到 collector imm list + `HostSynchronize(UINT64_MAX)`。
3. **poll 臂（v4c 路径）**：`BatchQktArmPollRead` :5338-5378 — 在设备忙的 poll sweep 把读命令
   append 到专用 poll list，**numWaitEvents == numEvents**（等的就是它读的那批事件），设备
   停车等全部事件 signal 后一次读出；drain 侧 `BatchQktCompletePollRead` :5503+ 只做
   sync+memcpy（健康时立即返回）。
4. **reset batch（本战役靶子）**：`BatchQktResetEvents` :5860-5999 — 在下一轮 poll/read 之前，
   把上一代 re-signal 过的同批事件 **307×`AppendEventReset` + 1×`HostSynchronize`** 打到
   collector imm/poll list 上。全部暴露在 `urEnqueueGraphExp` 切片内：
   350µs append + 230µs sync = **557µs/步**（node3 出货配置最大单项暴露税）。
5. **消费侧 reset 记账**：`EventHistoryConsumePending` :6415-6453 — 最后一个同代 clone 读完后
   把 reset 推迟（`EventHistoryDeferGraphEventReset`）进 pending 池，由 #4 批量执行。
6. **拦截点（W 注入点，已确认结构）**：
   - `OnEnterCommandListAppendGraphExp/Ext` → `StageGraphExecution` → `PrepareGraphExecution`
     （:9991-9995；在 **driver 的 graph append 之前** 执行：drain + HostSynchronize + clone 暂存）；
   - `OnExitCommandListAppendGraphExp/Ext` → `SubmitGraphExecution`（:9997-10002；在 driver 的
     graph append **成功之后** 执行；失败走 Revert）。
   - **关键确认**：capture 期的 graph 构建走 `Begin/EndGraphCapture` API（:9940-9962），
     **AppendGraph 拦截只在 replay 期触发** — 在这两个拦截点注入的 op 永远不会污染被捕获流。

---

## 2. 为什么字面 "parity 双池" 无法闭合（竞争分析，设计否决记录）

ledger 的字面 sketch 是 "re-signal 用另一池事件，reset 与设备执行重叠"。**此路不通**：

1. **烤入事件身份固定**：E 事件在 capture 期一次烤入 graph（§1.1），之后每一代 replay
   re-signal 的是**同一批物理事件**。事件身份不能逐代交替（除非重捕获——重捕获=推翻
   XPU-graph 的全部收益）。"另一池 re-signal" 在烤入约束下不可实现。
2. **跨队列竞争两类失败模式**（collector imm/poll list 与 app 命令队列**无顺序保证**）：
   - **(a) HANG**：reset 推迟到 gen N+1 signal 之后才落到设备 → 摧毁 fresh 包；同时
     armed read 的 wait 挂在永不再 signal 的旧状态上 → 停车永不返回。
   - **(b) 静默错代**（更危险）：部分 reset（如 signaled-gate 跳过）或 wait 提前通过 latched
     事件 → armed read 瞬时通过 → 捕获早于全部 kernel 完成 → **错代时间戳包，无 TSBAD
     报错、只是错误记录**。这正是 v4b 注释反复警告的 hazard 类（:4082-4202 设计史块）。
3. **"reset 与设备执行重叠" 的重叠窗口无保证**：把 reset append 到 poll list 的 read 之后，
   窗口 = gen N+1 结束 → gen N+2 signal（app staging 延迟 ~ms）。正常时 reset（~350µs 设备）
   落在窗口内，但调度抖动下无任何机制保证——mode (a)/(b) 退化为概率问题。
   **概率缓解 ≠ 消除，否决。**

---

## 3. 设计：影子完成事件 W（signal-only，跨队列竞争类**消除**而非缓解）

### 3.1 机制

- **每步（确切说每次成功的 graph replay 提交）一个 collector 持有的影子完成事件 W**，
  从新增的小型"影子完成池"弹出（与 1024 事件 timestamp 池分离，见 §4）。
- **注入点**（复用 §1.6 已确认的拦截结构）：
  - `OnEnter`：不变（drain + clone 暂存照旧）。
  - `OnExit`（driver append result==SUCCESS 后）：往**同一个 app command list** append
    **1×`zeCommandListAppendSignalEvent(list, W)`** — in-stream，紧随 graph 之后。
- **流序**：`[...app 既有 ops][graph N+1 kernels][signal W]` → **W 恰在 gen N+1 全部完成时
  signal**。graph 与 signal 同一列表、流序保证——W 的 signal 与 graph 完成之间**没有跨队列
  竞争**。
- **armed read wait 集改变**：`BatchQktArmPollRead` 的 wait 从 `{307×E}` 改为 **`{本步全部 W}`**
  （单图步 = 1 个 W；多图步 = k 个 W，`numWaitEvents=k`）。读的 query 集不变（E 的包）。
- **E 事件的 reset 从批量路径整体消除**。latched E 无害，论证：
  1. 无人再 wait E（armed read 只等 W）；
  2. QKT 批量读 latched 事件返回**最新一代的包**（每次 signal 覆写包内容）——正确代；
  3. v4b drain-read fallback 在 `HostSynchronize` 之后读 = gen N+1 全部完成后的包 = 正确代
     （该安全性证明已是 :4082-4202 现有结论）；
  4. W 自身保证"读不早于本代完成"，E 的 latch 状态与正确性**解耦**。
- **下一代无需等待 E reset**：E 从"必须 reset 才能复用"变为"永不 reset 的只写包槽"。
  reset batch 557µs → **~0**。

### 3.2 W 的生命周期与 unlatch（这是设计的关键安全点）

- **pop**：OnExit、driver append 成功后从 W 池弹出一个 **保证 un-signaled** 的 W。
  **R7 补强（评审定稿）**：pop 时执行一次 `zeEventQueryStatus(W)`——返回 SIGNALLED 即违反
  不变量（在池 W 被陈旧 re-launch 的 list 再 signal 过，见 §9 R7）→ one-shot [WARNING]
  + 计数 + 该 W **host-reset 后归还池尾弃用**、另取下一个；池空 → 本步降级 v4b。
  不变量由此**每次 pop 自愈**，从"信任 list 不 re-launch"变为"每次 pop 一次状态读"
  （µs 级，每步一次，可接受——相对消除的 557µs 可忽略；QueryStatus 是 host 侧查询，
  无设备往返）。
- **release**：本步批量读完成时（`BatchQktCompletePollRead` 成功路径、以及**所有** fallback/
  失败路径的步末对账点）`zeEventHostReset(W)` 后归还 W 池。**host reset 在归还前执行** →
  池中任何 W 在 pop 时必然 un-signaled。
- **为什么 host-reset-at-release 而不是 in-stream reset-W**（设计内二选一的否决记录）：
  in-stream `AppendEventReset(W)`（OnEnter、graph 前）依赖"app list 队列头的 reset 在 armed
  read 的 wait 求值之前执行完"——该论证成立（drain N+1 的 HostSynchronize :8499-8501 已证明
  gen N 完成、设备空闲，提交后 µs 级 reset 即执行；arm 在 submit 之后的 poll sweep 才 enqueue），
  但它是**时序论证**；host-reset-at-release 是**状态论证**（pop 时必然 unlatched，wait 永远
  不可能提前通过）。取状态论证——更简单、可静态证明。代价 = 每步 1 次 host reset（µs 级），
  相比消除的 557µs 可忽略。
- **每步 W 引入的全部暴露成本**：1×AppendSignalEvent append（µs）+ 1×host reset（µs）
  + 池 pop/push（无锁 freelist，ns）。合计 ~几 µs/步。

### 3.3 多图步与并发 replay

- 本 workload 每步 1 图（batches=396/396、max_n=307=单图 kernel 数）。多图步：每图各自的
  W（每次 replay 提交一个），armed read wait 集合 = 本步 staged 的全部 W，形状不变。
- 同一 graph 从两个 list 并发 replay：每次提交独立 pop W，天然支持。
- `uni_batchqkt_read_pending_` 单 armed-read 槽守卫**原样保留**（同一时刻至多一个在飞读）。

### 3.4 门控与降级（每一条失败都落到今天的精确路径）

| 情形 | 行为 |
|---|---|
| `UNITRACE_GRAPH_BATCH_QKT=0` 或 QKT_AT_POLL 关 | **零 W 代码路径**（不 pop、不 append、不建池）——legacy 行为逐字节不变 |
| capture 期 | AppendGraph 拦截不触发（§1.6），天然无注入；W 池不参与 capture |
| W pop 失败（池耗尽/分配失败） | one-shot [WARNING] + 本步走 v4b 路径（含 reset batch）——优雅降级 |
| signal-W append 失败 / driver append 失败 | 不 append 或 Revert（现有 OnExit 语义）；W 立即 host-reset 归还；本步 v4b 路径 |
| armed read 未建立 / CompletePollRead 失败 | 现有 fallback：v4b collect+execute（"armed read 从不碰 clone 状态"安全性现成）；W 仍按时 host-reset 归还 |
| graph append 成功但 list 从未提交 | 与今天同构（E 也不 signal）：无 arm、无读、W 在步末对账点 host-reset 归还 |
| **pop 时 W 已 latched（R7：在池 W 被陈旧 re-launch 再 signal）** | pop 后 `QueryStatus(W)==SIGNALLED` → one-shot [WARNING] + host-reset 后弃用归还、另取；池空 → 本步 v4b 降级（不变量每次 pop 自愈） |
| 步末对账点遗漏防御 | W 借出计数器（per-context）+ 每步对账：借出>0 未归还 → one-shot [WARNING] + 补偿归还；teardown/ctx-destroy 强制清点（V3 模式） |

---

## 4. 事件池生命周期变更

- **新增：影子完成池**（per-context，惰性创建；容量 = 活跃并发 replay 提交数上界，默认
  小池 256，池空时按需扩容同 timestamp 池模式；pool flags 与 timestamp 池同
  `HOST_VISIBLE | flags_`，event scope `ZE_EVENT_SCOPE_FLAG_HOST`——**POC 验证项 (c)**）。
- **不动**：1024 事件 timestamp 池（EVENT_POOL_SIZE）、`ReleaseEvent` 的 L0 V2 workaround
  （destroy+recreate 同 index）、`ReleaseContext` 强制释放语义。
- **W 池纳入 ctx 生命周期清理**：`ReleaseBatchQktContextResources`（zeContextDestroy ENTER，
  context 存活时配对释放——V2 加固的同款形状）扩展覆盖 W 池 + 在飞 W；exit 侧安全网
  `InvalidateBatchQktContext` 同步扩展（只标死不碰设备）。
- **graph 对象析构路径**：W 与 graph 对象无所有权绑定（per-提交，非 per-graph），graph 销毁
  无需动作——这是 per-提交 W 相对 per-graph W 的一个额外优点。

---

## 5. 三铁律落实（V2/V3 同款标准）

1. **配对**：W 的 pop/release、pool create/destroy、每 ctx 借出计数器与对账，全部显式配对；
   失败路径逐条清点（§3.4 表格即配对清单）。
2. **异常**：所有新增 ZE 调用检查 status；失败 → one-shot [WARNING]（`UNITRACE_DEBUG_BATCHQKT`
   门控）+ 优雅降级到现有路径；**绝不 abort 被插桩应用**（与 GetEvent 契约同款：调用点全守卫）。
3. **无 PTI_ASSERT**：运行期不变量（借出计数、pending 空池、包代一致性）用计数+降级；
   assert 仅保留内部不变量（如 descriptor 同步性——与现有 :5526 同款）。

---

## 6. EventState 机具审计（最大实现风险，逐消费者设计答案）

E 事件"永不 reset"会改变**共享同一批事件的其它状态机具**的行为。已枚举全部消费者并给出答案：

| 消费者 | 锚点 | 设计答案 |
|---|---|---|
| armed read wait | :5338-5378 | wait 集改 {W}（本设计核心） |
| v4b drain read | :5249-5336 | 不变；读后 E 不 reset（包=正确代，§3.1 论证 3） |
| reset batch 本体 | :5860-5999 | 批量路径不再调用（代码保留，仅 fallback 步行使）；meta 记录 396/步→~0 |
| pending-reset 推迟 | :6415-6453 `EventHistoryDeferGraphEventReset` | 批量路径不再 defer（pending 池恒空→无泄漏）；fallback 步照旧 defer+执行 |
| stale-latch 清除环 | :8514-8519 | **保留**：它清的是"无 pending 消费者"的 latched 事件——批量路径下 E 恒有 pending 记账吗？**实施时逐条核对**：改为批量拥有的 E 跳过（其 latch 无害），避免每步 307 次无谓 QueryStatus |
| EventHistory 代计数 | :8571 `EventHistoryBumpSignal` path=1 | **保留**（每代 bump，快照=消费代检查依据）；仅 pending-reset 分支短路 |
| legacy fallback 消费 | :6415-6453 非 graph 分支 | 不变（eager 事件仍走完整 legacy reset 路径——eager 事件与 graph E 共池但身份不同） |
| Fix B 入口屏障 | PrepareGraphExecution | 不变（barrier 等的是 list 完成，非 E 状态） |
| `ReleaseContext` | ze_event_cache.h | 不变（强制释放语义与 latch 状态无关） |
| **command list re-launch 路径（R7 审计行，2026-10-19 评审补强）** | — | **审计项**：确认 vLLM/NEO 无同一 command list 在 W 归还入池后被 re-launch 的路径（证据锚：batches=396/396 = 每步恰好一次 AppendGraphExp）。**若审计发现 re-launch 存在 → 停下补设计，不得带病实施**；pop 时 QueryStatus 门（§3.2）为其运行期防线 |

**实施前必须以清单形式再过一遍全部 `QueryStatus`/`ResetEvent`/`HostReset` 调用点**（审计项，
实现 PR 的第一步骤），任何未列出的消费者出现 → 停下补设计。

### 6.1 全调用点审计清单（2026-10-19 实施第一步，已逐条核对 78c7c0e 实码；~60 个调用点/8 簇）

实现决策先记两条：**(a) W 采用 collector 侧窗口槽实现（`uni_batchqkt_pending_ws_`，
`{context, W}` 对的 vector，`uni_batchqkt_mutex_` 保护），不用 clone 字段**——arm/drain/OnExit
全在提交线程的既有互斥点上，槽随 drain 清空实现与 pending clones 的同窗口 1:1 关联，revert
路径零记账负担；**(b) W 池直接复用 `ZeEventCache(flags=0)`**（池 flags=HOST_VISIBLE，
POC diag3 eP2 已证 signal+wait 可用），destroy+recreate 语义天然覆盖 R7（旧句柄已销毁，
陈旧 signal 打不中新对象），lifecycle（ReleaseContext/destroy pairing）全部继承 hardened 实现。

| # | 调用点簇 | 锚点（78c7c0e 实码） | W-active 下处置 |
|---|---|---|---|
| C1 | flush/finalize 路径 QueryStatus ×5 处 | :1978/:1985, :2148/:2156, :2234/:2241, :3169/:3176, :3313/:3320 | **不变**（teardown 时刻，读最新包 latch 安全） |
| C2 | metric query 事件族 | :2701-2728 | **不变**（独立事件独立生命周期，与 graph E 无共享） |
| C3 | EventHistory 机具本体 | :3431-3541 | **全保留**（BumpSignal/AddPending/ConsumePending/HasPending/MarkReset/Erase）；仅 `DeferGraphEventReset` 的两个调用点被 C5/C9 门控 |
| C4 | `ReleaseGraphCommandEvent`（丢弃路径：batch append 失败/query 失败 :6250/:6294） | :3827-3837 | **不变**（罕见失败路径；ConsumePending 记账保留，reset 时刻安全——query 已证当代完成） |
| C5 | `BatchQktReleaseArmedEvent`（arm 时刻释放，批量路径主 reset 源） | :5698-5719 | **门控**：`ShadowWActive && graph_command_` → 保留 MarkReset+ConsumePending，**跳过 defer/host-reset**（E 按设计保持 latched） |
| C6 | `ProcessCommandSubmitted` 消费尾（非 deferred_emit 路径） | :6419-6451 | **不变**——仅批量读未供 pre_ts 的深层 fallback 才到达；彼时代已被证完成，reset 安全且恢复卫生态 |
| C7 | MarkReset 两处 | :5702, :6420 | **保留**（TSLOG 记账；BQ 下 TSLOG2 强制关） |
| C8 | `BatchQktEventReady`（drain collect 就绪门，memoized QueryStatus） | :4813-4832 | **不变**（仅 v4b fallback 路径使用；latched E 恒 ready → collect 行为不变） |
| C9 | `BatchQktResetEvents`（parked 集 flush） | :5887-5999 | **代码不变；W 下 parked 集恒空 → 永不执行**（C5 门控后无人 defer）；保留给非 W 模式与 fallback |
| C10 | stale-latch 清除环 | :8514-8519 | **门控**：`ShadowWActive` → 跳过整环（E 按设计 latched，且清除会破坏 armed read 的 wait 语义）；省每步 307 次 QueryStatus |
| C11 | anomaly 路径 QueryStatus+QueryLatestTimestamp | :6254-6271 | **不变**（诊断专用，latch 安全） |
| C12 | ReleaseEvent 调用点（list/graph/app teardown，destroy+recreate） | :6555, :6588, :6629, :8244/:8253, :8683-9227 | **不变**（对 latched 事件 destroy+recreate 安全，且重建即 NOT_READY） |
| C13 | capture 烘焙 + path=0 bump | :7151/:7164/:7183 | **不变** |
| C14 | metric GetEvent ×5 | :7326-7911 | **不变** |
| C15 | app signal/wait 事件 intercepts | :6910/:6978/:7006 | **不变**（app 自有事件） |
| C16 | TSLOG 日志读取 | :3899/:3900/:3936/:3938 | **不变** |
| C17 | Fix B per-LIST 事件（timestamp_event_to_signal_） | :6497-6498, :6689 | **不变**（独立事件，eager 路径） |
| W1 | **新增** W 池 + pop 门（R7） | `shadow_w_cache_` (ZeEventCache flags=0) | pop 后 QueryStatus==SIGNALLED → ReleaseEvent（destroy+recreate）另取，≤2 次，计数+one-shot |
| W2 | **新增** W 注入 | OnExit graph submit → `BatchQktInjectShadowW(list)` | 仅 **in-order list**（`ZeCommandList::in_order_`，:1217/:6493）；乱序 list 注入会允许 signal 越过 graph（mode-b）→ 注入禁用+计数降级 |
| W3 | **新增** arm wait 集 | `BatchQktArmPollRead` :5446-5449 | wait 集 = `pending_ws_` 中同 context 的 W；**空 → 不 arm**（绝不在 W-active 下 wait 于 E 本身——E 可能仍 latched 上代，wait 立即返回会读脏包）→ v4b drain 读 |
| W4 | **新增** W 释放 | `BatchQktReleaseShadowWs(ctx, device_ok)` | CompletePollRead 全出口 + Execute 全出口 + ctx destroy（enter=释放/exit=abandon）+ Finalize backstop；幂等；sync 失败路径 device_ok=false（abandon 计数，不归池） |
| W5 | **新增** 降级注入钩子 | `UNITRACE_TEST_SHADOW_W_FAIL=1` | GetW 恒 nullptr → 每步 W 缺失 → arm 跳过 → v4b drain 读（验收条件 4 的实测入口） |

**re-launch 审计结论（R7 行）**：实码核对，graph 提交唯一入口 = `OnEnterCommandListAppendGraphExp`
（:9991，Exp/Ext 各一），无 list re-launch 路径；batches=396/396（每步恰一次 AppendGraphExp）
与实码一致。**不触发停下条件。**

**饥饿约束落实（批准条件 2）**：W 注入点在 OnExit（graph 已 append 成功后）、arm 在其后
的 device-busy poll sweep、resolve 在下次 OnEnter 的 drain（其 HostSynchronize 先行）——
与生产现状的 park 窗口逐点一致；arm 的 W 空集早退（W3）保证不存在"无 W 裸 park"形态。

---

## 7. POC 结果（2026-10-19，node3 device0 = Intel(R) Graphics [0xe223]/BMG-G31，loader 1.26.2 / NEO 1.13.35563；collector 未动刀）

**POC ALL-PASS（两轮复跑一致）**，产物：`build11/poc_bqs2/`（bq_s2_poc.c md5 e241ef30、
kspin.cl、kspin_bmg.spv、bq_s2_poc_run.log md5 ba8664b8；node3 侧 /tmp/bqs2/ 同物）。

| 项 | 结果 | 证据 |
|---|---|---|
| (a) numWaitEvents=1 + numEvents=307 | **PASS** | spec 层独立参数（ze_api.h :4630-4646 仅约束 phWaitEvents↔numWaitEvents 配对）；实测 3 代 append 全 SUCCESS，读行为正确 |
| (b) in-stream AppendSignalEvent(W) 在 app imm list | **PASS** | 3 代 signal 全 SUCCESS；park 证明 + sync 在 W signal 后解析 → 流序正确 |
| (c) W 事件能力组合 + 独立影子池 | **PASS** | 同池 W（index 400）与独立 4-slot 池 W2（gen3）均 device-signal+host-reset+device-wait 正常 |
| park 证明 | **PASS**（3/3 代） | gen2/gen3 在 **E 已 latched（mode-b 状态）** 下读仍停车等 W → wait-on-W 不被 latched E 破坏（设计核心安全性质直接实证） |
| (e) re-signal-without-reset 覆写语义 | **PASS**（独立判据） | 307/307 包 gen2≠gen1 且 307/307 严格递增（same=0, not_greater=0） |
| R7 陈旧 re-launch 隐患 | **实锤为真** + 门恢复有效 | hostReset 归还后陈旧 list 再 signal → QueryStatus=SIGNALLED（hazard REAL）；再 hostReset → NOT_READY（pop-gate 自愈动作有效） |

**POC 过程中的驱动行为发现（写入设计约束，重要）**：
1. **停车读饿死后继提交**：本驱动上，设备侧停车 wait（armed read）会饿死**之后**提交到
   其它队列的同 ordinal 队列 op（diag4 B1：armed read 先挂 → regular queue 的 [signal W]
   永不执行；diag5 B4：imm_app 后续 op 同样被饿）。且一次卡死的读会**毒化后续一切提交**
   （diag 早期 C/D/E 的"kernel+signal 挂死"实为 B 的卡死读饿死所致，非该原语本身坏——
   diag3 无卡死读时同形状全通）。
   → **设计约束**：armed read 必须保持生产不变量——**只在图已提交、引擎忙时 arm**，
   且在**下一次 app 提交之前**（= 现有 drain 在 OnEnter 的次序）resolve。W 设计不改变
   park 窗口（与今天 wait-on-E 的 park 窗口一致），不变量保持。实现评审时按此约束检查。
2. kernel+signal-event 直接 append 在健康状态下工作正常（H/L），poll 专用 imm list 与
   app imm list 双列表形状工作正常（= 生产形状）。
3. node3 dmesg 受限（`dmesg` 不可读），内核日志走 `journalctl -k`；POC 全程 fault/ERROR
   增量 = 0。
4. POC 遗留 printf 静态缓冲二次调用瑕疵已修（r2s 双调用点改局部拷贝），复跑 PASS。

---

## 8. 预注册验收 delta（对 rg1 锚，防误报/防漏报）

| 量 | rg1 基线 | BQ-S2 预期 | 判据 |
|---|---|---|---|
| `unitrace.reset_batch` 记录 | 396/步（p50 314µs p99 618 max 2668） | **~0**（仅 fallback 步保留） | 硬判据：批量路径稳态=0 |
| `unitrace.enqueue` d_resets_us | 557 | **~0-5** | 硬判据 |
| 新增 meta 记录 | — | **不加**（W 不产生新记录；观测量复用现有 d_* 字段） | census 不因 meta 增项 |
| op census ndev/nhost/g1_ops | 138696/194754/121572 | **不变**（W 无 chrome op） | 等同判据 |
| nesting | 1581/1583 | **不变** | 等同判据 |
| TSBAD / footer / dmesg 三段 / A5 unexplained | 0/1/0/0 | **全部维持** | 硬判据，任一破→停 |
| node3 ITL | 38.43/38.43 | 37.9±0.1 | 规格 ≤38.57 必达；<37.9 记录不承诺 |
| node1 trace-ITL vs bare | +4.64%/+4.46% | **~+2.7-3.0%** | 不得劣于 rg1 |
| BQ poll P99 尾 | 13 尾步 39-44ms | 观察项（归因 pass，非承诺） | 仅记录 |

---

## 9. 风险登记与 fallback 链

| 风险 | 概率 | 缓解/降级 |
|---|---|---|
| R1 (a) 不合法（wait 数≠query 数被拒） | 低 | 首选仍成立需 (a)；若被拒 → 报告并停（**不自行切 O4 路线**——那是概率缓解，需单独评审） |
| R2 (b) 拦截 append 被驱动拒 | 低 | signal append 失败→本步 v4b 路径（§3.4）；若普遍失败→战役停，报告 |
| R3 W latch 竞争 | 极低（状态论证已闭合） | armed read 失败/超时→drain read fallback（现有机制） |
| R4 EventState 机具漏审计 | 中（工作量风险非设计风险） | §6 清单制 + 未列出消费者→停；实施 PR 第一步=全调用点审计 |
| R5 W 池泄漏/双借 | 低 | 借出计数器+每步对账+teardown 强制清点（V3 模式 one-shot 日志） |
| R6 行为变差（任一回归线） | — | **rg1 逐项对账，任一变差即停**（协调者纪律） |
| R7 在池 W 被陈旧 re-launch 的 list 再 signal → pop 拿到 latched W → armed read 瞬时通过（= §2 模式 b） | 低（vLLM 每步一次 append 的证据锚） | **运行期防线**：pop 后 QueryStatus 门（§3.2，每次 pop 自愈不变量）；**审计防线**：§6 re-launch 路径审计行，发现即停。**POC 已实锤 hazard 为真且 hostReset 恢复有效** |
| R8 停车读饿死后继提交（POC 驱动行为发现） | 低（生产不变量下不触发） | 设计约束 = 保持生产 park 窗口不变量（§7）；实现评审按约束检查 |

全链失败 = 今天精确路径（fallback 无新增风险路径）；W 代码整体门控在 BATCHQKT 开关下，
关闭即逐字节回到 78c7c0e 行为。

---

## 10. 双机验证计划（实现批准后执行）

1. **node3 先**（环境干净，税基线 +2.64%/38.43）：POC 四项（§7）→ build → bare 锚 ×1 →
   双 smoke → 正式臂 ×2（预注册 §8）→ 与 build8 出货配置逐项对账。
2. **node1 复验**（rg1 基线）：全套 `.claude/skills/unitrace-human-acceptance`
   （裸臂先锚 → 双 smoke → 正式臂 ×2 → t14 无旁路），与 rg1 逐项对账，任一变差即停。
3. 规则照旧：max 2 tries/arm、gate、dmesg 三段熔断、共租让路、session ≤8 字符、raw 全留
   （copy-never-delete）、新脚本 README 索引。

## 12. 实施产物与脚本索引（新脚本 README 索引，铁律 5）

**diff（commit #5 候选，冻结待裁决）**
- `build11/ze_collector.78c7c0e_to_bqs2.diff`（+435/−11，基线 md5 9382952f → 实施后 fbee9b26）
- `build11/ze_event_cache.78c7c0e_to_bqs2.diff`（+ReleaseAll；20478073 → ab293b3c）

**实施源树**
- node1：`/mnt/disk3/home/tfeng/unitrace/src/unitrace_src/tools/unitrace/src/levelzero/`
  （W 版现役；原基线备份 `*.78c7c0e_bak` 同目录）
- 本地：`build11/tslogfix_src/src/levelzero/`（同 md5 双写）
- node1 build 产物：`build11r3/bin/` lib `5e3a243f` + launcher `50a3810f`（pins.env）

**node3 脚本（本地 `scripts/bq_node3_stage/`，已同步 node3 `/home/intel/v4c_stage/bq_staging/scripts/`）**
- `launch_ws_bq.sh` — BQ-S2 臂启动（=launch_v4c_bq.sh 换 bqs2 staging 路径 + LD_LIBRARY_PATH
  XPTI-LIB-FIX 行；**该行必须带**，见踩坑①）
- `runner_bqs2_node3.sh` — BQ-S2 验证梯 runner（=runner_bq_node3.sh 换 pins + W 臂
  bare/wssmoke/ws/wsinject + W-aware 判定 BQ-W/BQ-WRESET/BQ-WQ/BQ-WDEGRADE；用法
  `bash runner_bqs2_node3.sh <ARM> <REPS>`）

**node3 staging（copy-never-delete，新目录）**
- `/home/intel/unitrace_sdk_bqs2/tools/unitrace/build/`：lib `5e3a243f`（node1 build）+
  launcher `50a3810f` + `libunitrace_mpi.so`（v4c 树原件）+ `libxptifw.so.1`
  `df0c2e82`（build3/xpti_fix 原件，**勿用 build11 的 `.1.0.1` 拷贝**，见踩坑①）

**node3 验证记录（2026-10-19，`/home/intel/v4c_stage/bq_results/`）**
- bare 锚 bqa1r2：ITL 37.45（ref 追加；bqa1 try1 引擎初始化瞬态失败 "Mamba cache blocks
  127<128"，try2 2.86GiB 正常——与 unitrace 无关，见踩坑②）
- wssmoke ×2（bww1/bww1r2）：exact=1 W=1，ITL 37.58；判定 FAIL 仅为旧版严格 WQ 判据
  （reset_batches==0），判据已按 capture 代残差放宽（BQ-WRESET 单列）
- 正式臂 ×2（bwq1/bwq2）：**PASS** ITL 37.99/37.97（§8 预注册 37.9±0.1 ✓；
  裸基线 37.44 → +1.47%/+1.42%）；w_pops=w_released=396=submissions，
  w_latched/w_inject_fail/w_degrade/w_abandoned=0；qkt_at_poll==cmds==114840，
  qkt_at_drain=0；reset_batches=1（capture 代一次性 672-683µs，§8 偏差见 §13）
- 降级路径实测（bwi1，条件 4）：`UNITRACE_TEST_SHADOW_W_FAIL=1` → w_inject_fail=38、
  w_degrade=249、w_pops=0、**exact=1**（v4b drain read 全量采集）、qkt_at_poll=0、
  qkt_at_drain=10730 —— 降级梯真实走通
- dmesg 三段：bwq1/bwq2/bwi1 增量 0 行异常

## 10.2 node1 验收记录（2026-10-19 20:46–21:33，runner_rg1s2_node1.sh，window 同晚）

- **s2bare PASS ×2**：ITL 36.43/36.48（rg1 锚 36.41/36.44/36.41 ✓）；t14 refs 取本窗锚
  36.48/36.34/38.89。
- **s2smk PASS**：meta=232（≥20）、footer=1、dmesg-bad=0、glued=0；W 形态健康
  （w_pops=32=w_released=32，fault 计数全 0）。
- **s2sm0 PASS**：meta=0；census 判据 op 计数等同（cpu_op 5620==5620、gpu_op 2846==2846）。
- **正式臂 ×2 PASS**（s2a/s2ar2）：ITL 37.84/37.88（bench），trace-ITL +4.34%/+4.44%
  vs rg1 +4.64%/+4.46% —— **持平或更好**；bench 税 +3.7%/+3.8% vs rg1 +4.06%/+4.00%。
  W 形态完美：w_pops=w_released=396=submissions，w_latched/w_inject_fail/w_degrade/
  w_abandoned=0；qkt_at_poll==cmds==121572、qkt_at_drain=0、flush_at_drain=0；
  reset_batches=1（capture 代残差 551/624µs 一次性，§13.1）。
- **t14 无旁路收口**（refs=本窗裸锚）：
  - A1 op census **逐位同 rg1a**（cpu_op 194754、gpu_op 138696、replay 121572、dom 38016）。
  - M1 meta census：3220/3241 vs rg1 3624 —— **差额 404 全部对账为结构性预期**：
    reset_batch 396→1（−395，每步 reset 消除的直接体现）+ handoff 852→843/864（−9/−12，
    drain-read 路径收缩）；nesting 1581/1583→1186/1188（−395 = reset_batch 嵌套消失）；
    嵌套例外身份与 rg1 相同（qkt_batch+reset_batch 各 1 例）。
  - T8：s2a 5/7、s2ar2 4/7 vs rg1 6/7、5/7 —— 差额**恰为 CHECK7 一项**：t8 分析器按
    v4b 形态预期 reset_batches=396，W 模式为 1（结构性 MISMATCH），而 7a-7d 真完整性
    子项全 PASS（leakage=0、stale-generation=0、raw causality=0、138696 1:1 drift 0.25µs）。
    CHECK1（税本身）/CHECK3（eager memcpy 重叠）与 rg1 同判。
  - **A5：s2a 1082/1082 unexplained=0（PASS）；s2ar2 912/915 unexplained=3（字面变差 →
    按纪律停止并上报）**。三条定位（/tmp/locate2.py 留档 node1）：submit 20.6ms（0% busy）、
    urEnqueueGraphExp 27.7ms（13.4% busy）、zeCommandListAppendGraphExt 21.6ms（0% busy），
    **同一嵌套链、同一时刻，全部位于 step 1 之前 ~2-22ms 的首次 graph 提交区**（ts
    1792415940091891-1792415940121945，step1 ts=1792415940119586）；396 步稳态区 0 条。
    同窗同二进制的 s2a 在同一相对位置同链仅 15.8/13.3ms（低于 20ms A5 入群阈值，从未入群）。
    定性：首次提交的 host 侧一次性延迟离群（f1a 基线本就有 2 条同类 GraphExt unexplained；
    rg1 恰好 0），非稳态、非 W 机制缺陷；但按"任一变差即停"如实上报，是否补跑 rep3 由裁决。
- **dmesg 三段**：全部臂增量 0 异常行（monitor 卡隔离 WATCH_PCI=0000:1a:00.0；C6 噪声
  按既有归类不算熔断）。
- **踩坑⑤（node1 staging）**：bqs2_stage/bin 初建时缺 `lib/` 符号链接 → launcher 找
  `bin/../lib/libunitrace_mpi.so` 失败，s2smk 26 秒死于 "[ERROR] Library .../lib/
  libunitrace_mpi.so cannot be found or opened"。修复 = `ln -sfn ../build11r3/lib
  bqs2_stage/lib`（tsfix_stage 同款布局；build11r3/lib 内含 libunitrace_mpi.so +
  libxptifw.so.1 df0c2e82）。**教训：node1 staging 目录必须同时带 bin/ 与 lib→ 符号链接，
  与 tsfix_stage 1:1。**（第一次 s2bare+s2smk 梯 20:25-20:35 因此作废，raw 保留。）

**结论：node1 验收 = 结构性各项全对账通过、税更好；唯 A5 rep2 三条 startup 区
unexplained 为字面变差停止项，如实上报待裁决（不自行补跑）。**

### 10.3 裁决执行（2026-10-19 21:38–22:0x，四项裁决落地）

1. **rep3（s2ar3，单正式臂 1 try）— A5==0，s2ar2 定性确认**：runner_s2ar3_node1.sh
   （runner_rg1s2_node1.sh 克隆，仅正式臂单 try；同窗锚 36.48/36.34/38.89 复用——gate 自
   20:46 连续未断、GPU clients=0、共租负载机制同前窗）。ITL 37.55（bench，税 +2.9%），
   trace-ITL +3.39%/+2.61%（三 rep 最好）；meta=3220、gexp=396、W 形态完美
   （w_pops=released=396、fault 全 0、qkt_at_poll==cmds==121572、drain=0）；
   **t14 A5 = 1137/1137 unexplained=0**；T8 6/7（仅 CHECK1 税本身）。首次提交链时长分布：
   s2a 15.8/13.3ms、s2ar3 16.5/14.0ms（同带，低于 20ms A5 入群阈值）、s2ar2 27.7/21.6ms
   （唯一离群）——**首图提交区一次性抖动实锤，A5 停止项关闭**。
   留档：`results/s2ar3/`（bench/t14 四件套/dmesg 三段）；dmesg-bad=4 = C6 idle-PM 噪声
   （fault 类 0；另见他卡 0000:45:00.0 共租 bind debug 行，非本卡）。
   harness bug 留档：run_s2ar3 单 try PASS 路径 `[ "$TRY" = "2" ] && return 0` 落空 →
   误报 "FAILED — STOP"（bench 实际 PASS，verdicts_rg1s2.txt 有 PASS 行）；已修（return 0）。
2. **t8 分析器 CHECK7 W-aware 化（裁决 3，已做）**：`scripts/t8_analyze_c2c.py`
   （备份 `.pre_ws2_bak`，新版 md5 7454049cfcf8e014c2ca56451c079d15）——reset 预期改双模：
   按.summary 自检 reset_batches<nsteps 判 shadow-W 模式（预期 reset_batches<=2、
   reset_batched<=2×per、reset_max<=per，capture 代残差）；否则 v4b 模式原预期不变；
   其余六检查未动；打印加 mode= 标注。四 trace 验证：rg1a 6/7、rg1ar2 5/7（v4b，ALL
   MATCH，**不变**）；s2a 6/7、s2ar2 5/7（shadow-W，ALL MATCH，**CHECK7 转 PASS**）。
   rg1/s2 t14 ws2 版报告另存 `A5_*_ACCEPT_ws2.md`/`a5_*_accept_ws2.json`（原件不动）。
3. **§8 偏差（capture 代残差 reset 批）结案**（裁决 4）：接受——每步税已 0；
   :3836/:6817 两站点记入改进项清单（下次动刀顺手），不专项处理。
4. commit #5 冻结不变，等 diff review（A5 停止项已关）。

## 13. §8 偏差登记（如实上报）

1. **reset_batch 记录非严格 0**：capture/warmup 代（~290 事件）仍走 pre-W 管线
   （ReleaseGraphCommandEvent :3836 与 drain 路径 :6817 未加 W 门——有意保留），在首个
   sweep 一次性批量 reset（683µs）。**每步 reset 税 = 0**（§8 目标达成：557µs/步×396 步 →
   一次性 683µs）；严格 0 需再 gating 两处 + 重建 + 全量复验，未做（风险/收益不成立，
   留给裁决）。
2. 其余 §8 项（TSBAD/footer/census/nesting/dmesg/降级）node3 侧全部符合；node1 侧
   s2ar2 A5 三条 unexplained 经 rep3 复核定性为首次提交一次性抖动（§10.3.1），
   **2026-10-19 裁决 4：本偏差（capture 代残差 reset 批）接受结案**。

### 踩坑速查（本次新增）
① **node3 staging 的 libxptifw.so.1 必须用 build3/xpti_fix 的 `df0c2e82`**；build11
  staging 的 `libxptifw.so.1.0.1`（0ae153e8）内含 phmap assert 代码，LD_LIBRARY_PATH
  以 bqs2 目录为先时 torch.xpu.init 即崩（`phmap.h:149 not a mask`），表象酷似
  "工具库 build 环境不兼容"，曾误判 node1 icx build 与 W 代码（三向 bisect 后定位：
  崩与不崩完全由 LD_LIBRARY_PATH 先序决定，与 lib md5 无关）。
② **bare 臂 "footer=0" 假 FAIL**：runner_bq_node3.sh 的 bare 分支 footer 计数查
  `python*.json`，bare 无 unitrace 永远 0（历史 PASS 都来自 phase 脚本）。bqs2 runner
  已改为 ut 臂才查 footer。
③ **node1 build（tfeng_build11r3, icx 2026.x）产物只用于 node1**；node3 验证链每代
  lib 都是 cri2（tf_unitrace_v4c2, icx 2026.3.0）build 的（本战役 BQ-S2 lib 亦为
  node1 build，node3 probe 现已验证可加载——前述"node1 build 不能上 node3"是踩坑①
  的误判，已撤回）。
④ vLLM "Mamba cache blocks 127<128" 为阈值边缘瞬态（KV 2.44GiB vs 2.86GiB 抖动），
  retry 即过，勿改 bench 参数（会作废 ITL ref）。
**字面"parity 双池"被烤入事件约束否决；改为每步一个 in-stream signal 的影子完成事件 W
（host-reset-at-release 保证 pop 时必然 unlatched——状态论证而非时序论证），armed read 等 W
不等 E，E 的 reset 从批量路径整体消除，跨队列竞争类被消除而非缓解；557µs/步 → ~0，
预期 node3 37.9±0.1、node1 ~+2.7% 税；四项 POC 先行，六消费者机具审计清单制，
rg1 逐项对账任一变差即停。**
