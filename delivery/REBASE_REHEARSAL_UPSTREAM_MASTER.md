# Rebase 演练报告:poc #1-#12 → innersource master 8d737c4a

> 2026-10-23。演练环境:/tmp/poc_rebase(本地,未推送);上游 = innersource
> `applications.analyzers.profilingtoolsinterfaces.sdk` master `8d737c4a`
> ("[Unitrace] Added CRI support in GfxCoreFamilyToIgaGen() (#1255)")。
> 方法:`git replace --graft bbe5e6b 23bbd18f` 接通谱系 →
> `git rebase --onto 8d737c4a 23bbd18f master` 逐 commit 重放 **#1-#12 全部 14 个 commit**。

## 结论(TL;DR)

**可合并**。14 个 commit 全部重放成功,约 26 个冲突 hunk,其中只有 **1 处深语义合并**
(graph-capture 锁重构 × 我方 HARDEN 计数),其余均为机械类(改名/优雅降级块/成员块槽位)。
剩余成本 = cri2 容器编译验证 + node1 冒烟,不是合并本身。

## 逐 commit 冲突清单

| # | commit(主题) | 冲突 | 类别与解法 |
|---|---|---|---|
| 1 | bbe5e6b(v3fix 图时间戳修复) | **0** | 与 upstream 无区域重叠,直过 |
| 2 | 44db6ff(batch QKT + AT_POLL) | cl 2 / ze 1 | include 区(`<mutex>` 采纳 upstream)、成员块槽位(我方 Fix B 成员 + upstream UniModuleId 注释并存)、Close() 内插 emitter drain(结构合并) |
| 3 | 45c2710(meta + BATCHQKT harden) | ze 8 / ec 7 | **A 类 ×11**:upstream `UNITRACE_ASSERT` vs 我方优雅降级块 → 取我方(P2 设计,assert 语义被降级块取代)。**B 类 ×2(唯一深语义点)**:upstream 把 graph-capture 锁上移到函数级;我方 #3 的函数内 `lock_guard` 若照搬会**自死锁**(shared_mutex 不可重入)→ 解法 = 采纳 upstream 锁结构 + 移植我方 `uni_batchqkt_capture_depth_` fetch_add/fetch_sub |
| 4 | 78c7c0e(TS_INTERP + tslog 修复) | ze 4 | 全取我方(HARDEN 降级 ×2、摊销 GTS 调用替换 assert+真 GTS ×2) |
| 5 | 0fa7330(影子 W) | ze 1 | 成员块槽位(同 #2 模式) |
| 6 | 7c0cbc6(review-fix) | README 1 | 双保留(upstream 弃用 WARNING + 我方 fork 出处注) |
| 7 | 4602cff(B+D 开销标注) | cl 1 | diff3 把我方 Flush 尾部与 upstream Close 尾部对齐成假冲突;我方 Flush 完整在场 → 取 HEAD |
| 8-12 | ffac356/c4ab6fa/54047bd/1d917cc/e6486a9 | **0** | 全部直过 |

## 上游 39 commit 语义对照(3-way)

upstream 主题与我方 #1-#8 **目的零重叠**:upstream = 基建与健壮性
(graph 锁 ×4 UNITRACEI-238/236/212×2、self-contained 构建 UNITRACEI-159、
PTI_ASSERT→UNITRACE_ASSERT / Logger→UniLogger 改名、session-stop 落盘
UNITRACEI-120 #1089、protobuf mpi_rank 初始化修复 #1225、logger_factory fork
处理 #1257、metric config #796、CRI 支持 #1255、测试重组);我方 = vLLM XPU
图 decode 的正确性+性能+标注。接触点仅 4 处,均已在演练中合并:

1. **graph capture/锁**:upstream 结构胜出,我方 HARDEN 计数移植(见 #3 B 类)。
2. **event cache 错误路径**:我方降级块取代 assert(有意设计)。
3. **chromelogger finalize**:upstream Close()(幂等+提前释放文件)与我方
   JSON 闭合/`chrome_json_closed_` 合并 —— Close()→Flush() 统一写闭合括号并
   store 标志;teardown 直写分支 load 同一标志,守卫语义保留(已验证两路在场)。
4. **protobuf**:upstream 修 mpi_rank 初始化次序(INIT_MPI_RANK_VAL 宏),我方
   #10 设备 mark 走既有 EmitInstant API —— 正交,共存。

## 演练过程教训(复现时勿踩)

1. **graft 基底必须含 #1**:第一轮把 bbe5e6b 当排除基底只重放 #2-#12,#1 的
   v3fix 层(ZeEventHistory 结构体、signal_gen_ 快照 ×5、FlushGraphEventResets
   ×5→3、event destroy 清理)被**静默丢弃** —— tree 看起来正常但缺整个图修复层,
   且 `event_history_` 引用缺失结构体必不编译。正确做法 = bbe5e6b 也作为重放
   commit(本轮 #1 直过,无成本)。
2. **干净自动合并 ≠ 语义正确**:第一轮自动合并区出现 upstream `local_device_submissions_`
   与我方 `pre_ts` 参数的"混血"文本;必须以**事后逐 hunk 归因 diff**(final vs poc
   master,每行删除都要归因到 upstream 改动)做系统校验,本轮该扫描证实全部删除行
   可归因(改名/重构/指标整合),并抓出 1 处需补回的 `event_history_.erase`。
3. **rebase 中 git add -A 前必须确认解完**:演练中间 commit(redo-#2/#4/#6)曾把
   残留冲突标记带进提交;最终树已验证干净,但正式合并时应逐文件确认 0 标记再 continue。
   演练产物分支 `rehearsal1-partial` 保留作证据。

## 验证状态

- ✅ 14/14 commit 重放完成,最终树 0 冲突标记
- ✅ 符号 parity 13/13 全等(ZeEventHistory/signal_gen_/FlushGraphEventResets/
  event_history_/uni_batchqkt_/deferred_ts_/EmitDeviceMetaMarks/overhead_total/
  TakeContainedMetaDebtUs/shadow_w_signal/BatchQktArmPollRead/UniTsGetTimestamps/meta_windows_)
- ✅ final vs poc master 逐行归因:全部删除行 = upstream 改名/重构/整合;upstream
  新特性(graph-capture 重构、session-stop、mpi_rank 修复、UniLogger)全部在场
- ✅ perfettologger.h 差异恰为 upstream 的 +5/−5(self-contained)
- ⬜ **L0/L1 编译验证:未做**(需 cri2 容器 icx 工具链)——#3 的 HARDEN 移植与
  #2 大块是编译门重点;L2/L3(node1 冒烟 + census 对账)为合并决策前的最后门

## 合并决策建议

- 走 rebase 路线(本报告即演练结果),正式合并时以本清单为操作手册。
- 编译冒烟放 cri2(node1 跑 L3);预计 1 个工作日内可完成正式 rebase + 验证。
- 合并后我方 #1-#12 的判据(ENGINEERING_HANDOFF §3)需在新基底上全量重跑一轮
  (结构门 + 字节等同 + 税复测),因为 upstream 的 graph 锁重构改变了 host 路径
  锁行为(理论上中性,需实测)。
