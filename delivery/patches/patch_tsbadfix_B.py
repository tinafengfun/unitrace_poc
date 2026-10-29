#!/usr/bin/env python3
# patch_tsbadfix_B.py — tsbadfix COMMIT 2 (B 主修: per-submission W 引用闭环)
# Applies on top of the A-guard tree (patch_tsbadfix_A.py).
# Scope: pending_ws_ -> BqktPendingW {context,w,token,live_refs}; W release
# aligned with consumption (tail-funnel decref, revert/event-destroy abandon,
# terminal flag); the six unbalanced drain-side W release sites removed
# (device_ok=false abandon + context-invalidate + teardown backstop kept);
# summary counters extended with the closure invariants.
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

# --- B1: pending_ws_ type + invariant comment (replaces "by construction") ---
rep("B1-type", """  // W's staged since the last drain of their context (the current window).
  // Written at the OnExit graph submit, read by the arm, released at the
  // drain/teardown -- all under uni_batchqkt_mutex_. Cleared per context by
  // BatchQktReleaseShadowWs; the drain runs before the next submission's W is
  // pushed, so the window correlation with the pending clones is by
  // construction.
  std::vector<std::pair<ze_context_handle_t, ze_event_handle_t>> uni_batchqkt_pending_ws_;""",
"""  // tsbadfix B: one pending W entry per graph submission whose clones are
  // still being consumed. live_refs = clones of this submission's token not
  // yet terminated (completed via the tail funnel, or abandoned via
  // revert/event-destroy discard); the W is staged for destruction exactly
  // when live_refs reaches 0 -- release aligned with consumption, never
  // "when the drain ends" (the pre-fix unbalanced release). Written under
  // uni_batchqkt_mutex_ (push at the OnExit submit with the staging
  // window's final ref count; decremented by the tails). The window
  // correlation the arm relies on ("every batch clone's token has a pending
  // W") is now enforced by this refcount + the A guard, not assumed.
  struct BqktPendingW {
    ze_context_handle_t context;
    ze_event_handle_t w;
    uint64_t token;      // staging token of the submission this W gates
    uint64_t live_refs;  // clones of that token not yet terminated
  };
  std::vector<BqktPendingW> uni_batchqkt_pending_ws_;""")

# --- B2: arm wait-set build field rename ---
rep("B2-waitset", """      for (auto &entry : uni_batchqkt_pending_ws_) {
        if (entry.first == pr.context) {
          shadow_ws.push_back(entry.second);
        }
      }""",
"""      for (auto &entry : uni_batchqkt_pending_ws_) {
        if (entry.context == pr.context) {
          shadow_ws.push_back(entry.w);
        }
      }""")

rep("B3-guard-fields", """        for (auto &entry : uni_batchqkt_pending_ws_) {
          if (entry.first == pr.context && entry.token == command->bqkt_w_token_) {""",
"""        for (auto &entry : uni_batchqkt_pending_ws_) {
          if (entry.context == pr.context && entry.token == command->bqkt_w_token_) {""")

# --- B4: InjectShadowW push with refs (+ refs==0 staging, dead-branch note) ---
rep("B4-inject", """      // dead): abandon the W — never push a dead context's event into the
      // window, the arm could park on it.
      uni_batchqkt_w_abandoned_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    uni_batchqkt_pending_ws_.push_back({context, w});""",
"""      // dead): abandon the W — never push a dead context's event into the
      // window, the arm could park on it. The clones staged under this token
      // terminate through their own paths; their tails will decref a token
      // that is no longer pending (counters only, no device calls).
      uni_batchqkt_w_abandoned_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // tsbadfix B: the W gates exactly the clones staged in this replay's
    // window. A window with no clones (refs == 0: an early-returning prepare
    // or an empty graph) has nothing to gate -- stage the W for the pool
    // return at the next flush instead of pushing a zero-reference entry.
    if (TlBqktStg().refs == 0) {
      uni_batchqkt_ws_to_free_.push_back(w);
      return;
    }
    uni_batchqkt_pending_ws_.push_back({context, w, TlBqktStg().token, TlBqktStg().refs});""")

# --- B5: tail entry decref (terminal funnel) ---
rep("B5-tail-decref", """    ze_kernel_timestamp_result_t timestamp;
    bool bad_read = false;
    if (!on_event) {""",
"""    // tsbadfix B (path 1): this tail is the single terminal funnel for every
    // consumed clone (drain read, armed read, deferred emit, fence/event
    // sweeps -- every caller verified C3). Terminate the clone's W reference
    // here, BEFORE any early return: even a dropped (TSBAD) or query-failed
    // record consumed the clone, so its reference must go. completed = the
    // clone's record was accounted (emitted or fail-loud dropped).
    BatchQktCloneTerminate(command, /*completed=*/true);

    ze_kernel_timestamp_result_t timestamp;
    bool bad_read = false;
    if (!on_event) {""")

