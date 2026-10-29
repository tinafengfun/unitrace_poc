# TSBAD 修复设计 — armed read W 等待集覆盖缺口（P0：event loss）

> 2026-10-29。状态：**Tina 已批准（v2 裁决版）**——B 主修 + A 粗粒度守卫开修复战役；
> SHADOW_W=0 不并入正式修复，作独立第三根 smoke 对照管。
> 根因定案与证据链见同目录 ACCEPT_RECORD.md §3（2026-10-29 修订版）。

## 0. 裁决（2026-10-29，Tina）

- 优先级：**正确恢复 W 生命周期闭环 > 修改量小 > 局部测试通过**。
- 主方案 **B**（根因本质 = "clone 仍待消费，但对应 W 已被 release"；B 把 release
  条件从"drain 结束"改为"submission 全部相关 clone 已完成或被明确终止"）。
- **A 只吸收粗粒度守卫**（fail-closed + 可观测），不单独做根治、不完整叠加。
- **SHADOW_W=0 不合并**：两套 W 语义并存会产生双重所有权（clone 引用哪个 W、
  谁是唯一 release owner、状态如何同步均无设计）；shadow 管可能托底掩盖 B 的
  缺陷，降低验收可信度。D2 = 独立第三根 smoke 管（基线管 / B+A 修复管 /
  SHADOW_W=0 对照管），不作为 B 的 pass 条件、暂不删除、暂不默认启用。
- 合并 shadow 的四条前置（全部满足才复议）：shadow 提供 B 无法覆盖的必要语义；
  证明它不是第二套所有权机制；明确唯一 release owner；B 单独失败且根因证明在
  wait 对象生命周期。若合并也须单 owner 结构（SubmissionState：original W +
  optional shadow payload + live clone count + single release owner）。
- 可选两步拆分（降合入风险）：第一阶段 A 守卫+counter 先拦静默丢 metric，
  第二阶段 B 闭环根治——本轮实现为**同轮两 commit**（A 守卫 commit 与 B 主修
  commit 分开，bisectable），一次构建一次 smoke。

## 1. 根因（完整链）

**健全性条件**：任何读点读一批 graph clone 的时间戳包时，必须保证批内每个 clone
的 kernel 已完成。旧栈 v4c 用"wait = 被读事件本身"（numWaitEvents == numEvents）
结构性地满足它；BQ-S2 W 模式把 wait 换成"窗口影子 W 集合"，健全性改为依赖
**窗口相关性不变量**：pending W 集合 == pending clone 集合所属 submission 的 W 集合。

**破坏链（6 步）**：

1. gexp k 的 OnExit 提交 graph 并 push W_k（`BatchQktInjectShadowW`，
   ze_collector.h.rebase:5969，pending_ws_ = {W_k}）。
2. replay k 在 device 上执行中；下一条 poll sweep arm 批读：批 = pending clone，
   wait = pending W（:5610）——此时健全。
3. gexp k+1 的 OnEnter（`PrepareGraphExecution`）跑 drain：host-sync 只覆盖
   **本 graph（G k+1）** 事件所在的 list；replay k 若在**另一 list/queue**上仍在跑
   （vLLM piecewise 多图 + eager 交错，跨 list 无顺序保证），其 clone 的事件未信号，
   `BatchQktCollectStatusGated` 的就绪门**正确地跳过**它们 → clone 滞留 pending。
4. 但 drain 末尾的 W release 是 **context 级无条件**（:5448 及全部 drain 路径）——
   把 W_k 连同窗口一起释放。**滞留 clone 的 W 从此不在 pending_ws_ 里**。
   （设计注释声称"drain 先于下一个 W push → 窗口相关 by construction"（:10958）
   —— 它假设 drain 消费全部 pending clone，实际只消费 gate 通过的子集，不变量为假。）
5. 后续 submission 的 clone 继续滞留/累积；下一个 poll sweep arm：批 = 滞留 clone
   （多个 submission）+ 新 clone，wait = 只有**新** submission 的 W。
