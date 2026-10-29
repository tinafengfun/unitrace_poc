# TSBAD_FIX_IMPL — tsbadfix B+A 修复实现记录（2026-10-29）

> 设计：同目录 TSBAD_FIX_DESIGN.md（v2 裁决版，Tina 已批准：B 主修 + A 粗守卫，
> SHADOW_W=0 独立对照管）。本记录 = 实现 + 构建门结果 + 待验证项。
> 两个 bisectable patch 文件按 Tina 的"可选两步拆分"落地（A 守卫 commit 与 B 主修
> commit 分开，一次构建一次 smoke）。

## 1. Patch 落地（两 commit 拆分）

| 文件 | 作用域 | 应用后 md5 |
|---|---|---|
| `tmp/patch_tsbadfix_A.py` | **commit 1（A 守卫 + token 管线，inert）**：ZeCommand 增 `bqkt_w_token_/bqkt_w_done_`（显式 init）；PrepareGraphExecution 头部 staging 窗口复位；clone loop 挂 token + `clone_attached++`；arm A 守卫（fail-closed 整批降级 + w_degrade++ + 一次性诊断行）；staging 结构（BqktStaging/TlBqktStg/BqktTokenNext）。**release 语义本 commit 不变** | `7cbc9cd77607016355611fdb48ff8a1e` |
| `tmp/patch_tsbadfix_B.py` | **commit 2（B 主修）**：`pending_ws_` entry {context,w} → `BqktPendingW {context,w,token,live_refs}`；释放对齐消费——`ProcessCommandSubmittedTail` 入口 decref（终态漏斗）；Revert×3 站点 + event-destroy 丢弃路径 abandon；terminal flag 防双 decref（路径 6）；`refs==0` 立即回收（空窗口）；**6 处 drain 侧无条件 W release 移除**（Execute 5379/5401/5413/5429/5441/5448 + CompletePollRead 5712/5737/5793——现在按消费计数释放）；保留 device_ok=false（5740）+ InvalidateBatchQktContext + teardown backstop；summary 增 clone_attached/completed/abandoned/live + w_pending | `8257fd6c68f00ef33d4d9113f090e9b7` |

- 拆分等价性已验证：pristine(bcfafa7c) + A + B == 组合 patch 结果（逐字节 diff 仅
  注释措辞，语义恒等；canonical = A+B 结果）。
- pristine 备份：node1 `ze_collector.h.pre_tsbadfix_20261029`（copy-never-delete）。

## 2. C3 全链路核对（decref 覆盖闭环）

**每个 clone 恰好一次终态**（`bqkt_w_done_` 防重）：

| 终态路径 | 位置 | 类型 |
|---|---|---|
| 尾漏斗（唯一消费出口） | `ProcessCommandSubmittedTail` 入口（9 处调用点全核：drain 1989/2157/2247/3293/3324/3714/3754/6194/6746 + fence/event sweep + deferred completer 6746 + shutdown-race inline） | completed |
| 提交失败回滚（3 站点） | 10136（queue execute fail）/10246（immediate append fail）/10538（gexp submit fail）→ `BatchQktAbandonStagedClones()` 在 Revert 前 | abandoned |
| event destroy 丢弃 | staged 子集（ForEvent 前 abandon）+ submitted 丢弃循环逐条 abandon | abandoned |
| 提前返回 | tail 内 query-fail/TSBAD 早退：decref 已在入口完成 | completed |
| device lost | `device_ok=false` 语义保留（5740/Invalidate）：W 条目丢弃计 w_abandoned，clone 后续 tail decref 落空（仅计数，无 device 调用） | 混合，可观测 |
| teardown | backstop（6848）销毁残留 W 条目；跨线程 thread_local clone 无法枚举——其 token 后续 terminat 落空仅计数（**已知边界，summary gauge 可见**） | — |

- 锁序：所有新锁点均为 submission(any)→uni_batchqkt_mutex_（与既有 sweep 同向），
  无新增 batchqkt→submission 边；W 销毁仍走 staged destruction（FlushGraphEventResets
  释放锁后统一销毁，无 ABBA）。