# --- B6: CloneTerminate + abandon helpers ---
rep("B6-helpers", """  // Public release: idempotent, callable from any sweep/teardown thread.
  // HARDEN (review): no lock-free early return on pending_ws_.empty() -- the""",
"""  // -----------------------------------------------------------------------
  // tsbadfix B: per-submission W reference closure. States (Tina's ruling):
  // Create W -> clone attached (staging, token+refs) -> clone completed
  // (tail funnel) / clone abandoned (revert, event-destroy discard, teardown)
  // -> live_refs == 0 -> release W (staged destruction, pool return at the
  // flush). The W stays pending while any of its clones is live -- that is
  // exactly the invariant the pre-fix code violated by releasing W's at the
  // drain while gate-skipped clones stranded across the release.
  // -----------------------------------------------------------------------

  // Terminal accounting of one clone's W reference. Idempotent via
  // bqkt_w_done_ (path 6: double completion / repeated callbacks decref
  // once). Lock order: caller holds the submission lock (any mode) -> we
  // take uni_batchqkt_mutex_ (same direction as every existing sweep; never
  // the reverse). Safe for W-less clones (token not pending: counters only).
  void BatchQktCloneTerminate(ZeCommand *command, bool completed) {
    if (command == nullptr || command->bqkt_w_token_ == 0 || command->bqkt_w_done_) {
      return;
    }
    command->bqkt_w_done_ = true;
    if (completed) {
      uni_batchqkt_clone_completed_.fetch_add(1, std::memory_order_relaxed);
    }
    else {
      uni_batchqkt_clone_abandoned_.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    for (auto it = uni_batchqkt_pending_ws_.begin(); it != uni_batchqkt_pending_ws_.end(); ++it) {
      if (it->context == command->context_ && it->token == command->bqkt_w_token_) {
        if (--it->live_refs == 0) {
          // The submission's last live clone terminated: the W is done.
          // Staged destruction (see BatchQktReleaseShadowWsLocked) -- the
          // pool return runs in BatchQktFreeStagedShadowWs at the flush,
          // outside the submission locks.
          uni_batchqkt_ws_to_free_.push_back(it->w);
          uni_batchqkt_pending_ws_.erase(it);
        }
        break;
      }
    }
  }

  // Path 5 (partial-build rollback): the caller is about to revert every
  // staged clone back to the pool -- terminate their W references first.
  // Same thread that staged them (the thread-local list is unguarded here by
  // design, exactly like the Revert that follows).
  void BatchQktAbandonStagedClones(void) {
    for (ZeCommand *cmd : local_device_submissions_.commands_staged_) {
      if (cmd != nullptr) {
        BatchQktCloneTerminate(cmd, /*completed=*/false);
      }
    }
  }

  // Path 2 (event-destroy discard, staged subset): abandon only the staged
  // clones bound to the event being destroyed.
  void BatchQktAbandonStagedClonesForEvent(ze_event_handle_t event) {
    for (ZeCommand *cmd : local_device_submissions_.commands_staged_) {
      if (cmd != nullptr && cmd->event_ == event) {
        BatchQktCloneTerminate(cmd, /*completed=*/false);
      }
    }
  }

  // Public release: idempotent, callable from any sweep/teardown thread.
  // HARDEN (review): no lock-free early return on pending_ws_.empty() -- the""")

# --- B7: Execute release-site removals (6 sites) ---
rep("B7a-mixed-ctx", """        batch.cmds.clear();
        BatchQktReleaseShadowWs(context, /*device_ok=*/true);
        return false;
      }
      // HARDEN: a null event handle must never reach""",
"""        batch.cmds.clear();
        return false;
      }
      // HARDEN: a null event handle must never reach""")

rep("B7b-null-event", """        uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
        batch.cmds.clear();
        BatchQktReleaseShadowWs(context, /*device_ok=*/true);
        return false;""",
"""        uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
        batch.cmds.clear();
        return false;""")

rep("B7c-capture-hold", """      uni_batchqkt_capture_skip_.fetch_add(1, std::memory_order_relaxed);
      batch.cmds.clear();
      BatchQktReleaseShadowWs(context, /*device_ok=*/true);
      return false;""",
"""      uni_batchqkt_capture_skip_.fetch_add(1, std::memory_order_relaxed);
      batch.cmds.clear();
      return false;""")

rep("B7d-alloc-fail", """      batch.cmds.clear();
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      BatchQktReleaseShadowWsLocked(context, /*device_ok=*/true);
      return false;""",
"""      batch.cmds.clear();
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      return false;""")