6. device 查询在新 W fire 时执行 = 新 replay 完成、**滞留 clone 的 kernel 还在跑/
   未启动** → 包 = 0xFFFFFFFF / 部分写 → `TsReadIsBad`（:4022）→
   `[TSBAD] dropped record`（fail-loud 丢弃，每轮 ~86 条）。

**观测证据**（rbsmkc2，全部闭合）：

| 证据 | 数值 | 含义 |
|---|---|---|
| 第一条 armed read 批 | n=110（tslog:590） | 唯一异常批 |
| 该批窗口内 valid graph=1 + TSBAD | 24 + 86 = **110** | 逐条闭合：全丢在这一批 |
| drain 小批之和 | 143（全净） | drain 读点本身没丢 |
| 总账 | 143 + 110 + 7×307 = 2402 = cmds | 全量闭合 |
| 读快照 | 490874 的 463-467 完成、468 在跑、469+ 未启动；506701+ 全未启动 | mid-flight 一次读点 |
| BQ-SUMMARY | fallback=0 capture_skip=0 w_degrade=0 w_latched=0 | Execute 的计数化 bail 全未发生 → 滞留由 release/gate 非对称造成，无计数可见 |
| 正式臂 rbga1/rbga2 | TSBAD=0 | decode 一步一图一同步，无滞留，机制不触发 |
| 旧栈 m1smoke0 | TSBAD=0 | v4c wait=被读事件本身，结构性健全 |

**为什么正式臂为 0**：decode 稳态单 graph/步 + 每步 host-sync 覆盖自己的 clone，
gate 恒全过、无滞留 → wait 集恒覆盖批。smoke 早段 prefill 多图 + eager 交错
（17ms 阻塞 memcpy）制造滞留 → 机制触发。

## 2. 修复方案

### 方案 A — arm 覆盖性过滤（读侧）

arm 收集批时只收"其 submission 的 W 仍 pending"的 clone（需 clone→W 关联：
staging 时给 clone 记 token，ZeCommand 已有 `signal_gen_` 字段可类比加
`bqkt_w_token_`）；未覆盖 clone 不进批，留给 drain 就绪门路径。release 逻辑不动。

- **好处**：diff 最小（一个过滤谓词 + 一个字段）；arm 的批/wait 恒覆盖、结构健全；
  不碰 W 生命周期与 5 处 release 站点，回归面小。
- **坏处**：**不闭环**——滞留 clone 的最终读取依赖 drain 的 status gate；W 模式下
  该 gate 的健全性依赖 app reset 事件的行为（观测成立但非设计保证，见 §6）。
  且滞留 clone 的 W 已被释放，arm 永远不再等它们——gate 一旦失真就是
  **静默错代记录**（二阶隐患原样保留）。

### 方案 B — release 对齐消费（源头；per-submission W 生命周期）

W 与 submission 的 clone 集建立引用关联：pending_ws_ entry 从 {context, w} 扩为
{context, w, token, refcount}；clone staging 时写 token。**每个消费点**（Execute
成功批、CompletePollRead 批、legacy per-event 尾）逐 clone 减引用；引用归零才
release 该 W。滞留 clone 的 W 保持 pending。

- **好处**：从源头修复不变量——"窗口相关 by construction"由假设变为构造保证；
  **arm 代码零改动**（wait=pending W 的既有语义自动变健全：pending W 集恒 ==
  pending clone 的 W 集）；滞留 clone 不再被抛弃，其 W 继续作为完成门；不引入
  新的滞留类。
- **坏处**：release 路径复杂化——4-5 处 release 站点全改引用计数式；需要 W 泄漏
  兜底（clone 被 teardown 清掉而引用未清 → abandon 路径，teardown 已有
  device_ok=false 模式可扩展）；ZeCommand 加字段（每 clone 8B）；diff 中等偏大、
  消费点多（C3 全链路 grep 逐点核对）。

### 方案 C — 代际门（gate 升级）

`BatchQktEventReady`（:4920）从纯 status 升级为 status + 代际匹配：collect 仅当
event 的**完成代** ≥ clone 的 `signal_gen_`；完成代在 gexp drain 的 same-graph
host-sync 后推进。一个原语覆盖所有读点（arm 批构建、CollectStatusGated、fallback）。

