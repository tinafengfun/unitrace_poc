# unitrace v4c-r2+ 构建与环境变量开关手册（build9 出货版）

> 2026-09-30。适用源码状态：ze_collector.h `a651610c`（build9，出货默认全开版）/
> ze_event_cache.h `20478073` / chromelogger.h（meta 版）。
> 前置战果：node3 trim24L ITL **38.43/38.43（+2.64% vs bare 37.44，规格 ≤38.57 PASS）**，
> 长收尾 vdelong 18 分钟 Mean 38.43-38.45 无漂移（bqm1）。
> 设计原则：**出货特性默认全开（不设任何 env 即为完整功能），每个开关 `=0` 可单独关闭**
> ——用于 A/B 测试与回归防抵押。

---

## 1. 如何编译

### 1.1 推荐路径：cri2 容器构建（与全部已验证产物同链路）

构建机 = <build-host> (tfeng@<build-host>, sudo 密码见内部凭据台账（不入库）)，容器 `tf_unitrace_v4c2`
内做 fresh configure（cmake 4.x 绝不复用陈旧 build 目录）：

```bash
# ① 源文件 scp 到 cri2，再 docker cp 进容器源树
scp tools/unitrace/src/levelzero/ze_collector.h \
    tfeng@<build-host>:/mnt/disk3/home/tfeng/staging_ze_collector_buildN.h
ssh tfeng@<build-host>
sudo docker cp /mnt/disk3/home/tfeng/staging_ze_collector_buildN.h \
    tf_unitrace_v4c2:/mnt/disk3/home/tfeng/unitrace_sdk_v4c2/unitrace_sdk/tools/unitrace/src/levelzero/ze_collector.h

# ② 容器内 fresh configure + ninja（必须 bash -lc 登录 shell，ninja 在 oneAPI env 里）
sudo docker exec tf_unitrace_v4c2 bash -lc \
 "SDK=/mnt/disk3/home/tfeng/unitrace_sdk_v4c2/unitrace_sdk; B=\$SDK/tools/unitrace/build; \
  cd \$B && rm -rf CMakeCache.txt CMakeFiles && \
  cmake .. -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > cmake_config.log 2>&1 && \
  ninja unitrace unitrace_tool > make.log 2>&1; echo rc=\$?; \
  md5sum \$B/unitrace \$B/libunitrace_tool.so"

# ③ 取出产物（docker cp 出来再 scp；docker cp 出的文件需 sudo chmod 644）
sudo docker cp tf_unitrace_v4c2:$B/libunitrace_tool.so /mnt/disk3/home/tfeng/staging_lib_buildN.so
sudo chmod 644 /mnt/disk3/home/tfeng/staging_lib_buildN.so
scp tfeng@<build-host>:/mnt/disk3/home/tfeng/staging_lib_buildN.so <目标机 staging>/buildN/
```

参考脚本：`poc_scripts/build/build_c2.sh`（含 strings 自检：`qkt_at_poll`/`GRAPH_BATCH_QKT` 计数 ≥1）。

### 1.2 快速语法门（本地，无 GPU 无容器）

改动 header 后先过本地语法检查再上构建机：

```bash
bash poc_scripts/build/syntax_check_local.sh   # -fsyntax-only，0 错误才继续
```

### 1.3 部署目标机前必查（node3 教训）

```bash
ldd libunitrace_tool.so | grep "not found"   # 必须为空；build3 曾缺 libxptifw.so.1 全灭
```

若缺：从 cri2 容器 `/opt/gfx-deps/dpcpp/lib/libxptifw.so.1.0.1` 取出，放目标机目录并建
`.so.1` symlink；注意**同进程只允许一套 libxptifw**（双套 → phmap assert SIGABRT）。

---

## 2. 环境变量开关总表（build9 默认值）

### 2.1 出货特性——**默认全开**，`=0` 关闭（A/B 用）