rep("B7e-append-fail", """      batch.cmds.clear();
      // BQ-S2: no device op of this read referenced the W's (the drain read
      // waits on nothing), so they can be returned to the pool even here.
      BatchQktReleaseShadowWsLocked(context, /*device_ok=*/true);
      return false;""",
"""      batch.cmds.clear();
      // BQ-S2: no device op of this read referenced the W's. tsbadfix B: the
      // W's stay pending -- their clones were NOT consumed by the failed
      // read (the drain falls back to the per-event path for the same
      // clones), so releasing here would strand them without a gate; the
      // tails terminate the references instead.""")

rep("B7f-success", """    // BQ-S2: the drain read consumed this window's packets — release the W's.
    BatchQktReleaseShadowWsLocked(context, /*device_ok=*/true);""",
"""    // BQ-S2: the drain read consumed this window's packets. tsbadfix B: the
    // W's are NOT released here -- the consuming loop below runs the tail of
    // every batched clone, and each tail decrements its submission's W
    // reference; the W is staged for destruction when its last clone is
    // done (release aligned with consumption, not with the read).""")

# --- B8: CompletePollRead release removals (KEEP the device_ok=false site) ---
rep("B8a-desync", """      uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
      // BQ-S2: the armed read never ran — release this context's W's safely.
      BatchQktReleaseShadowWsLocked(pr.context, /*device_ok=*/true);
      if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&""",
"""      uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
      // BQ-S2: the armed read never ran. tsbadfix B: the W's stay pending --
      // their clones are still live and were not consumed (the caller falls
      // back to the drain read for the same clones).

      if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&""")

rep("B8b-null-elem", """        batch.cmds.clear();
        batch.ts.clear();
        // BQ-S2: the armed read never ran — release this context's W's safely.
        BatchQktReleaseShadowWsLocked(pr.context, /*device_ok=*/true);
        if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&""",
"""        batch.cmds.clear();
        batch.ts.clear();
        // BQ-S2: the armed read never ran. tsbadfix B: the W's stay pending
        // -- their clones are still live and were not consumed.
        if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&""")

rep("B8c-success", """    // BQ-S2: the read resolved (the wait on the shadow W's completed), so the
    // W's are done — host-destroy/recreate them back into the pool.
    BatchQktReleaseShadowWsLocked(pr.context, /*device_ok=*/true);
    return true;""",
"""    // BQ-S2: the read resolved (the wait on the shadow W's completed).
    // tsbadfix B: the W's are NOT released here -- the caller's consuming
    // loop runs the tail of every batched clone; each tail decrements its
    // submission's W reference, and the W is staged for destruction when its
    // last clone is done (release aligned with consumption).
    return true;""")

# --- B9: release/destroy/backstop field renames ---
rep("B9a-release-locked", """    auto it = uni_batchqkt_pending_ws_.begin();
    while (it != uni_batchqkt_pending_ws_.end()) {
      if (it->first == context) {
        if (device_ok) {
          uni_batchqkt_ws_to_free_.push_back(it->second);
        }""",
"""    auto it = uni_batchqkt_pending_ws_.begin();
    while (it != uni_batchqkt_pending_ws_.end()) {
      if (it->context == context) {
        if (device_ok) {
          uni_batchqkt_ws_to_free_.push_back(it->w);
        }""")

rep("B9b-destroy-locked", """    std::vector<ze_event_handle_t> dead;
    auto it = uni_batchqkt_pending_ws_.begin();
    while (it != uni_batchqkt_pending_ws_.end()) {
      if (it->first == context) {
        dead.push_back(it->second);
        it = uni_batchqkt_pending_ws_.erase(it);""",
"""    std::vector<ze_event_handle_t> dead;
    auto it = uni_batchqkt_pending_ws_.begin();
    while (it != uni_batchqkt_pending_ws_.end()) {
      if (it->context == context) {
        dead.push_back(it->w);
        it = uni_batchqkt_pending_ws_.erase(it);""")

rep("B9c-backstop", """    for (auto &entry : uni_batchqkt_pending_ws_) {
      uni_batchqkt_ws_to_free_.push_back(entry.second);
    }""",
"""    for (auto &entry : uni_batchqkt_pending_ws_) {
      uni_batchqkt_ws_to_free_.push_back(entry.w);
    }""")

# --- B10: revert/event-destroy abandon call sites ---
rep("B10a-queue-execute-fail", """    else {
      local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
    }
  }

  static void OnExitCommandQueueSynchronize(""",
"""    else {
      // tsbadfix B (path 5): the staged clones are rolled back -- terminate
      // their W references before they return to the pool (no tail will run).
      reinterpret_cast<ZeCollector*>(global_data)->BatchQktAbandonStagedClones();
      local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
    }
  }

  static void OnExitCommandQueueSynchronize(""")