- **好处**：理论上最彻底，同时封堵二阶隐患（stale-but-valid 静默错代）。
- **坏处**：完成代推进点难以精确定义——same-graph sync 只推进本 graph 事件的代，
  跨图滞留 clone 的代永不推进 = 永不收集（**新形式的永久滞留**，还需补推进站点）；
  latched 事件下"完成代"与 status 解耦需要 collector 全程跟踪每次 signal，状态机
  复杂；t8/T14 的 gate 行为依赖改变，回归面最大。

### 对比

| | A（arm 过滤） | B（release 对齐） | C（代际门） |
|---|---|---|---|
| 闭环性 | 部分（drain gate 留口） | **闭环** | 闭环（理论） |
| arm 改动 | 过滤谓词 | **零** | 批构建 |
| release 改动 | 无 | 引用计数式 | 无 |
| 新滞留风险 | 滞留交给 drain（现状已证净） | 无 | 跨图代不推进=永久滞留 |
| diff/回归面 | 小 | 中 | 大 |
| 二阶隐患 | 保留 | 不变（见 §6） | 封堵 |

## 3. 推荐：B 为主修 + A 的粗版作防御性降级

- **主修 = B**（release 对齐消费）。恢复不变量的源头，arm 零改动。
- **附带守卫（A 的粗版，belt-and-suspenders）**：arm 收集循环里若发现批内存在
  token 无对应 pending W 的 clone → **整批降级**（不 arm，留 drain，w_degrade++
  留痕）。不需要逐 clone 过滤，几行代码；防未来其他路径再破坏不变量时 fail-loud
  而非静默（P2）。
- 不推荐 C：状态机复杂 + 永久滞留风险 + 回归面最大，收益不比 B 多。
- D2 因果确认（可选）：`UNITRACE_GRAPH_QKT_SHADOW_W=0` smoke 臂（v4c wait 形态）
  预期 TSBAD=0，直接证明 W 模式是因果——可与修复后验证合并为一轮 smoke 三臂。

## 4. 实现要点（影响面，v2 按 Tina 约束细化）

### 4.1 数据结构与状态

- `uni_batchqkt_pending_ws_`（:10964）entry：{context, w} →
  **{context, w, token, live_refs}**。
- `ZeCommand` 新字段：`uint64_t bqkt_w_token_`（0=无）+ `bool bqkt_w_done_`
  （terminal flag，防重复 decref）。
- staging 态（OnEnter→OnExit 窗口内累积）：per-collector
  `uni_batchqkt_staging_{token, refs}`（uni_batchqkt_mutex_ 保护）：
  PrepareGraphExecution 分配 token=++计数并清 refs；clone loop 逐 clone 写
  token、refs++；OnExit `InjectShadowW` push {context, w, token, refs}。
- **窗口内的空档是合法态**：OnEnter 与 OnExit 之间 clone 带 token 但 W 未 push——
  此窗口内其他线程 arm 会命中 A 守卫 → 降级 drain（正确的 fail-closed）。

### 4.2 A 守卫（fail-closed，独立 commit）

`BatchQktArmPollRead` 收集循环后：批内任一 clone 的 token 在 pending_ws_ 无
匹配 entry → **整批不 arm**、w_degrade++、一次性诊断行
（[BATCHQKT] arm guard: N/M clones lack pending W, degrading to drain）。
禁止静默跳过。

### 4.3 B 引用闭环（主修 commit）

release 条件 = "该 token 的 live_refs 归零"，不再随 drain 无条件释放。

**6 条 release/终止路径（C3 全链路核对清单）**：

| # | 路径 | 处理 |
|---|---|---|
| 1 | 正常完成 | clone metric 读取 + 移出 submissions → decref（terminal flag 先查后置） |
| 2 | abandon（clone 被取消/不再收集） | 显式 decref |
| 3 | device lost / device_ok=false | 批量终止未完成 clone，统一 decref（既有 abandon 语义） |
| 4 | context teardown | **先 force-close 全部 live clone（decref）再放 W**——不得直接无条件放 W |
| 5 | 部分构建失败 | clone 已 staging 但 graph append 失败：回滚引用（decref 全部该 token clone） |
| 6 | 重复完成/重复回调 | terminal flag：completed/abandoned/failed 后只允许一次 decref |

