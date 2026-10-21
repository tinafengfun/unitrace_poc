# META=2 设备侧标注（C 层）设计文档

状态：**草案 v1，待 Tina 评审**。前置：B+D 开销标注（commit #7 = 4602cff，已推送）已闭环——
host 侧 collector_us/api_net_us 注入 + overhead_total 终值，税复测 PASS（B+D 增量 −0.081%）。
本战役 = 分层标注的 C 层：**设备（GPU）时间轴上采集自身引入的操作的可见性**。

设计原则（与 B+D 同源，BUILD_AND_SWITCHES.md 原则块）：时间轴必须真实、不从时间轴上减开销；
开销可见 / 可归因 / 可过滤；"净耗时"走查询字段不篡改时间戳。

---

## 1. 问题：设备侧现在有什么不可见的开销

采集在**设备时间轴**上引入的真实操作（它们真实地占提交带宽和流位置，但 Perfetto 上
混在负载 kernel 中间，无法区分）：

| # | 操作 | 频率 | 设备侧成本形态 | host 侧现状 |
|---|---|---|---|---|
| S1 | **shadow-W signal**（`BatchQktInjectShadowW`，OnExit 往 graph 的 in-order list 尾部 append 一个 `zeCommandListAppendSignalEvent(W)`） | 每 graph replay 1 次 | 占 1 个流位置；设备执行一次 signal 写（sub-µs）；不可直接测（见 §6 边界） | **无 meta 窗（缺口）**——append 的 host 成本藏在 app 的 graph host 切片里，B 层看不到 |
| S2 | **batched QKT read**（armed poll read：1 次 `zeCommandListAppendQueryKernelTimestamps` 于 collector 自有 imm list，wait set = shadow W's，read set = 本步共享事件） | 每 step 1 次 | 设备在 kernel 信号后执行批量时间戳读，占流位置 | **已 self-admitted**（arm/drain 两处 `unitrace.qkt_batch` 窗，B 层归属） |
| S3 | 共享事件 re-signal（驱动在 graph 内部对 :7150 替换点的物理事件每代 re-signal） | 每 replay，驱动内部 | 藏在 graph 内，**无锚点可标** | — |
| S4 | collector-owned imm list 创建（`BatchQktEnsurePollList`） | 每 context 一次 | 无持续设备开销 | 一次性 host 成本，量级可忽略，不标 |

核心事实：**S2 的 armed read 本来就会读取本步全部共享事件的 qkt 包**——kernel 的设备
时刻已经在手里（cursor 消费处已转换到主机时间基准）。这意味着设备侧标注可以
**完全从既有包派生，不为标注注入任何新 GPU 操作**。

## 2. 设计核心：零新增 GPU 操作

C 层所有设备侧 mark 都是**既有数据的重新表达**，不为标注注入任何新的设备 op、
不增加任何 ioctl：

- W signal 的设备时刻 ≈ 本步最后一个 kernel 的 device end（in-order 流序保证
  signal 紧随其后）——该时刻就是 cursor 消费处最后一条命令的已转换 `end_time_`，
  与 kernel 切片**同一时间基准，零换算**。
- QKT read 被同一组事件门控（wait set = W，W ≈ 最后 kernel end），设备执行起点
  同样从最后 kernel end 派生。

因此 C 层税与 B 层同构：理论 ≈0。这是与"给 W 再包一层设备时间戳查询"方案的本质
区别——那会为测量而注入更多设备 op（自我膨胀，违反原则）。

## 3. 记录形态（三个 mark）

### C-1 `unitrace.shadow_w_signal`（设备 track instant）
- 触发点：**本步 batch read resolve 后、W release 前**计数 pending W（POC 冒烟实锤：
  drain v4b 路径的 release 在 batch.ok 之前，release 后数恒 0 会吞掉 drain 步的 C-1
  ——首版冒烟 shadow_w_signal=8 vs shadow_w_append=32 即此缺口，已修）；
  且**仅当本步确有注入的 W**（计数 >0）——degrade 步无 W，不发（如实缺失）。
- 时刻：**本步全部成功读取包的 max(end_time)**（已转换 host-ns，与 kernel 切片对齐）。
  🔴 反思修正：不能用 list 序最后一条命令的 end_time——多 engine/tile 并行时
  in-order list 序 ≠ 时间序，list 序最后的 kernel 可能更早结束；W 真实信号时刻 =
  最后结束的 kernel = max(end_time)。
- **TSBAD 规则**：坏包不参与 max；本步全部包坏 → 无诚实锚点，跳过发射。
- 落点：**device track**——复用 `GetDevicePidTid(device, engine_ordinal, engine_index)`
  与 kernel 切片同 track，这样在 Perfetto 里 W mark 出现在 kernel 之间才可读。
- 形态：**instant（ph=R / protobuf zero-dur slice）**，cat=`unitrace_meta`，cname=grey。
- args：`{"w_pops_step": 1}`——v1 从简：不带回填 bracket。

### C-2 `unitrace.qkt_read_device`（设备 track instant）
- 触发点：**任何成功的 batch read（arm 路径与 v4b drain 路径都发）**——degrade 到
  drain read 时设备侧同样有一次 QKT op，不可漏。🟡 反思修正：v1 只写了 arm 路径。
