#!/usr/bin/env python3
# patch_tsbadfix_A.py — tsbadfix COMMIT 1 (A 守卫 + token 管线，bisectable)
# Applies on pristine ze_collector.h (md5 bcfafa7c84cbca52b23b7e150e8db977).
# Scope: ZeCommand token fields + explicit init, staging window (reset at
# PrepareGraphExecution head, attach at the clone loop), A guard in
# BatchQktArmPollRead (fail-closed degrade), clone_attached counter + one-shot
# diag notice. Release semantics UNCHANGED in this commit (inert plumbing).
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "ze_collector.h  # usage: patch_tsbadfix_X.py <path-to-ze_collector.h>"
src = open(P).read()
orig = src
fails = []

def rep(name, old, new, count=1):
    global src
    n = src.count(old)
    if n != count:
        fails.append(f"{name}: matched {n}x (expected {count})")
        return
    src = src.replace(old, new)

rep("A1-fields", """  const ze_kernel_timestamp_result_t *deferred_ts_;
  bool implicit_scaling_;""",
"""  const ze_kernel_timestamp_result_t *deferred_ts_;
  // tsbadfix: shadow-W reference this clone carries (0 = none). Attached at
  // the replay staging loop; terminated exactly once (bqkt_w_done_ guards
  // double-termination) when the clone terminates.
  uint64_t bqkt_w_token_;
  bool bqkt_w_done_;
  bool implicit_scaling_;""")

rep("A2-init", """    command->deferred_ts_ = nullptr;  // E6-v4b: never inherit a recycled emit slot
    command->graph_command_ = false;""",
"""    command->deferred_ts_ = nullptr;  // E6-v4b: never inherit a recycled emit slot
    command->bqkt_w_token_ = 0;       // tsbadfix: no W reference until staged
    command->bqkt_w_done_ = false;
    command->graph_command_ = false;""")

rep("A3-prepare-head", """    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    // Look up source graph info directly from executable graph""",
"""    // tsbadfix: fresh staging window for every replay. A previous window
    // whose submit failed or degraded (W-less) must not leak its token/refs
    // into this one.
    TlBqktStg() = BqktStaging{};

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    // Look up source graph info directly from executable graph""")

rep("A4-clone-attach", """      EventHistoryAddPending(cmd->event_);
      cmd->engine_ordinal_ = engine_ordinal;""",
"""      EventHistoryAddPending(cmd->event_);
      // tsbadfix: attach the staging token and count the live reference.
      // The W itself is pushed at OnExit (BatchQktInjectShadowW) with the
      // final reference count; between OnEnter and OnExit the token exists
      // without a pending W -- an arm in that window hits the A guard and
      // degrades to the drain read (fail-closed, never mid-flight).
      if (TlBqktStg().token == 0) {
        TlBqktStg().token = BqktTokenNext().fetch_add(1, std::memory_order_relaxed);
      }
      cmd->bqkt_w_token_ = TlBqktStg().token;
      cmd->bqkt_w_done_ = false;
      TlBqktStg().refs++;
      uni_batchqkt_clone_attached_.fetch_add(1, std::memory_order_relaxed);
      cmd->engine_ordinal_ = engine_ordinal;""")

