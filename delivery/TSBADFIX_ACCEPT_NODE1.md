# TSBAD B+A 修复 —— node1 完整人类验收报告（2026-10-29）

> 修复内容与实现记录见 `TSBAD_FIX_DESIGN.md` / `TSBAD_FIX_IMPL.md`（patch 文件在
> `patches/`）。本文 = 人类验收几件套（裸锚 → smoke ON/OFF → canonical bench ×2
> → t14 收口）的最终判定。**判定线预注册，测后未改。**
> （机器地址/凭据按惯例不入库，见内部 credential ledger。）

## 结论

**PASS —— 五预注册数字 + 配套项全绿，回归判定与上一已验收 run（rbga 轮）逐项
对账无回归。** 修复主判据 TSBAD 从 smoke 基线 86/91/86 降至 **三臂 0**，且
gate-OFF 臂字节等同恢复 m1a 严格口径（gpu_op 2846==2846 精确相等）。

## 验收环境

- node1 card2，本 boot 枚举 mask=1→41:00.0；构建容器 <build container>。
- 产物 lib `tsbadfix_stage/bin/libunitrace_tool.so` =
  `a7c35bd6c8cb43dde789b32482ebf8a3`；launcher `50a3810f…`（未动）。
- 本 boot 宿主重启后 tbba 裸锚重锚（跨窗锚禁用）；共租者占用 card2 期间 gate
  让路，清场后自动续跑（attempt 5）。

## 五预注册数字

| # | 判据线 | 结果 |
|---|--------|------|
| 1 | `unitrace.step`==396 | **396/396 ✓**（tbfa1/tbfa2 双臂一致） |
| 2 | A1 replay 形状 | **121,572 = 396×307 精确**；gpu_op 138,696、dom 38,016 与 rbga 基线逐位相同；三源交叉：bench 396 步 + BQ cmds=121,572（seen==collected，BQ-EXACT=1）+ t8 uniform-307 |
| 3 | ITL vs 本窗裸锚 | tbba mean 30.06 → tbfa1 30.925 / tbfa2 30.94，**+2.878%** < 6% 人类线、≤3% 参考线 ✓ |
| 4 | 完整性 | **TSBAD=0 双臂**（修复主判据）、footer=3（3 进程结构）、dmesg 增量 0 ✓ |
| 5 | A5 unexplained | **0**（1200/1200 explained）✓ |

## 配套项

- **meta 嵌套率**：1186/1187（1 bad = `unitrace.qkt_batch` 6.121µs 微切片，已知
  合法类；优于 rbga 轮 2 bad）。
- **T8**：5/7。FAIL = CHECK1（跨窗 ITL 线，同 rbga 同型）+ CHECK7（期望表
  backlog）；CHECK2-6 全 PASS。
- **gate-OFF 字节等同**（smoke rbfxc2/rbfx0c2）：cpu_op **5622==5622**、gpu_op
  **2846==2846 精确相等** —— Δ5 彻底消失，86 条 TSBAD 丢弃全部回收；字节差
  56,586B / 213 meta = 265B/条 ≈ meta 本体 → meta=纯注释证明成立。
- **BQ-FIX 闭环恒等式**（双臂）：`clone_attached(2402) == completed(2402) +
  abandoned(0)`、`clone_live=0`、`w_pending=0`、`w_pops(32) == released(32) +
  abandoned(0)`、`w_degrade=0` → **=1**。

## 回归判定（与 rbga 轮逐项对账）

| 项 | rbga（上一已验收） | 本轮 | 判定 |
|---|---|---|---|
| step | 396 | 396 | 持平 |
| A1 shape | 121,572=396×307 | 同值逐位 | 持平 |
| 税（graph） | +2.449% REPORT-DECISION | +2.878% ≤3% 线内 | 持平级 |
| TSBAD | 0（正式臂） | 0（含 smoke 全臂） | 改善（smoke 根治） |
| A5 unexplained | 0 | 0 | 持平 |
| 嵌套 bad | 2（已知类） | 1（已知类） | 改善 |
| T8 | 5/7（同型 FAIL） | 5/7（同型 FAIL） | 持平 |

**REGRESSION-PASS。** t14 机器判据 M1/A1/REG 的 FAIL 均为 c2c 跨窗 ref 类（与
rbga 轮同因同型），按"与上一已验收 run 逐项对账"口径人工判定 PASS。

## 已知边界（如实）

- teardown 无法枚举其他线程 thread_local 的 live clone：其引用未终态时
  `clone_live` gauge 终值非 0（fail-loud 可见）；W 无泄漏（backstop 销毁全部
  条目）。本轮实测未出现该情况。
- drain 的 status gate 对 latched 事件的二阶隐患（stale-but-valid 错代记录）
  不在本修复范围（DESIGN §6 观察项）。
- W 模式 OFF 时 token 照常挂/落（decref 落空仅计数）——BQ 关闭路径行为不变，
  L3 =0 字节等同门仍适用（meta OFF 臂验证）。