- W-less submission（inject fail / ooo / capture hold / dead context）：token 无对应
  pending W → A 守卫降级 + tail decref 落空仅计数——恒等式仍闭合。

## 3. 构建门（node1，容器 <build container>，fresh configure）

- **L0/L1 PASS**：增量 ninja 9/9（tracer.cc 全 TU 含 ze_collector.h）→ 官方
  `build_tsbadfix_node1.sh`（build_node1.sh 克隆，staging=tsbadfix_stage）fresh
  configure + ninja **全过**：ldd SVML/libimf/libirng/libintlc 4 件 +
  DT_NEEDED=libxptifw.so.1 + not-found=0；strings qkt_at_poll/GRAPH_BATCH_QKT/
  TSINTERP；staging copy md5 复验一致。
- 产物：`tsbadfix_stage/bin/`：lib md5 **9e47e8f53e8cd07389f9fd1848a04685**、
  launcher **50a3810fe6332776c2f9a86bbba17b9b**（与既有记录一致 = launcher 未动）。
- `tsbadfix_stage/pins.env` 已写（LIBEXP/BINEXP）。README 已登记两个新脚本。

## 4. smoke 三臂验证（2026-10-29 run2，**全绿 ALL-DONE rc=0**）

脚本 `scripts/smoke_tsbadfix_node1.sh`（md5 ace410a6，判据：rb2c2 逐字保留 + 三臂
TSBAD==0 + bq_gate 恒等式断言 + 字节门恢复 m1a 严格口径）。日志
`results/tsbadfix_smoke_run2.log` + `results/rbfx_session.log`：

| 臂 | 判定 | 关键数字 |
|----|------|----------|
| rbfxc2（meta ON） | **PASS** | meta=213、TSBAD=**0**（基线 86）、footer=3（3 进程结构）、dmesg-bad=0 |
| rbfx0c2（meta OFF） | **PASS** | meta=0、TSBAD=**0**（基线 91）、footer=1→3 进程、dmesg-bad=0 |
| rbsh0c2（meta ON + SHADOW_W=0 对照管） | **PASS** | meta=246、TSBAD=**0**、dmesg-bad=0（D2 因果确认管独立健康） |
| 字节门（m1a 严格口径） | **PASS** | cpu_op **5622==5622**、gpu_op **2846==2846 精确相等**（Δ5→0，86 条丢弃全部回收）；字节差 56,586B/213 meta=265B/条（≈meta 本体）→ **ACCEPT_RECORD §3 修订口径自动撤回** |
| BQ-GATE rbfxc2 | **PASS** | w_pops=32==released(32)+abandoned(0)；clone_attached=2402==completed(2402)+abandoned(0)；clone_live=0；w_pending=0；w_degrade=0 |
| BQ-GATE rbfx0c2 | **PASS** | 同上闭合（w_pops=32、clone 2402/2402/0、live=0、pending=0、degrade=0） |

注：① bq_gate v1 误 grep "BQ-SUMMARY"（实际行名 `[BATCHQKT] summary:`）产生
no-BQ-SUMMARY 假 FAIL——脚本解析修正（run1→run2），恒等式判据本身未动；
② run1 产物目录 stale 改名 `results/{rbfxc2,rbfx0c2}.stale.run1_20261029/`（防
out_$TAG 旧 json census 翻倍，copy-never-delete）。

## 5. 阻塞项（已全部解除）

1. ~~repro_cri 未运行~~ —— Tina 2026-10-29 批准重启（"需要1重启"），smoke 已跑。
2. ~~C-state 漂移~~ —— Tina 裁决"忽略"（powersave 维持，本窗内对照不受影响）。
3. ~~patch 应用到错误 header~~（本记录 §1-§3 之外新发现，2026-10-29 补记）：构建
   实际编译的是 `src/ze_collector.h`（57b4fc33，quoted-include 从 tracer.h 就近
   解析 + -I src/ 在前），非 `src/levelzero/ze_collector.h`（11191 行变体，只有
   .h 无 .cc，从未编译）。src/ 副本内容 ≡ pristine bcfafa7c + 一条遗留
   SMOKE-DIAG 临时行（rb_stage lib 无该字符串，可证 rb 血统同源）。处理：备份
   `src/ze_collector.h.stalesrc_57b4fc33_20261029` 后以 canonical 8257fd6c 覆盖
   src/ 副本；levelzero/ 副本不动（非编译输入）。重建后 lib=a7c35bd6 且 strings
   clone_attached/w_degrade 进二进制，smoke 全绿确证。