rep("A5-arm-guard", """      for (auto &entry : uni_batchqkt_pending_ws_) {
        if (entry.first == pr.context) {
          shadow_ws.push_back(entry.second);
        }
      }
      if (shadow_ws.empty()) {
        uni_batchqkt_w_degrade_.fetch_add(1, std::memory_order_relaxed);
        return;
      }""",
"""      for (auto &entry : uni_batchqkt_pending_ws_) {
        if (entry.first == pr.context) {
          shadow_ws.push_back(entry.second);
        }
      }
      if (shadow_ws.empty()) {
        uni_batchqkt_w_degrade_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // tsbadfix A (fail-closed guard): every clone in the batch must carry a
      // token that still has a pending W for this context. A missing entry
      // means the clone's submission stranded (its W was released by an
      // unbalanced path, or the W push never happened): arming would park on
      // W's that do not gate this batch's clones and read them mid-flight
      // (the pre-fix ~86-TSBAD/round smoke loss). Degrade the WHOLE batch to
      // the drain's readiness-gated read -- never arm a partially-covered
      // batch, never silently skip single clones.
      size_t uncovered = 0;
      for (ZeCommand *command : pr.cmds) {
        if (command->bqkt_w_token_ == 0) {
          uncovered++;
          continue;
        }
        bool covered = false;
        for (auto &entry : uni_batchqkt_pending_ws_) {
          if (entry.first == pr.context && entry.token == command->bqkt_w_token_) {
            covered = true;
            break;
          }
        }
        if (!covered) {
          uncovered++;
        }
      }
      if (uncovered != 0) {
        uni_batchqkt_w_degrade_.fetch_add(1, std::memory_order_relaxed);
        if (!uni_batchqkt_notice_arm_guard_.exchange(true, std::memory_order_relaxed) &&
            BatchQktDiagEnabled()) {
          std::cerr << "[BATCHQKT] arm guard: " << uncovered << "/" << n
                    << " batch clones lack a pending W, degrading to the drain read"
                    << std::endl;
        }
        return;
      }""")

rep("A6-staging-members", """  // W's staged since the last drain of their context (the current window).
  // Written at the OnExit graph submit, read by the arm, released at the
  // drain/teardown -- all under uni_batchqkt_mutex_. Cleared per context by
  // BatchQktReleaseShadowWs; the drain runs before the next submission's W is
  // pushed, so the window correlation with the pending clones is by
  // construction.
  std::vector<std::pair<ze_context_handle_t, ze_event_handle_t>> uni_batchqkt_pending_ws_;""",
"""  // tsbadfix: per-thread staging window (OnEnter..OnExit of one graph
  // append): the token handed to every clone staged in the window and the
  // number of live references accumulated. Reset at the head of every
  // PrepareGraphExecution.
  struct BqktStaging {
    uint64_t token = 0;
    uint64_t refs = 0;
  };
  static BqktStaging &TlBqktStg(void) {
    static thread_local BqktStaging stg;
    return stg;
  }
  // Token allocator; 0 means "no token" (eager commands, un-staged clones).
  static std::atomic<uint64_t> &BqktTokenNext(void) {
    static std::atomic<uint64_t> next{1};
    return next;
  }
  // W's staged since the last drain of their context (the current window).
  // Written at the OnExit graph submit, read by the arm, released at the
  // drain/teardown -- all under uni_batchqkt_mutex_. Cleared per context by
  // BatchQktReleaseShadowWs; the drain runs before the next submission's W is
  // pushed, so the window correlation with the pending clones is by
  // construction.
  std::vector<std::pair<ze_context_handle_t, ze_event_handle_t>> uni_batchqkt_pending_ws_;""")

rep("A7-counters", """  std::atomic<bool> uni_batchqkt_notice_w_latched_{false};
  std::atomic<bool> uni_batchqkt_notice_w_ooo_{false};""",
"""  // tsbadfix: clone lifecycle counter (the attached total; completed and
  // abandoned arrive with the reference-closure commit).
  std::atomic<uint64_t> uni_batchqkt_clone_attached_{0};
  std::atomic<bool> uni_batchqkt_notice_arm_guard_{false};  // A-guard one-shot diag
  std::atomic<bool> uni_batchqkt_notice_w_latched_{false};
  std::atomic<bool> uni_batchqkt_notice_w_ooo_{false};""")

if fails:
    print("PATCHA-FAIL")
    for f in fails:
        print(" ", f)
    sys.exit(1)

open(P, "w").write(src)
print("PATCHA-OK bytes %d -> %d" % (len(orig), len(src)))