- 时刻：同 C-1 的 max(end_time) 锚点（read 被 W/事件门控，设备执行起点 = 最后
  结束 kernel 的 end）；TSBAD 规则同 C-1。
- 形态：同 C-1 instant；与 C-1 在同一时刻叠两个 instant，靠 name 区分
  （Perfetto 沿 track 纵向堆叠，可读）。
- args：`{"n": <本步包数>}`。
- 语义备注：QKT read 的**设备执行时长未知且可能非平凡**（n 个包的批量写，与
  4.5µs/op 管线排空发现同类形态）——instant 是诚实的下界表达；不可见 ≠ 不存在，
  这正是 C 层的价值。

### C-3 `unitrace.shadow_w_append`（host 窗，走 B 层既有机制）
- 触发点：`BatchQktInjectShadowW` 内，`mt0/mt1` 包住 `zeCommandListAppendSignalEvent`
  调用（与 5637/5779 处 `unitrace.qkt_batch` 窗完全同构）。
- 语义：发一个 host meta 窗 → B 层 `TakeContainedMetaDebtUs` 在**包含它的 graph host
  切片**（EVENT_COMPLETE 直接父）上自动归属为 `collector_us`——不需要新机制，S1 的
  host 缺口就此闭合。
- 只有 META=2 时发射（META=1 保持字节等同现版）。

### D 总计联动
- 设备 mark 是 meta 记录 → 自然计入 `uni_meta_records_n_`。
- `unitrace.overhead_total` args 增加 `device_marks_n`（只计数）。**设备侧成本不折算进
  `collector_total_us`**（host µs）：设备时钟语义不同，混算会伪造精度；设备侧成本的
  表达就是 mark 本身（可见性），这与"不篡改时间轴"原则一致。

## 4. 开关语义

| UNITRACE_TRACE_META | 行为 | 字节等同性 |
|---|---|---|
| 0 | 关闭（回归工具定位不变） | 与现版 META=0 逐位同 |
| 1 | host B + D（现状） | 与现版 META=1 逐位同 |
| **2** | **= 1 + C 层全部 mark（用户完整模式）** | 新增 mark 行/事件 |

## 5. 双路输出

- **JSON**：新 helper `DeviceMetaJsonLine`（与 `MetaRecordJsonLine` 同形，但带 device
  track 的 pid/tid/deviceid 三元组），走既有 thread_local buffer + finalized fallback
  直写通路（teardown 陷阱的兜底已就绪，发射线程 = drain/poll 线程，其 buffer 正常存活）。
- **protobuf**：`PerfettoEmitDeviceSlice` 旁新增 zero-dur instant 发射（cat=unitrace_meta）；
  若 Perfetto 视图对 device track instant 渲染不佳，回退形态 = 1µs thin slice（实现期
  用真实 trace 在 UI 里裁决，POC 阶段定稿）。

## 6. 税分析与判据

| 成分 | 新增 GPU op | 新增 ioctl | 预期税 |
|---|---|---|---|
| C-1/C-2 设备 instant | 0 | 0 | ≈0（消费处两次字符串构造 + 一次 buffer push） |
| C-3 host 窗 | 0 | 0 | 与现有 `unitrace.qkt_batch` 窗同量级（每 step 2 次 clock 读） |
| D 联动 | 0 | 0 | 计数器 fetch_add |

🔴 **实现期修正（2026-10-22，首轮税复测 STOP 后实锤）**：预期税 ≈0 对 mark 成立，对锚点
循环的**初版写法**不成立——max-end 循环对每步 307 包二次调 `GetHostTime`，而
`UNITRACE_DEBUG_TS=1` 下每次 `GetHostTime` 写一条 ~250B `[TSCONV]` 诊断行（行锁
fwrite），实测 TSCONV 行数 260,268 vs META=0 臂 138,696，**差值 121,572 = 396 步 ×
307 包精确闭合**，tslog 多写 +20.5MB/次，全在 `uni_batchqkt_mutex_` 持有下的 drain
关键路径 → +1.185% STOP。修复 = `GetHostTime` 加 `emit_diag` 参数（默认 true，
C 层循环传 false），诊断行精确回到 META=0 基线；循环残余 = 纯算术 ~30µs/步（≈0.1%，
噪声级）。教训入 §11。

**预注册判据**（与 bdtax 同式）：META=2 vs META=0，B+D+C 增量 ≤ +0.3pt（mean，主口径；
median/P99 记账不判）。>0.8pt 或 dmesg 异常 → STOP。

## 7. 已知边界（如实声明）

1. **W signal 的真实设备执行时长不可直接测**——测它需要在 W 前后注入设备时间戳 op，
   违反零新增原则。以 instant 表达（真实成本 sub-µs，instant 是诚实的下界表达）。
2. **S3 re-signal 无锚点**——藏在驱动 graph 内部，除非驱动暴露设备侧标记，否则不可标。
   属驱动侧改进项，不在本战役。