| 开关 | 默认 | 开启效果 | `=0` 关闭后形态 | 实测税贡献（node3 trim24L） |
|---|---|---|---|---|
| `UNITRACE_GRAPH_BATCH_QKT` | **ON** | 图时间戳批量读：290 事件挂 collector 专用 immediate list 每步一批读 | legacy 每节点 timestamp 事件（v4b 形态） | 净回收 **4.25ms/步**（关=大回退） |
| `UNITRACE_GRAPH_QKT_AT_POLL` | **ON** | 批量读武装在设备忙的 poll sweep（设备侧等待） | 读解析挪到 staging drain（v4b 形态） | 与 BQ 联动；关=退回 drain 解析 |
| `UNITRACE_DEFERRED_TS` | **ON** | eager 命令的时间戳 query+read+emit 挪 completer 线程（与设备执行重叠） | drain 内 inline 处理（+0.48ms/步） | **−0.48ms/步** |
| `UNITRACE_TRACE_META` | **ON** | `unitrace.*` meta 记录（enqueue/emit/qkt_batch/reset_batch/step/final_flush） | 无 meta 记录，其余字节等同（逃生门） | ≈0（纯 host 侧） |
| `UNITRACE_GRAPH_BATCH_QKT_RESET` | **ON** | 事件批量 reset（290×append + 1×sync） | 逐事件 fallback reset（`reset_fallback` 计数升高） | 批量版更省 |

### 2.2 可选实验项——默认关，`=1` 开

| 开关 | 默认 | 效果 | 备注 |
|---|---|---|---|
| `UNITRACE_ASYNC_EMIT` | OFF | chrome 记录构造挪独立 emitter 线程 | 实测 ITL 中性（emit 本就 100% 隐藏）；保险项 |
| `UNITRACE_TS_EXT` | OFF | 扩展 kernel 时间戳源 | 实验性 |

### 2.3 诊断/调试——默认关

| 开关 | 效果 |
|---|---|
| `UNITRACE_DEBUG_TS` | 时间戳换算逐条打印（TSCONV） |
| `UNITRACE_DEBUG_TS2` | inline 顺序诊断模式；**强制 legacy 逐事件查询路径**（BQ 被忽略并打一行 notice） |
| `UNITRACE_DEBUG_DEFER` | deferred-ts 诊断输出 |
| `UNITRACE_DEBUG_BATCHQKT` | BATCHQKT 生命周期日志（one-shot 门控） |
| `UNITRACE_DEBUG_EMIT` | async-emit 诊断输出 |

### 2.4 注入测试——默认关

| 开关 | 效果 |
|---|---|
| `UNITRACE_TEST_ALLOC_FAIL` | 强制每个 BATCHQKT 分配失败 → 验证优雅降级（预期 batches=0 + `qkt_skip_alloc_fail>0` + 无 fault） |

### 2.5 launcher 侧

| 开关 | 效果 |
|---|---|
| `UNITRACE_UseResultDirectory` | 结果目录组织（logger_factory） |

---

## 3. 使用示例

### 3.1 默认使用（零 env，全部出货特性已开）

```bash
# SERVER（标准形态：四 chrome flag 必带，--start-paused 控采集）
VLLM_XPU_USE_CUSTOM_MODEL=1 VLLM_XPU_ENABLE_XPU_GRAPH=1 ZE_AFFINITY_MASK=0 \
NEOReadDebugKeys=1 EnableImplicitConvertionToCounterBasedEvents=0 \
<path>/unitrace --chrome-itt-logging --chrome-sycl-logging --chrome-call-logging --chrome-kernel-logging \
--output-dir-path <out-dir> --start-paused \
vllm serve /models/<model>/ --trust-remote-code --tensor-parallel-size 1 \
--max-model-len 8192 --no-enable-prefix-caching --enable-chunked-prefill \
--max-num-seqs 128 --shutdown-timeout 120

# CLIENT（压测；注意：本体系不用 --profile，采集由 session 控制驱动）
no_proxy=127.0.0.1 vllm bench serve --base-url http://127.0.0.1:8000 \
--model /models/<model>/ --dataset-name random \
--random-input-len 3500 --random-output-len 100 --max-concurrency 1 \
--num-prompts 2 --request-rate inf --num-warmups 2 --ignore-eos \
--ready-check-timeout-sec 0
```