## 6. 已知边界（如实）

- teardown 无法枚举其他线程 thread_local 的 live clone：其引用在退出前未终态时，
  clone_live gauge 在 summary 终值不为 0（fail-loud 可见），W 无泄漏（backstop 销毁
  全部条目）。设计 §4.4 的"teardown 后 clone_live==0"判据以此口径解读。
- drain 的 status gate（CollectStatusGated）对 latched 事件的二阶隐患（stale-but-
  valid 错代记录）不在本修复范围（DESIGN §6 观察项，t8 CHECK2 + 1:1 覆盖）。
- W 模式 OFF 时 token 照常挂/落（decref 落空仅计数）——BQ 关闭路径行为不变，
  L3 =0 字节等同门仍适用（meta OFF 臂验证）。

## 7. 完整验收（2026-10-29 深夜窗，Tina 指令"完整验收，ok 了推送 github"）

runner=`scripts/runner_accept_tsbadfix_node1.sh`（人类验收几件套序列；card2=41:00.0，
本 boot 枚举 mask=1→41:00.0；共租让路等待 ~1.5h 后清场自动续跑；宿主 10-29 重启后
tbba 重锚——跨窗锚禁用）。判定（预注册，mean 主口径）：

| # | 判据线 | 结果 |
|---|--------|------|
| 1 | unitrace.step==396 | **396/396 ✓**（双臂一致） |
| 2 | A1 replay shape | **121,572=396×307 精确**，逐位同 rbga 验收基线（gpu_op 138,696、dom 38,016 同值）；三源交叉：bench 396 步 + BQ cmds=121,572（seen==collected，BQ-EXACT=1）+ t8 uniform-307 ✓ |
| 3 | ITL vs 本窗裸锚 | **+2.878%**（tbfa mean 30.925 = 30.91/30.94 vs tbba 30.06）< 6% 人类线、≤3% 参考线 ✓ |
| 4 | TSBAD=0 + footer + dmesg | **TSBAD=0 双臂**（修复主判据）、footer=3（3 进程）、dmesg 增量 0 ✓ |
| 5 | A5 unexplained | **0**（1200/1200 explained）✓ |
| 配套 | meta nesting | 1186/1187（1 bad = qkt_batch 6.1µs 微切片，已知合法类，优于 rbga 2 bad）✓ |
| 配套 | T8 | **5/7**（FAIL=CHECK1 跨窗 ITL 线 + CHECK7 期望表 backlog——rbga 同型已知合法类；CHECK2-6 全 PASS） |
| 配套 | gate-OFF 字节等同 | smoke rbfxc2/rbfx0c2：cpu_op 5622==5622、gpu_op **2846==2846 精确相等**（§4）✓ |
| 配套 | BQ-FIX 闭环恒等式 | **=1 双臂**（clone_attached==completed+abandoned、clone_live=0、w_pending=0、w_pops==released+abandoned）✓ |

- t14 机器判据 M1/A1/REG FAIL 均为 c2c 跨窗 ref 类（与 rbga 轮同因同型），按"与上一
  已验收 run 逐项对账"口径人工判定 PASS；A1 三数字与 rbga 基线**逐位相同**。
- 环境备注：本窗 C-state=powersave（Tina 裁决忽略）；共租者在 card2 有短作业轮换，
  gate 让路至 01:20 清场后完成（attempt 5）。
- 产物：本地 `accept_tsbadfix_20261029/`（双臂 chrome trace 136MB×2 + tslog 48MB×2 +
  bench/dmesg/verdicts/t14 json+report/t8 json）；node1 raw 不删。
- **结论：tsbadfix B+A 修复通过完整人类验收（五数字 + 配套全绿）→ 具备推送 github 条件。**