3. **C-1 的上括号含 idle**——若做 thin slice 会把设备 idle 计成开销（伪造），故 instant
   是唯一不撒皮的形态（与 §2 一致）。
4. 多设备/多 engine：mark 落点取本步 commands 的 (device, engine_ordinal, engine_index)，
   天然多 track 分散，无单点假设。

## 8. 实现点清单（锚点）

| 改动 | 文件:位置 | 内容 |
|---|---|---|
| C-3 host 窗 | ze_collector.h `BatchQktInjectShadowW`（~5996 append 调用处） | mt0/mt1 + `MetaRecord("unitrace.shadow_w_append", ...)`（仅 META≥2） |
| C-1/C-2 发射 | ze_collector.h drain/poll cursor 消费收口（batch.ok 处，~5692/5791 区域） | 末命令 `end_time_` + device/engine 三元组 → 发射两个 device instant |
| 开关门 | ze_collector.h `MetaOn()` 旁 | `MetaDeviceMarks()`（env==2） |
| JSON 路径 | chromelogger.h `MetaRecordJsonLine` 旁 | `DeviceMetaJsonLine`（device pid/tid 版）+ EmitMetaRecord 的 device 分支 |
| protobuf 路径 | chromelogger.h `PerfettoEmitDeviceSlice` 旁 | device instant 发射 |
| D args | ze_collector.h `ZeCollector::Finalize`（overhead_total 处） | args 加 `device_marks_n` |

新计数器：`uni_device_marks_n_`（lifetime，load 非 exchange）。

## 9. 验证计划

1. **POC 冒烟**（node1 card1，bd_stage 纪律不变）：META=2 单臂——footer、completion、
   C-1/C-2 instant 数 = replay/step 数、C-3 窗在 graph host 切片归属（collector_us 增量
   对账）、dmesg-bad=0；META=1 臂字节等同现版（md5 对照）。
2. **税复测**：META=2 ×2 vs META=0 ×2 + bare 锚（runner_bdtax_node1.sh 改臂版），§6 判据。
3. 文档收口：BUILD_AND_SWITCHES.md 原则块 C 行更新 + README 索引。

## 10. 待裁决点（Tina）

1. **META=2 是否含 C-3（host 窗闭合 S1 缺口）**——推荐**含**（零成本、复用 B 机制、
   缺口不闭合则 W 的 host 成本永远不可归因）。
2. **设备 mark 形态 = instant（推荐）vs thin slice**——推荐 instant（§7.3，slice 会把
   idle 计成开销）。
3. **`device_marks_n` 计入 overhead_total args（推荐）**——只计数不折算 µs。
4. protobuf 设备 instant 的渲染若不佳，是否接受 1µs thin slice 回退形态——POC 阶段
   用真实 trace 裁决后回报。

---

## 11. 设计反思记录（2026-10-21，Tina 要求正反两面复核后定稿）

**裁决**：C-3 含入 / instant / device_marks_n 计入 / protobuf 回退形态先测再定。

**反思修正（3 处）**：
1. 🔴 C-1/C-2 锚点从"list 序最后一条命令的 end_time"改为**本步成功包的 max(end_time)**
   ——多 engine 并行时 list 序 ≠ 时间序，原方案会把 W mark 画早（违背时间轴真实性）。
2. 🟡 TSBAD 规则补全：坏包不参与 max；全坏跳过发射。
3. 🟡 C-2 覆盖 arm + drain 两路；C-1 仅在确有注入 W 时发射（W release 点）。

**正反面权衡**：
- 正：C 层闭合设备侧可见性；S1 host 缺口闭合（W append host 成本可归因）；零新增
  GPU op、税理论 ≈0；QKT read 设备时长未知且可能非平凡——可见性有实际价值。
- 反：设备侧操作低频（W 1/replay、QKT 1/step），单步信息增量有限；instant 不表达
  时长（缓解：args 注明下界语义）；模式矩阵 0/1/2 × json/protobuf 验证面变大
  （缓解：POC 冒烟覆盖字节等同性预注册）。
- 结论：值得做。完整性 + S1 缺口是实打实的收益，反面各项均有缓解或如实声明。
- 否决的替代方案：给 W 包设备时间戳 op（自我膨胀，违反零新增原则）；C-1/C-2 合并
  为单 mark（可过滤性优先，双 name 支持独立隐藏）。
- **税复测 STOP 复盘（2026-10-22）**：首轮 +1.185% STOP 不是"设计税"而是"诊断重排放
  泄漏"——锚点循环二次转换时每包重发一条 TSCONV 诊断行。三点教训：
  1. 「理论 ≈0」的成本论证必须枚举**每一行被调用代码的副作用**，不能只数"新增记录数"
     ——`GetHostTime` 这类共享函数带着诊断发射副作用，复用它 ≠ 零成本；
  2. 预注册判据救了战役：STOP 线强制停下归因，而不是把 +1.2% 当"可接受"放行；
  3. 归因要靠算术闭合（TSCONV 差值 = 396×307 分毫不差），而不是嫌疑列表。
  修复后复测 = m2tax2（tag m2tbc/m2c1/m2c2/m2d1/m2d2）。