要点：四个 `--chrome-*-logging`（itt/sycl/call/kernel）是被验证配置必须全带；
`--profiler-config.profiler xpu` 与客户端 `--profile` 刻意不用；模型路径两端一致。

### 3.2 验证默认特性已生效（冒烟）

trace/summary 里应有（grep 判据）：
`BQ-SUMMARY` 行 `batches>0`、`reset_fallback=0`、`collected==seen`；chrome trace 有
`urEnqueueGraphExp` 与 `unitrace.enqueue` 记录、footer 存在。

### 3.3 A/B 回归测试（防退化）

一次只关一个开关，与默认臂对比 Mean ITL（±3% 规格线，锚=同窗裸跑）：

```bash
# 全开基线（默认）
<标准命令>                                   # 期望 38.43 量级

# 逐项关闭二分（每次只改一个）
UNITRACE_GRAPH_BATCH_QKT=0 <标准命令>        # 期望显著回退（~39.9 量级，legacy 每节点事件）
UNITRACE_GRAPH_QKT_AT_POLL=0 <标准命令>      # 期望小回退（读挪 drain）
UNITRACE_DEFERRED_TS=0 <标准命令>            # 期望 +0.48ms/步（~38.9 量级）
UNITRACE_TRACE_META=0 <标准命令>             # 期望 ITL 不变（meta≈0，仅记录消失）

# 全关参考（≈u0 基线口径， sanity 下界）
UNITRACE_GRAPH_BATCH_QKT=0 UNITRACE_GRAPH_QKT_AT_POLL=0 UNITRACE_DEFERRED_TS=0 <标准命令>
```

判读：某开关关闭后 ITL **不升**=该开关无税贡献（符合预期）；显著升高=该开关是承重优化
（如 BQ=0 → +4.25ms/步）。若默认臂相对裸跑超出 ±3% 规格，用上表逐项二分定位退化来源。

### 3.4 历史臂复现对照（如需对齐旧数据）

| 旧臂 | 复现 env（在默认全开基础上） |
|---|---|
| v4b（仅 BQ） | `UNITRACE_GRAPH_QKT_AT_POLL=0 UNITRACE_DEFERRED_TS=0 UNITRACE_TRACE_META=0` |
| v4c（build3 形态） | `UNITRACE_DEFERRED_TS=0` |
| vde（DEFERRED_TS 单开） | （默认即 vde 形态，另需 build8 前的 lib） |
| u0（基线） | `UNITRACE_GRAPH_BATCH_QKT=0 UNITRACE_GRAPH_QKT_AT_POLL=0 UNITRACE_DEFERRED_TS=0 UNITRACE_TRACE_META=0` |

---

## 4. 产物锚点（备查）

| 产物 | md5 | 位置 |
|---|---|---|
| build9 源 ze_collector.h（默认全开版） | `a651610ca1849a7eeccf3def1cae3508` | git + cri2 staging |
| build9 lib（出货默认全开版） | `903bad204865f92dbde791bf849e6109` | node3 `bq_staging/build9/` + repro_node3 ULIB |
| build8 lib（上一版，需 env 开 BQ/AT_POLL/DT） | `a67920db`（lib）/ `0469e260`（源） | node3 `bq_staging/build8/` |
| build3 lib（V2/V3 加固版） | `9f4b8db4` | node3 `bq_staging/build3/` |
| launcher（unitrace bin） | `4f33542a` | 随构建 |
| 验收判据报告 | — | `BQ_TAX_HOTSPOT_AND_OPT.md` / `BQ_TAX_LEDGER_PERFETTO_VISIBILITY.md` |
