# Rebased 版本完整 vLLM 抓取回归测试报告（node1 GPU0/card1）

> 2026-10-24/25。Tina 指令（2026-10-23）：「把新版本做一次完整 vllm 抓取测试，看看有没有
> regression，eager 和 graph 做对比，做严谨 audit 和对比。」
> 对象：**rb_stage** = rebase 演练树 b97fb93（innersource master 8d737c4a + poc #1-#12 共 14 commit，
> cri2 构建，launcher md5 `3d0bf570` / lib md5 `65c7f352`）vs **bd_stage** = poc #7 4602cff
> （launcher `50a3810f` / lib `9e47e8f5`）。负载：canonical vLLM serve（Qwen3.8-27B-trim24L，
> TRITON_ATTN，--max-num-seqs 128）+ canonical bench（random 3500/100，conc=1，2 prompts + 2 warmups）。
> 日志：`results/rebase_session.log`、`results/verdicts_rebase.txt`。

## 1. 主判据（预注册，mean 主口径）

**REGRESSION = rebased graph vs bd_stage graph，2v2 mean ITL：**

| 臂 | Mean ITL (ms) | Median | P99 | 结构门 |
|---|---|---|---|---|
| rbga1（NEW graph rep1） | 30.95 | 30.87 | 31.73 | 全绿 |
| rbga2（NEW graph rep2） | 30.97 | 30.86 | 31.76 | 全绿（见 §5 incident #1） |
| oldga1（OLD graph rep1） | 30.75 | 30.64 | 31.55 | 全绿 |
| oldga2（OLD graph rep2） | 30.88 | 30.79 | 31.64 | 全绿 |

**NEW mean 30.960 vs OLD mean 30.815 → delta +0.471% → REPORT-DECISION**（预注册带
≤+0.3pt PASS / +0.3~0.8pt REPORT / >+0.8pt STOP）。落 REPORT 带，**上报待裁决**。

参考锚：同窗 bare graph rbba = 30.22 → NEW graph 总税 **+2.449%**（与 N1-T1 收口值
+2.4% 同量级，即 rebase 未放大总税；增量 0.471% 中含同窗噪声成分——bd_tax 窗
META=1/OFF 差异实测 −0.081%，而本窗 2v2 rep 间散布 oldga 30.75/30.88、new 30.95/30.97）。

## 2. eager vs graph 对比（描述性，预注册不设 pass/fail）

| 指标 | graph（rbga1） | eager（rbea1，唯一有效 rep） |
|---|---|---|
| Mean ITL (ms) | 30.95 | 30.51 |
| trace 大小 | 136.4 MB | 491.1 MB（×3.6） |
| meta 记录 | 3238 | 1268 |
| urEnqueueGraphExp | 396 | 0 |
| BATCHQKT | EXACT=1、W=1（batches 精确对账） | 全零 summary（不触发，符合设计） |
| collector_us 字段 | 41 | 0（B 层 meta_windows 为 graph 路径专属；eager 无债=设计行为，10-25 修正记账） |
| overhead_total | 2 | 2（D 层 load 总量，两模式在场） |
| eager 总税 vs rbbea（bare eager 30.21） | — | +0.99%（单 rep，记账不判） |

eager 模式 ITL 略低于 graph（−1.4%，单点观察）：trim24L + conc=1 口径下 eager 无图
提交开销差；本口径仅描述，不作结论。eager trace 更大符合预期（无 graph 复用、
逐 kernel 展开）。

## 3. 全臂结果一览（含失败与重试）

| 臂 | 配置 | 结果 | 备注 |
|---|---|---|---|
| rbba | bare graph ×1 | PASS | 30.22（本窗 graph 锚） |
| rbga1 try1 | NEW graph META=1 | **PASS** | — |
| rbga1 try1（首次发射） | — | FAIL×2 | 根因=rb_stage 缺 `lib→build11r3/lib` 符号链接（launcher 解析 `../lib/libunitrace_mpi.so` 失败，README 已知坑复刻）；修复后重跑，此前的 FAIL 留档 verdicts |
| rbga2 | NEW graph META=1 | PASS（数据有效）+incident | boot 期 bcs fault，见 §5 #1 |
| rbga2x | NEW graph 复现实验 ×1 | **PASS（未复现）** | 31.00，结构全绿 → rbga2 定性一次性事件 |
| oldga1/oldga2 | OLD graph META=1 | PASS ×2 | 同窗基线 |
| rbbea | bare eager ×1 | PASS | 30.21（本窗 eager 锚） |
| rbea1 | NEW eager META=1 | 数据有效 / 注册门 FAIL | card1 零 fault；FAIL 两因均为监控/门缺陷（见 §5 #3），ITL 30.51 可用 |
| rbea1b | NEW eager 修正门重跑 | **FAIL-HW（run 无效）** | card1 ccs+CAT fault，见 §5 #2；trace 666B 退化 |
| rbea2 | NEW eager | 未跑 | 因 #2 停手，待换窗重试 |