rep("B10b-immediate-append-fail", """        else {
           local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
        }""",
"""        else {
           // tsbadfix B (path 5): rollback -- terminate the W references.
           collector->BatchQktAbandonStagedClones();
           local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
        }""")

rep("B10c-gexp-submit-fail", """    if (result != ZE_RESULT_SUCCESS || !UniController::IsCollectionEnabled()) {
      local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
      return;
    }""",
"""    if (result != ZE_RESULT_SUCCESS || !UniController::IsCollectionEnabled()) {
      // tsbadfix B (path 5): rollback -- terminate the staged clones' W
      // references before they return to the pool.
      reinterpret_cast<ZeCollector*>(global_data)->BatchQktAbandonStagedClones();
      local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
      return;
    }""")

rep("B10d-event-destroy-staged", """        // discard event associated staged commands
        local_device_submissions_.RevertStagedKernelCommandAndMetricQueriesForEvent(*(params->phEvent));""",
"""        // discard event associated staged commands
        // tsbadfix B (path 2): abandon the staged clones' W references first.
        collector->BatchQktAbandonStagedClonesForEvent(*(params->phEvent));
        local_device_submissions_.RevertStagedKernelCommandAndMetricQueriesForEvent(*(params->phEvent));""")

rep("B10e-event-destroy-submitted", """          if (command->event_ == *(params->phEvent)) {
            std::cerr << "[INFO] Remove command from submitted commands on event destroy" << std::endl;
            local_device_submissions_.commands_free_pool_.push_back(command);""",
"""          if (command->event_ == *(params->phEvent)) {
            std::cerr << "[INFO] Remove command from submitted commands on event destroy" << std::endl;
            // tsbadfix B (path 2): abandon this clone's W reference.
            collector->BatchQktCloneTerminate(command, /*completed=*/false);
            local_device_submissions_.commands_free_pool_.push_back(command);""")

# --- B11: summary counters ---
rep("B11-summary", """              << " w_released=" << uni_batchqkt_w_released_.load(std::memory_order_relaxed)
              << " w_abandoned=" << uni_batchqkt_w_abandoned_.load(std::memory_order_relaxed)
              << std::endl;""",
"""              << " w_released=" << uni_batchqkt_w_released_.load(std::memory_order_relaxed)
              << " w_abandoned=" << uni_batchqkt_w_abandoned_.load(std::memory_order_relaxed)
              // tsbadfix B: W/clone lifecycle closure. Invariants (any
              // summary): clone_attached == clone_completed +
              // clone_abandoned; after teardown w_pending == 0 and
              // clone_live == 0; w_degrade > 0 on the healthy path = the A
              // guard fired (fail-closed, never silent). w_pops ==
              // w_released + w_abandoned at teardown.
              << " clone_attached=" << uni_batchqkt_clone_attached_.load(std::memory_order_relaxed)
              << " clone_completed=" << uni_batchqkt_clone_completed_.load(std::memory_order_relaxed)
              << " clone_abandoned=" << uni_batchqkt_clone_abandoned_.load(std::memory_order_relaxed)
              << " clone_live=" << (uni_batchqkt_clone_attached_.load(std::memory_order_relaxed) -
                                    uni_batchqkt_clone_completed_.load(std::memory_order_relaxed) -
                                    uni_batchqkt_clone_abandoned_.load(std::memory_order_relaxed))
              << " w_pending=" << uni_batchqkt_pending_ws_.size()
              << std::endl;""")

# --- B12: remaining counters ---
rep("B12-counters", """  // tsbadfix: clone lifecycle counter (the attached total; completed and
  // abandoned arrive with the reference-closure commit).
  std::atomic<uint64_t> uni_batchqkt_clone_attached_{0};
  std::atomic<bool> uni_batchqkt_notice_arm_guard_{false};  // A-guard one-shot diag""",
"""  // tsbadfix B: clone/W lifecycle counters (see the [BATCHQKT] summary line
  // for the invariants). clone_attached arrives with the A-guard commit; the
  // terminal counters belong to the reference-closure commit.
  std::atomic<uint64_t> uni_batchqkt_clone_attached_{0};
  std::atomic<uint64_t> uni_batchqkt_clone_completed_{0};
  std::atomic<uint64_t> uni_batchqkt_clone_abandoned_{0};
  std::atomic<bool> uni_batchqkt_notice_arm_guard_{false};  // A-guard one-shot diag""")

if fails:
    print("PATCHB-FAIL")
    for f in fails:
        print(" ", f)
    sys.exit(1)

open(P, "w").write(src)
print("PATCHB-OK bytes %d -> %d" % (len(orig), len(src)))