### 4.4 计数器（监测全集）

- 新增：`clone_attached`、`clone_completed`、`clone_abandoned`、
  `W_pending`（gauge，summary 时 = created−released−abandoned）、
  `clone_live`（gauge = attached−completed−abandoned）；W_created = 既有 w_pops。
- 既有：`w_released`、`w_degrade`、`w_abandoned`、`cmds/seen/collected`、TSBAD。
- **合入恒等式**（任何 summary 必须满足）：
  `W_created == W_released`（teardown 后）；
  `clone_attached == clone_completed + clone_abandoned`；
  `metric_expected == metric_collected`（census gpu_op ON==OFF）；
  teardown 后 `W_pending == 0 && clone_live == 0`；
  正常路径 `W_degrade == 0`。

### 4.5 预期行为变化

滞留 clone 的 W 保持 pending → arm 会等它（正确等待）或 guard 降级 → 丢失类
从"每轮丢 ~86 条"变为"延后到完成代"；qkt_at_poll/qkt_at_drain 分布微移
（~86/2402 量级）。

## 5. 验收判据（预注册，v2 按 Tina 两管表定稿）

### 5.1 smoke 三臂（card2，GPU gate 全过）

| 验证项 | B+A 修复管（meta ON） | meta OFF 臂 | SHADOW_W=0 对照管 |
|---|---|---|---|
| TSBAD | **0** | 0 | 0 |
| census gpu_op | **ON==OFF 精确相等**（达成则 §3 字节门口径修订自动撤回） | 同左 | 精确相等 |
| footer / dmesg | 1 / 0 | 1 / 0 | 1 / 0 |
| W_degrade | 正常负载 0 | — | 单独记录 |
| W 恒等式 | W_created==W_released==gexp 数；clone_attached==completed+abandoned | — | original/shadow 均无泄漏 |
| completion / meta 门 | 既有 smoke 判据不改 | 既有 | 既有 |

### 5.2 不只看 TSBAD=0（Tina 约束）

- metric_expected == metric_collected；W_created == W_released；
  clone_attached == clone_completed + clone_abandoned；teardown 后
  W_pending==0、clone_live==0；W_degrade 正常路径为 0。

### 5.3 其余门槛

- 多 submission 并发：clone 不跨 submission 错配（批内 token 单一性检查）。
- 多 list 场景：replay/gexp/drain 分布不同 list 不丢数据（smoke prefill 即覆盖）。
- 长稳：pending W / live clone 不持续增长（正式臂复跑看 summary gauge）。
- teardown：退出后引用全归零（summary 终值）。
- device lost：无 UAF/double-free/永久残留（代码审计 + 既有 alarm 监控）。
- 降级可见性：degrade 必有 counter/log。
- 性能：refcount/集合维护开销 ≤0.3pt（L4 主口径，正式臂复跑）。
- t14 / T8 / A5 / trace↔tslog 1:1 全套不变。

## 6. 已知边界与二阶隐患

- **drain 的 status gate（CollectStatusGated）健全性依赖 app reset 行为**：观测
  （seen=3603 vs collected=2402，1201 次 mid-flight skip）表明负载在 mid-flight 时
  事件确为未信号态（app reset 过），gate 实际有效；若某负载从不 reset 事件
  （latch 恒真），gate 恒通过 = drain 的 Execute（numWaitEvents=0）可能错代读。
  本修复不改变此路径——列为观察项，由 t8 CHECK2（因果）+ trace↔tslog 1:1 覆盖。
- mid-flight 的 stale-but-valid 包（<1ms 窗口、kernelEnd 已写但包未定稿）：
  B 修复后 armed read 恒等到全部 W fire，该类只可能出现在 drain 路径（同上观察项）。
- A 守卫的已知局限（Tina 指出）：143/143 drain 干净只能证明当前测试场景能清理，
  不能证明所有并发时序正确——守卫只拦 arm 侧，drain 侧可见范围问题由 B 闭环兜住。