## 4. 结构门审计（graph 组逐臂）

- footer 恰如预期（≥1，pause-flush 竞态轮询保护下实测 3：vllm API server + EngineCore 双进程 + 收尾）；
- BATCHQKT 精确对账（BQ-EXACT=1：seen==collected==cmds 且 fallback=0；W 健康：w_pops==w_released、latched/inject_fail/degrade/abandoned=0）——**rebased 版图路径核心优化（#2/#3/#5）在 upstream 基底上行为不变**；
- gexp=396 与历史全部窗口逐位同值（每步 1 次图提交 × 396 步）；
- meta/census：3238/3236/3228（new）vs 3240/3236（old）同量级；tslog 粘行 0；dmesg-bad=0（有效臂）。

## 5. 事件与教训（全程留痕）

1. **rbga2 boot 期 fault（10-24 19:11）**：bcs 引擎 PAGEFAULT（0x0100000000220000）+ Engine reset + GuC Memory CAT error + RAS。自恢复，bench/trace 完整。**Tina 批准单次复现实验**：rbga2x 同配置复跑干净 → 定性一次性事件，rbga2 数据计入 2v2。
2. **rbea1b fault（10-25 13:05）**：card1 **ccs ×3 + bcs** GuC Uncorrectable local error + Memory CAT error + **Xe devcoredump**（已抢救：`results/rbea1b/devcoredump_rbea1b_20261025.dat`，8.6MB，md5 e2477c50）。run 退化无效。**card1 一天内第二次 GuC uncorrectable** → S2 停手 → **Tina 裁决：换窗重试 eager**。
3. **监控/门两处修正（均 Tina 批准留痕）**：
   - dmesg_bad 过滤器只排 OPCI（41:00.0），共租卡 45:00.0 的 RAS 事件（10-24 21:27，5 条）误计入 rbea1 → 修正为按 PCI 归卡（非 WPCI 卡的 fault 行不计入）；
   - eager 臂预注册门两处失准：全零 BQ-SUMMARY（6 条，pause-flush 常规产物）误判为图污染；CUS≥1 硬门对 eager 不成立（B 层 meta_windows 为 graph 路径专属，eager 无债=设计）→ 修正：BQ 全零=干净（cmds>0 才算污染）、eager CUS 记账。
4. **rb_stage 部署坑复刻**：staging 必须带 `lib → build11r3/lib` 符号链接（launcher 按自身路径解析 `../lib/libunitrace_mpi.so`）——首次发射 rbga1 两 try 全灭于此，已入 runner preflight 教训链。

## 6. 硬件疑点（如实上报，不影响 graph 结论）

- card1（1a:00.0）10-24/25 两次 GuC uncorrectable（bcs 一次、ccs 一次），均 boot/init 相位、均自恢复或自限；同窗 bare 臂与其余 UT 臂全零 fault；backlog 有 card2/3 bad-pages 前科，宿主同日另有共租卡 45:00.0 RAS 事件——不排除宿主级/硬件因素，也不能完全排除与 rebased 版 boot 路径相关（单例不可归因）。devcoredump + dmesg 增量 + trace 全部留档待驱动团队。
- 换窗重试窗口（≥3h 无新 fault + gate + canary）自动执行 eager 两臂（新 tag rbea1c/rbea2，rbea1b 留档）。

## 7. 判定与建议

- **主判据 REPORT-DECISION：rebased graph vs bd_stage graph +0.471%（2v2 mean）**——在 0.3~0.8pt 带，无 STOP 级回归；总税 +2.449% 与 rebase 前 N1-T1 收口一致。结合结构门全绿、BQ/W 精确对账、gexp 逐位同值：**rebased 版功能与性能均无明显回归证据**；0.471% 增量处于噪声/带边界，建议裁决时参考 bd_tax 窗 −0.081% 的先例（同版开关差异实测可低至噪声级）。
- 待办：eager 组换窗收口（rbea1c/rbea2）→ 补 eager 税 2v2；card1 GuC 事件随证据包上报驱动团队。

## 8. 产物索引

- 会话日志/判定：`results/rebase_session.log`、`results/verdicts_rebase.txt`
- 各臂目录：`results/{rbba,rbga1,rbga2,rbga2x,oldga1,oldga2,rbbea,rbea1,rbea1b}/`（trace 136-491MB 未拉回，node1 本地保留）
- devcoredump：`results/rbea1b/devcoredump_rbea1b_20261025.dat`
- runner：`scripts/runner_rebase_node1.sh`（判据预注册于头注 + §5 修正留痕）；session 控制：`scripts/wait_bench_rb_node1.sh`
- 构建：cri2 `/mnt/disk3/home/tfeng/unitrace_sdk_v4c2/.../build/`（md5 见 §0）；rebase 演练报告：`delivery/REBASE_REHEARSAL_UPSTREAM_MASTER.md`
