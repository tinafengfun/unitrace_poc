//==============================================================
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#ifndef PTI_TOOLS_UNITRACE_LEVEL_ZERO_COLLECTOR_H_
#define PTI_TOOLS_UNITRACE_LEVEL_ZERO_COLLECTOR_H_

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cstdlib>
#include <thread>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <set>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#if !defined(_WIN32) && (defined(__gnu_linux__) || defined(__unix__))
#include <dlfcn.h>
#endif /* !defined(_WIN32) && (defined(__gnu_linux__) || defined(__unix__)) */

#include <level_zero/ze_api.h>
#include <level_zero/layers/zel_tracing_api.h>
#include <level_zero/layers/zel_tracing_register_cb.h>

#include "utils.h"
#include "ze_event_cache.h"
#include "utils_ze.h"
#include "collector_options.h"
#include "unikernel.h"
#include "unitimer.h"
#include "unicontrol.h"
#include "unimemory.h"

#include "ze_loader.h"
#include "common_header.gen"
#include "logger_factory.h"

struct ZeMetricQueryPoolKey {
  ze_context_handle_t context_;
  ze_device_handle_t device_;
  zet_metric_group_handle_t group_;
};

struct ZeMetricQueryPoolKeyCompare {
  bool operator()(const ZeMetricQueryPoolKey& lhs, const ZeMetricQueryPoolKey& rhs) const {
    if (lhs.context_ < rhs.context_) {
      return true;
    }
    if (lhs.context_ == rhs.context_) {
      if (lhs.device_ < rhs.device_) {
        return true;
      }
      if (lhs.device_ == rhs.device_) {
        return (lhs.group_ < rhs.group_);
      }
    }
    return false;
  }
};

struct ZeMetricQueryPools {
  // The pool size was reduced from 128 to 64 to optimize memory usage
  // and align with typical workload requirements, ensuring efficient
  // resource utilization without compromising performance.
  constexpr static uint32_t pool_size_ = 64;
  std::mutex query_pool_mutex_;
  std::map<zet_metric_query_handle_t, ZeMetricQueryPoolKey> query_pool_map_;
  std::map<ZeMetricQueryPoolKey, std::vector<zet_metric_query_handle_t>, ZeMetricQueryPoolKeyCompare> free_pool_;
  std::vector<zet_metric_query_pool_handle_t> pools_;

  ZeMetricQueryPools() {}

  ZeMetricQueryPools(const struct ZeMetricQueryPools& that) = delete;

  ZeMetricQueryPools& operator=(const struct ZeMetricQueryPools& that) = delete;

  ~ZeMetricQueryPools() {
#ifdef _WIN32
    // on Windows, it is very possible that L0 has been unloaded or is being unloaded at this point and L0 calls may have undefined behavior
    // hence skipping all destroy calls and returning early.
    return;
#else /* _WIN32 */
    ze_result_t status;

    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    for (auto it = query_pool_map_.begin(); it != query_pool_map_.end(); it++) {
      status = ZE_FUNC(zetMetricQueryDestroy)(it->first);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[WARNING] Failed to destroy metric query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
      }
    }
    query_pool_map_.clear();

    for (auto it = pools_.begin(); it != pools_.end(); it++) {
      status = ZE_FUNC(zetMetricQueryPoolDestroy)(*it);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[WARNING] Failed to destroy metric query pool (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
      }
    }

    pools_.clear();

    free_pool_.clear();
#endif /* _WIN32 */
  }

  zet_metric_query_handle_t
  GetQuery(ze_context_handle_t context, ze_device_handle_t device, zet_metric_group_handle_t group) {
    ze_result_t status;
    zet_metric_query_handle_t query;

    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    auto it = free_pool_.find({context, device, group});
    if (it == free_pool_.end()) {
      // no pools created

      zet_metric_query_pool_desc_t desc = {ZET_STRUCTURE_TYPE_METRIC_QUERY_POOL_DESC, nullptr, ZET_METRIC_QUERY_POOL_TYPE_PERFORMANCE, pool_size_};
      zet_metric_query_pool_handle_t pool;

      status = ZE_FUNC(zetMetricQueryPoolCreate)(context, device, group, &desc, &pool);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[ERROR] Failed to create metric query pool (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
        _Exit(-1);  // immediately exit
      }
      pools_.push_back(pool);

      std::vector<zet_metric_query_handle_t> queries;
      for (uint32_t i = 0; i < pool_size_ - 1; i++) {
        status = ZE_FUNC(zetMetricQueryCreate)(pool, i, &query);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[ERROR] Failed to create metric query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          _Exit(-1);  // exit immediately
        }
        queries.push_back(query);
        query_pool_map_.insert({query, {context, device, group}});
      }
      status = ZE_FUNC(zetMetricQueryCreate)(pool, pool_size_ - 1, &query);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[ERROR] Failed to create metric query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
        _Exit(-1);  // exit immediately
      }
      query_pool_map_.insert({query, {context, device, group}});

      free_pool_.insert({{context, device, group}, std::move(queries)});
    }
    else {
      if (it->second.size() == 0) {
        // no free queries, create a new pool

        zet_metric_query_pool_desc_t desc = {ZET_STRUCTURE_TYPE_METRIC_QUERY_POOL_DESC, nullptr, ZET_METRIC_QUERY_POOL_TYPE_PERFORMANCE, pool_size_};
        zet_metric_query_pool_handle_t pool;

        status = ZE_FUNC(zetMetricQueryPoolCreate)(context, device, group, &desc, &pool);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[ERROR] Failed to create metric query pool (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          _Exit(-1);  // immediately exit
        }
        pools_.push_back(pool);

        for (uint32_t i = 0; i < pool_size_ - 1; i++) {
          status = ZE_FUNC(zetMetricQueryCreate)(pool, i, &query);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Failed to create metric query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
            _Exit(-1);  // exit immediately
          }
          it->second.push_back(query);
          query_pool_map_.insert({query, {context, device, group}});
        }
        status = ZE_FUNC(zetMetricQueryCreate)(pool, pool_size_ - 1, &query);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[ERROR] Failed to create metric query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          _Exit(-1);  // exit immediately
        }
        query_pool_map_.insert({query, {context, device, group}});
      }
      else {
        query = it->second.back();
        it->second.pop_back();
      }
    }

    return query;
  }

  void
  PutQuery(zet_metric_query_handle_t query) {
    if (query == nullptr) {
      return;
    }

    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    auto it = query_pool_map_.find(query);
    if (it == query_pool_map_.end()) {
      return;
    }
    auto it2 = free_pool_.find(it->second);
    PTI_ASSERT(it2 != free_pool_.end());
    it2->second.push_back(query);
  }

  void
  ResetQuery(zet_metric_query_handle_t query) {
    const std::lock_guard<std::mutex> lock(query_pool_mutex_);
    if (query_pool_map_.find(query) == query_pool_map_.end()) {
      return;
    }
    ze_result_t status = ZE_FUNC(zetMetricQueryReset)(query);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[ERROR] Failed to reset metric query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
      _Exit(-1);  // exit immediately
    }
  }
};

struct ZeInstanceData {
  uint64_t start_time_host;  // in ns
  uint64_t timestamp_host;  // in ns
  uint64_t timestamp_device;  // in ticks
  uint64_t kid;  // passing kid from enter callback to exit callback

  // These used in Append commands
  zet_metric_query_handle_t query_; // Appended command query handle
  ze_event_handle_t in_order_counter_event_;  // Appended command event counter based event or null
  uint64_t signal_gen_;             // Signal generation of the signal event, snapshot for TSLOG2
  bool instrument_;                 // false if command should be skipped
};

thread_local ZeInstanceData ze_instance_data;

// TSLOG v2: per-event signal/reset history for detecting "signal state vs
// packet content" desynchronization. Covers collector-owned AND app-owned
// events (graph replay re-signals the app's physical events).
// Signal semantics: gen increments on every signal registration (append or
// graph replay clone); a reset zeroes gen. At query time:
//   gen == 0                 -> event was reset after its signal (stale/erased packet candidate)
//   gen > command snapshot   -> event was re-signaled by a newer command (reuse race)
//   last_signal_inst != expected -> reset+re-signal cycle happened in between
struct ZeEventHistory {
  uint64_t gen;               // current signal generation (0 = reset/never signaled)
  uint64_t signal_count;      // total signal registrations since first seen
  uint64_t reset_count;       // total resets since first seen
  uint64_t last_signal_path;  // 0 = kernel append, 1 = graph replay
  uint64_t last_signal_inst;  // instance_id of the command expected to signal
  uint64_t pending_clones;    // Graph replay fix v3: clones appended, not yet processed
};

struct ZeFunctionTime {
  uint64_t total_time_;
  uint64_t min_time_;
  uint64_t max_time_;
  uint64_t call_count_;

  bool operator>(const ZeFunctionTime& r) const {
    if (total_time_ != r.total_time_) {
      return total_time_ > r.total_time_;
    }
    return call_count_ > r.call_count_;
  }

  bool operator!=(const ZeFunctionTime& r) const {
    if (total_time_ == r.total_time_) {
      return call_count_ != r.call_count_;
    }
    return true;
  }
};

struct ZeKernelGroupSize {
  uint32_t x;
  uint32_t y;
  uint32_t z;
};

enum ZeKernelCommandType {
  KERNEL_COMMAND_TYPE_INVALID = 0,
  KERNEL_COMMAND_TYPE_COMPUTE = 1,
  KERNEL_COMMAND_TYPE_MEMORY = 2,
  KERNEL_COMMAND_TYPE_COMMAND = 3
};

enum ZeDeviceCommandHandle {
  MemoryCopy = 0,
  MemoryCopyH2H = MemoryCopy,
  MemoryCopyH2D,
  MemoryCopyH2M,
  MemoryCopyH2S,
  MemoryCopyD2H,
  MemoryCopyD2D,
  MemoryCopyD2M,
  MemoryCopyD2S,
  MemoryCopyM2H,
  MemoryCopyM2D,
  MemoryCopyM2M,
  MemoryCopyM2S,
  MemoryCopyS2H,
  MemoryCopyS2D,
  MemoryCopyS2M,
  MemoryCopyS2S,
  MemoryCopyRegion,
  MemoryCopyRegionH2H = MemoryCopyRegion,
  MemoryCopyRegionH2D,
  MemoryCopyRegionH2M,
  MemoryCopyRegionH2S,
  MemoryCopyRegionD2H,
  MemoryCopyRegionD2D,
  MemoryCopyRegionD2M,
  MemoryCopyRegionD2S,
  MemoryCopyRegionM2H,
  MemoryCopyRegionM2D,
  MemoryCopyRegionM2M,
  MemoryCopyRegionM2S,
  MemoryCopyRegionS2H,
  MemoryCopyRegionS2D,
  MemoryCopyRegionS2M,
  MemoryCopyRegionS2S,
  MemoryCopyFromContext,
  MemoryCopyFromContextH2H = MemoryCopyFromContext,
  MemoryCopyFromContextH2D,
  MemoryCopyFromContextH2M,
  MemoryCopyFromContextH2S,
  MemoryCopyFromContextD2H,
  MemoryCopyFromContextD2D,
  MemoryCopyFromContextD2M,
  MemoryCopyFromContextD2S,
  MemoryCopyFromContextM2H,
  MemoryCopyFromContextM2D,
  MemoryCopyFromContextM2M,
  MemoryCopyFromContextM2S,
  MemoryCopyFromContextS2H,
  MemoryCopyFromContextS2D,
  MemoryCopyFromContextS2M,
  MemoryCopyFromContextS2S,
  ImageCopy,
  ImageCopyH2H = ImageCopy,
  ImageCopyH2D,
  ImageCopyH2M,
  ImageCopyH2S,
  ImageCopyD2H,
  ImageCopyD2D,
  ImageCopyD2M,
  ImageCopyD2S,
  ImageCopyM2H,
  ImageCopyM2D,
  ImageCopyM2M,
  ImageCopyM2S,
  ImageCopyS2H,
  ImageCopyS2D,
  ImageCopyS2M,
  ImageCopyS2S,
  ImageCopyRegion,
  ImageCopyRegionH2H = ImageCopyRegion,
  ImageCopyRegionH2D,
  ImageCopyRegionH2M,
  ImageCopyRegionH2S,
  ImageCopyRegionD2H,
  ImageCopyRegionD2D,
  ImageCopyRegionD2M,
  ImageCopyRegionD2S,
  ImageCopyRegionM2H,
  ImageCopyRegionM2D,
  ImageCopyRegionM2M,
  ImageCopyRegionM2S,
  ImageCopyRegionS2H,
  ImageCopyRegionS2D,
  ImageCopyRegionS2M,
  ImageCopyRegionS2S,
  ImageCopyFromMemory,
  ImageCopyFromMemoryH2H = ImageCopyFromMemory,
  ImageCopyFromMemoryH2D,
  ImageCopyFromMemoryH2M,
  ImageCopyFromMemoryH2S,
  ImageCopyFromMemoryD2H,
  ImageCopyFromMemoryD2D,
  ImageCopyFromMemoryD2M,
  ImageCopyFromMemoryD2S,
  ImageCopyFromMemoryM2H,
  ImageCopyFromMemoryM2D,
  ImageCopyFromMemoryM2M,
  ImageCopyFromMemoryM2S,
  ImageCopyFromMemoryS2H,
  ImageCopyFromMemoryS2D,
  ImageCopyFromMemoryS2M,
  ImageCopyFromMemoryS2S,
  ImageCopyToMemory,
  ImageCopyToMemoryH2H = ImageCopyToMemory,
  ImageCopyToMemoryH2D,
  ImageCopyToMemoryH2M,
  ImageCopyToMemoryH2S,
  ImageCopyToMemoryD2H,
  ImageCopyToMemoryD2D,
  ImageCopyToMemoryD2M,
  ImageCopyToMemoryD2S,
  ImageCopyToMemoryM2H,
  ImageCopyToMemoryM2D,
  ImageCopyToMemoryM2M,
  ImageCopyToMemoryM2S,
  ImageCopyToMemoryS2H,
  ImageCopyToMemoryS2D,
  ImageCopyToMemoryS2M,
  ImageCopyToMemoryS2S,
  MemoryFill,
  MemoryFillH = MemoryFill,
  MemoryFillD,
  MemoryFillM,
  MemoryFillS,
  Barrier,
  MemoryRangesBarrier,
  EventReset,
  LastCommand = EventReset
};

static const char *device_command_names[] = {
  "zeCommandListAppendMemoryCopy(H2H)",
  "zeCommandListAppendMemoryCopy(H2D)",
  "zeCommandListAppendMemoryCopy(H2M)",
  "zeCommandListAppendMemoryCopy(H2S)",
  "zeCommandListAppendMemoryCopy(D2H)",
  "zeCommandListAppendMemoryCopy(D2D)",
  "zeCommandListAppendMemoryCopy(D2M)",
  "zeCommandListAppendMemoryCopy(D2S)",
  "zeCommandListAppendMemoryCopy(M2H)",
  "zeCommandListAppendMemoryCopy(M2D)",
  "zeCommandListAppendMemoryCopy(M2M)",
  "zeCommandListAppendMemoryCopy(M2S)",
  "zeCommandListAppendMemoryCopy(S2H)",
  "zeCommandListAppendMemoryCopy(S2D)",
  "zeCommandListAppendMemoryCopy(S2M)",
  "zeCommandListAppendMemoryCopy(S2S)",
  "zeCommandListAppendMemoryCopyRegion(H2H)",
  "zeCommandListAppendMemoryCopyRegion(H2D)",
  "zeCommandListAppendMemoryCopyRegion(H2M)",
  "zeCommandListAppendMemoryCopyRegion(H2S)",
  "zeCommandListAppendMemoryCopyRegion(D2H)",
  "zeCommandListAppendMemoryCopyRegion(D2D)",
  "zeCommandListAppendMemoryCopyRegion(D2M)",
  "zeCommandListAppendMemoryCopyRegion(D2S)",
  "zeCommandListAppendMemoryCopyRegion(M2H)",
  "zeCommandListAppendMemoryCopyRegion(M2D)",
  "zeCommandListAppendMemoryCopyRegion(M2M)",
  "zeCommandListAppendMemoryCopyRegion(M2S)",
  "zeCommandListAppendMemoryCopyRegion(S2H)",
  "zeCommandListAppendMemoryCopyRegion(S2D)",
  "zeCommandListAppendMemoryCopyRegion(S2M)",
  "zeCommandListAppendMemoryCopyRegion(S2S)",
  "zeCommandListAppendMemoryCopyFromContext(H2H)",
  "zeCommandListAppendMemoryCopyFromContext(H2D)",
  "zeCommandListAppendMemoryCopyFromContext(H2M)",
  "zeCommandListAppendMemoryCopyFromContext(H2S)",
  "zeCommandListAppendMemoryCopyFromContext(D2H)",
  "zeCommandListAppendMemoryCopyFromContext(D2D)",
  "zeCommandListAppendMemoryCopyFromContext(D2M)",
  "zeCommandListAppendMemoryCopyFromContext(D2S)",
  "zeCommandListAppendMemoryCopyFromContext(M2H)",
  "zeCommandListAppendMemoryCopyFromContext(M2D)",
  "zeCommandListAppendMemoryCopyFromContext(M2M)",
  "zeCommandListAppendMemoryCopyFromContext(M2S)",
  "zeCommandListAppendMemoryCopyFromContext(S2H)",
  "zeCommandListAppendMemoryCopyFromContext(S2D)",
  "zeCommandListAppendMemoryCopyFromContext(S2M)",
  "zeCommandListAppendMemoryCopyFromContext(S2S)",
  "zeCommandListAppendImageCopy(H2H)",
  "zeCommandListAppendImageCopy(H2D)",
  "zeCommandListAppendImageCopy(H2M)",
  "zeCommandListAppendImageCopy(H2S)",
  "zeCommandListAppendImageCopy(D2H)",
  "zeCommandListAppendImageCopy(D2D)",
  "zeCommandListAppendImageCopy(D2M)",
  "zeCommandListAppendImageCopy(D2S)",
  "zeCommandListAppendImageCopy(M2H)",
  "zeCommandListAppendImageCopy(M2D)",
  "zeCommandListAppendImageCopy(M2M)",
  "zeCommandListAppendImageCopy(M2S)",
  "zeCommandListAppendImageCopy(S2H)",
  "zeCommandListAppendImageCopy(S2D)",
  "zeCommandListAppendImageCopy(S2M)",
  "zeCommandListAppendImageCopy(S2S)",
  "zeCommandListAppendImageCopyRegion(H2H)",
  "zeCommandListAppendImageCopyRegion(H2D)",
  "zeCommandListAppendImageCopyRegion(H2M)",
  "zeCommandListAppendImageCopyRegion(H2S)",
  "zeCommandListAppendImageCopyRegion(D2H)",
  "zeCommandListAppendImageCopyRegion(D2D)",
  "zeCommandListAppendImageCopyRegion(D2M)",
  "zeCommandListAppendImageCopyRegion(D2S)",
  "zeCommandListAppendImageCopyRegion(M2H)",
  "zeCommandListAppendImageCopyRegion(M2D)",
  "zeCommandListAppendImageCopyRegion(M2M)",
  "zeCommandListAppendImageCopyRegion(M2S)",
  "zeCommandListAppendImageCopyRegion(S2H)",
  "zeCommandListAppendImageCopyRegion(S2D)",
  "zeCommandListAppendImageCopyRegion(S2M)",
  "zeCommandListAppendImageCopyRegion(S2S)",
  "zeCommandListAppendImageCopyFromMemory(H2H)",
  "zeCommandListAppendImageCopyFromMemory(H2D)",
  "zeCommandListAppendImageCopyFromMemory(H2M)",
  "zeCommandListAppendImageCopyFromMemory(H2S)",
  "zeCommandListAppendImageCopyFromMemory(D2H)",
  "zeCommandListAppendImageCopyFromMemory(D2D)",
  "zeCommandListAppendImageCopyFromMemory(D2M)",
  "zeCommandListAppendImageCopyFromMemory(D2S)",
  "zeCommandListAppendImageCopyFromMemory(M2H)",
  "zeCommandListAppendImageCopyFromMemory(M2D)",
  "zeCommandListAppendImageCopyFromMemory(M2M)",
  "zeCommandListAppendImageCopyFromMemory(M2S)",
  "zeCommandListAppendImageCopyFromMemory(S2H)",
  "zeCommandListAppendImageCopyFromMemory(S2D)",
  "zeCommandListAppendImageCopyFromMemory(S2M)",
  "zeCommandListAppendImageCopyFromMemory(S2S)",
  "zeCommandListAppendImageCopyToMemory(H2H)",
  "zeCommandListAppendImageCopyToMemory(H2D)",
  "zeCommandListAppendImageCopyToMemory(H2M)",
  "zeCommandListAppendImageCopyToMemory(H2S)",
  "zeCommandListAppendImageCopyToMemory(D2H)",
  "zeCommandListAppendImageCopyToMemory(D2D)",
  "zeCommandListAppendImageCopyToMemory(D2M)",
  "zeCommandListAppendImageCopyToMemory(D2S)",
  "zeCommandListAppendImageCopyToMemory(M2H)",
  "zeCommandListAppendImageCopyToMemory(M2D)",
  "zeCommandListAppendImageCopyToMemory(M2M)",
  "zeCommandListAppendImageCopyToMemory(M2S)",
  "zeCommandListAppendImageCopyToMemory(S2H)",
  "zeCommandListAppendImageCopyToMemory(S2D)",
  "zeCommandListAppendImageCopyToMemory(S2M)",
  "zeCommandListAppendImageCopyToMemory(S2S)",
  "zeCommandListAppendMemoryFill(H)",
  "zeCommandListAppendMemoryFill(D)",
  "zeCommandListAppendMemoryFill(M)",
  "zeCommandListAppendMemoryFill(S)",
  "zeCommandListAppendBarrier",
  "zeCommandListAppendMemoryRangesBarrier",
  "zeCommandListAppendEventReset"
};

struct ZeKernelCommandTime {
  uint64_t append_time_;
  uint64_t submit_time_;
  uint64_t execute_time_;
  uint64_t min_time_;
  uint64_t max_time_;
  uint64_t call_count_;

  bool operator>(const ZeKernelCommandTime& r) const {
    if (execute_time_ != r.execute_time_) {
      return execute_time_ > r.execute_time_;
    }
    return call_count_ > r.call_count_;
  }

  bool operator!=(const ZeKernelCommandTime& r) const {
    if (execute_time_ == r.execute_time_) {
      return call_count_ != r.call_count_;
    }
    return true;
  }
};

struct ZeKernelCommandNameKey {
  uint64_t kernel_command_id_;
  uint64_t mem_size_;
  int tile_;
  ze_group_count_t group_count_;

  bool operator>(const ZeKernelCommandNameKey& r) const {
    if (kernel_command_id_ != r.kernel_command_id_) {
      return kernel_command_id_ > r.kernel_command_id_;
    }
    if (mem_size_ != r.mem_size_) {
      return mem_size_ > r.mem_size_;
    }
    if (tile_ != r.tile_) {
      return tile_ > r.tile_;
    }

    if (group_count_.groupCountX != r.group_count_.groupCountX) {
      return (group_count_.groupCountX > r.group_count_.groupCountX);
    }

    if (group_count_.groupCountY != r.group_count_.groupCountY) {
      return (group_count_.groupCountY > r.group_count_.groupCountY);
    }

    return (group_count_.groupCountZ > r.group_count_.groupCountZ);
  }

  bool operator!=(const ZeKernelCommandNameKey& r) const {
    if (kernel_command_id_ == r.kernel_command_id_) {
      if (mem_size_ == r.mem_size_) {
        if (tile_ == r.tile_) {
          return ((group_count_.groupCountX != r.group_count_.groupCountX) ||
              (group_count_.groupCountY != r.group_count_.groupCountY) || (group_count_.groupCountZ != r.group_count_.groupCountZ));
        }
      }
    }

    return true;
  }
};

struct ZeKernelCommandNameKeyCompare {
  bool operator()(const ZeKernelCommandNameKey& lhs, const ZeKernelCommandNameKey& rhs) const {
    if (lhs.kernel_command_id_ != rhs.kernel_command_id_) {
      return (lhs.kernel_command_id_ < rhs.kernel_command_id_);
    }
    if (lhs.mem_size_ != rhs.mem_size_) {
      return (lhs.mem_size_ < rhs.mem_size_);
    }
    if (lhs.tile_ != rhs.tile_) {
      return (lhs.tile_ < rhs.tile_);
    }
    if (lhs.group_count_.groupCountX != rhs.group_count_.groupCountX) {
      return (lhs.group_count_.groupCountX < rhs.group_count_.groupCountX);
    }
    if (lhs.group_count_.groupCountY != rhs.group_count_.groupCountY) {
      return (lhs.group_count_.groupCountY < rhs.group_count_.groupCountY);
    }
    if (lhs.group_count_.groupCountZ != rhs.group_count_.groupCountZ) {
      return (lhs.group_count_.groupCountZ < rhs.group_count_.groupCountZ);
    }
    return false;
  }
};

struct ZeKernelProfileTimestamps {
  uint64_t metric_start;
  uint64_t metric_end;
  int32_t subdevice_id;
};

struct ZeKernelProfileRecord {
  ze_device_handle_t device_ = nullptr;
  std::vector<ZeKernelProfileTimestamps> timestamps_;
  uint64_t kernel_command_id_;
  uint64_t instance_id_;
  ze_group_count_t group_count_;
  size_t mem_size_;
  std::vector<uint8_t> *metrics_ = nullptr;
};

using ZeKernelProfiles = std::map<uint64_t, ZeKernelProfileRecord>;

struct ZeCommand;
struct ZeCommandMetricQuery;

struct ZeGraph {
  // Captured command descriptors and metric queries, cloned on each
  // graph execution.
  std::vector<ZeCommand *> commands_;
  std::vector<ZeCommandMetricQuery *> metric_queries_;

  // Maps each captured signal event to the cmdlist that signaled it.
  // Used for fork detection (a non-capturing cmdlist waits on one of these)
  // and join detection (primary cmdlist waits on a forked cmdlist's event).
  std::map<ze_event_handle_t, ze_command_list_handle_t> event_to_cmdlist_;

  // The cmdlist that initiated capture (BeginGraphCapture / BeginCaptureIntoGraph).
  ze_command_list_handle_t primary_command_list_ = nullptr;

  // Cmdlists that auto-entered capture via fork detection.
  // Cleaned up on EndGraphCapture if not already joined.
  std::set<ze_command_list_handle_t> forked_command_lists_;
};

// Global graph tracking data structures
static std::shared_mutex graphs_mutex_;
static std::map<ze_graph_handle_t, ZeGraph*> graphs_;

// Map from executable graph to source graph (for looking up captured commands on execution)
static std::shared_mutex executable_graphs_mutex_;
static std::map<ze_executable_graph_handle_t, ZeGraph*> executable_to_source_graph_;

static std::mutex global_kernel_profiles_mutex_;
static ZeKernelProfiles global_kernel_profiles_;

void SweepKernelProfiles(ZeKernelProfiles& profiles) {
  const std::lock_guard<std::mutex> lock(global_kernel_profiles_mutex_);
  global_kernel_profiles_.insert(profiles.begin(), profiles.end());
}

static std::mutex global_device_time_stats_mutex_;
static std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare> *global_device_time_stats_ = nullptr;

void SweepKernelCommandTimeStats(std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare>& stats) {
  global_device_time_stats_mutex_.lock();
  if (global_device_time_stats_ == nullptr) {
    global_device_time_stats_ = new std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare>;
    UniMemory::ExitIfOutOfMemory((void *)(global_device_time_stats_));
  }
  for (auto it = stats.begin(); it != stats.end(); it++) {
    auto it2 = global_device_time_stats_->find(it->first);
    if (it2 == global_device_time_stats_->end()) {
      ZeKernelCommandTime stat;
      stat.append_time_ = it->second.append_time_;
      stat.submit_time_ = it->second.submit_time_;
      stat.execute_time_ = it->second.execute_time_;
      stat.min_time_ = it->second.min_time_;
      stat.max_time_ = it->second.max_time_;
      stat.call_count_ = it->second.call_count_;
      global_device_time_stats_->insert({it->first, std::move(stat)});
    }
    else {
      it2->second.append_time_ += it->second.append_time_;
      it2->second.submit_time_ +=  it->second.submit_time_;
      it2->second.execute_time_ += it->second.execute_time_;
      if (it->second.max_time_ > it2->second.max_time_) {
        it2->second.max_time_ = it->second.max_time_;
      }
      if (it->second.min_time_ < it2->second.min_time_) {
        it2->second.min_time_ = it->second.min_time_;
      }
      it2->second.call_count_ += it->second.call_count_;
    }
  }
  global_device_time_stats_mutex_.unlock();
}

static std::mutex global_host_time_stats_mutex_;
static std::map<uint32_t, ZeFunctionTime> *global_host_time_stats_ = nullptr;

void SweepHostFunctionTimeStats(std::map<uint32_t, ZeFunctionTime>& stats) {
  global_host_time_stats_mutex_.lock();
  if (global_host_time_stats_ == nullptr) {
    global_host_time_stats_ = new std::map<uint32_t, ZeFunctionTime>;
    UniMemory::ExitIfOutOfMemory((void *)(global_host_time_stats_));
  }
  for (auto it = stats.begin(); it != stats.end(); it++) {
    auto it2 = global_host_time_stats_->find(it->first);
    if (it2 == global_host_time_stats_->end()) {
      ZeFunctionTime stat;
      stat.total_time_ = it->second.total_time_;
      stat.min_time_ = it->second.min_time_;
      stat.max_time_ = it->second.max_time_;
      stat.call_count_ = it->second.call_count_;
      global_host_time_stats_->insert({it->first, std::move(stat)});
    }
    else {
      it2->second.total_time_ += it->second.total_time_;
      if (it->second.max_time_ > it2->second.max_time_) {
        it2->second.max_time_ = it->second.max_time_;
      }
      if (it->second.min_time_ < it2->second.min_time_) {
        it2->second.min_time_ = it->second.min_time_;
      }
      it2->second.call_count_ += it->second.call_count_;
    }
  }
  global_host_time_stats_mutex_.unlock();
}

struct ZeCommandMetricQuery {
  uint64_t instance_id_;        // unique kernel or command instance identifier
  zet_metric_query_handle_t metric_query_;
  ze_event_handle_t metric_query_event_;
  ze_device_handle_t device_;
  ZeKernelCommandType type_;
  bool immediate_;
};

struct ZeCommand {
  uint64_t kernel_command_id_;  // kernel or command identifier
  uint64_t instance_id_;        // unique kernel or command instance identifier
  ze_event_handle_t event_;
  ze_event_handle_t timestamp_event_;
  ze_event_handle_t in_order_counter_event_;
  ze_device_handle_t device_;
  ze_context_handle_t context_;  // T6'/E6: owning context (graph replay clones carry it for the batched timestamp read)
  uint64_t host_time_origin_;   // in ns
  uint64_t device_timer_frequency_;
  uint64_t device_timer_mask_;
  double device_ns_per_cycle_;
  uint64_t append_time_;
  uint64_t submit_time_;        // in ns
  uint64_t submit_time_device_; // in ticks
  ze_command_list_handle_t command_list_;
  ze_command_queue_handle_t queue_;
  ze_fence_handle_t fence_;
  uint64_t tid_;
  uint64_t mem_size_;           // memory copy/fill size
  ZeCommandMetricQuery *command_metric_query_;
  uint32_t engine_ordinal_;
  uint32_t engine_index_;
  ZeKernelGroupSize group_size_;
  ze_group_count_t group_count_;
  ZeKernelCommandType type_;
  std::vector<ze_kernel_timestamp_result_t *> *timestamps_on_event_reset_;  // points to timestamps_on_event_reset_ in the command list
  ze_kernel_timestamp_result_t **timestamps_on_commands_completion_;  // points to timestamps_on_commands_completion_ in command list
  uint64_t *device_global_timestamps_;  // points to device_global_timestamps_
  int timestamp_seq_;  // sequence number in the command list for timestamps
  std::vector<int> *index_timestamps_on_commands_completion_;  // indices to timestamps_on_commands_completion_
  std::vector<int> *index_timestamps_on_event_reset_;  // indices to timestamps_on_event_reset_
  uint64_t signal_gen_;  // signal generation of event_ expected by this command (TSLOG2)
  // E6-v4b: kernel-timestamp packet read at the staging drain, whose emit is
  // deferred to a later sweep (see BatchQktArmDeferredEmit). Non-null means
  // the packet is already safely buffered and this command must not be
  // re-collected, re-queried or reset until its emit ran.
  const ze_kernel_timestamp_result_t *deferred_ts_;
  bool implicit_scaling_;
  bool immediate_;
  bool graph_command_;  // true if this command is part of a graph execution (event is owned by graph)
};

// T6' (plan E6): per-replay batched kernel-timestamp read for graph replay
// clones (UNITRACE_GRAPH_BATCH_QKT=1). One graph replay shares ONE completion
// event across all of its clones, so once that event is signaled the packets of
// every clone can be read with a single device-side batch query instead of one
// ioctl per clone. The sweep collects the ready clones in list order, reads all
// packets in one shot, and the loop then consumes them in the same order via a
// cursor match (no per-command state), so emit order, TSBAD handling and the
// pending-clone event reset stay byte-for-byte on the legacy pipeline.
struct ZeBatchQkt {
  bool ok = false;      // packets read; cmds[i] is consumed from ts[i]
  size_t cursor = 0;    // next batched command the sweep loop expects to see
  std::vector<ZeCommand *> cmds;                  // sweep (list) order
  std::vector<ze_kernel_timestamp_result_t> ts;   // one packet per command
};


// E6-v4c (UNITRACE_GRAPH_QKT_AT_POLL=1): a poll sweep arms the step's batch
// read EARLY — one zeCommandListAppendQueryKernelTimestamps on a
// collector-owned immediate list whose numWaitEvents is the SAME event set it
// reads — and the staging drain finishes it (one HostSynchronize + memcpy +
// the unchanged v4b arm). Because the read is issued while the replay is
// still executing and gated device-side on its own events, the packets are
// captured the moment the last kernel of the step signals, i.e. during
// device-busy time, and the drain's exposed serial no longer contains the
// ~6.4ms append+sync (gq4: max_batch_us 7042 inside a 6.42ms graph ioctl).
// The descriptor is deliberately STATELESS with respect to the clones: arming
// writes no ZeCommand field, releases no event, consumes no pending counter
// and touches no packet, so a failed or skipped arm degrades to the exact v4b
// drain read with nothing to unwind. One slot only (uni_batchqkt_read_pending_
// guards it); guarded by the submission locks like the drain's own batch.
struct ZeBatchQktPollRead {
  bool live = false;                              // append succeeded, sync pending
  ze_context_handle_t context = nullptr;
  ze_device_handle_t device = nullptr;
  ze_command_list_handle_t list = nullptr;        // collector-owned poll imm list
  void *dst = nullptr;                            // collector-owned packet buffer
  size_t n = 0;                                   // == cmds.size() == events.size()
  uint64_t append_us = 0;                         // THIS batch's append host cost (v4b max_batch_us pairing)
  std::vector<ZeCommand *> cmds;                  // sweep (list) order
  std::vector<ze_event_handle_t> events;          // read set == wait set
};


struct ZeDeviceSubmissions;
std::shared_mutex global_device_submissions_mutex_;
std::set<ZeDeviceSubmissions *> *global_device_submissions_ = nullptr;

struct ZeDeviceSubmissions {
  std::list<ZeCommand *> commands_submitted_;
  std::list<ZeCommand *> commands_staged_;
  std::list<ZeCommand *> commands_free_pool_;
  std::list<ZeCommandMetricQuery *> metric_queries_submitted_;
  std::list<ZeCommandMetricQuery *> metric_queries_staged_;
  std::list<ZeCommandMetricQuery *> metric_queries_free_pool_;
  std::map<ZeKernelCommandNameKey, ZeKernelCommandTime, ZeKernelCommandNameKeyCompare> device_time_stats_;
  std::map<uint32_t, ZeFunctionTime> host_time_stats_;
  ZeKernelProfiles kernel_profiles_;
  std::atomic<bool> finalized_;

  ZeDeviceSubmissions() {
    finalized_.store(false, std::memory_order_release);

    ZeCommand *command = new ZeCommand;

    UniMemory::ExitIfOutOfMemory((void *)(command));

    commands_free_pool_.push_back(command);
    global_device_submissions_mutex_.lock();
    if (global_device_submissions_ == nullptr) {
      global_device_submissions_ = new std::set<ZeDeviceSubmissions *>;
      UniMemory::ExitIfOutOfMemory((void *)(global_device_submissions_));
    }

    global_device_submissions_->insert(this);
    global_device_submissions_mutex_.unlock();
  }

  ~ZeDeviceSubmissions() {
    global_device_submissions_mutex_.lock();
    if (!finalized_.exchange(true)) {
      // finalize if not finalized
      SweepKernelCommandTimeStats(device_time_stats_);
      SweepHostFunctionTimeStats(host_time_stats_);
      SweepKernelProfiles(kernel_profiles_);
      global_device_submissions_->erase(this);
    }
    global_device_submissions_mutex_.unlock();
  }

  ZeDeviceSubmissions(const struct ZeDeviceSubmissions& that) = delete;

  ZeDeviceSubmissions& operator=(const struct ZeDeviceSubmissions& that) = delete;

  inline void SubmitKernelCommand(ZeCommand *command) {
    if (!IsFinalized()) {
      commands_submitted_.push_back(command);
    }
    else {
      commands_free_pool_.push_back(command);
    }
  }

  inline void StageKernelCommand(ZeCommand *command) {
    commands_staged_.push_back(command);
  }

  inline ZeCommand *GetKernelCommand(void) {
    ZeCommand *command;

    if (commands_free_pool_.empty()) {
      command = new ZeCommand;
      UniMemory::ExitIfOutOfMemory((void *)(command));
    }
    else {
      command = commands_free_pool_.front();
      commands_free_pool_.pop_front();
    }

    // Explicitly initialize ZeCommand members.
    command->instance_id_ = 0;
    command->event_ = nullptr;
    command->in_order_counter_event_ = nullptr;
    command->device_ = nullptr;
    command->context_ = nullptr;
    command->append_time_ = 0;
    command->submit_time_ = 0;
    command->submit_time_device_ = 0;
    command->command_list_ = nullptr;
    command->queue_ = nullptr;
    command->mem_size_ = 0;

    command->timestamp_seq_ = -1;
    command->timestamp_event_ = nullptr;
    command->timestamps_on_event_reset_ = nullptr;  // points to timestamps_on_event_reset_ in the command list
    command->timestamps_on_commands_completion_ = nullptr;    // points to timestamps_on_commands_completion_ in command list
    command->device_global_timestamps_ = nullptr;
    command->index_timestamps_on_commands_completion_ = nullptr;   // indices to timestamps_on_commands_completion_
    command->index_timestamps_on_event_reset_ = nullptr;
    command->signal_gen_ = 0;
    command->deferred_ts_ = nullptr;  // E6-v4b: never inherit a recycled emit slot
    command->graph_command_ = false;

    return command;
  }

  inline void SubmitCommandMetricQuery(ZeCommandMetricQuery *query) {
    if (!IsFinalized()) {
      metric_queries_submitted_.push_back(query);
    }
    else {
      metric_queries_free_pool_.push_back(query);
    }
  }

  inline void StageCommandMetricQuery(ZeCommandMetricQuery *query) {
    metric_queries_staged_.push_back(query);
  }

  inline void SubmitStagedKernelCommandAndMetricQueries(ZeEventCache& event_cache, std::vector<uint64_t> *kids) {
    auto cit = commands_staged_.begin();
    auto mit = metric_queries_staged_.begin();
    for (; cit != commands_staged_.end(); cit++, mit++) {
      ZeCommand *cmd = *cit;
      ZeCommandMetricQuery *cmd_query = *mit;

      // back fill kernel instance id and reset event
      cmd->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
      // Do not reset cmd->event_ here. The command may have already completed so the cmd->event_ may have already been signaled.
      // cmd->event_ is reset inside ProcessComamndSubmitted()

      if (kids) {
        kids->push_back(cmd->instance_id_);
      }
      SubmitKernelCommand(cmd);

      if (cmd_query != nullptr) {
        cmd_query->instance_id_ = cmd->instance_id_;
        SubmitCommandMetricQuery(cmd_query);
      }
    }
    commands_staged_.clear();
    metric_queries_staged_.clear();
  }

  inline void RevertStagedKernelCommandAndMetricQueries(void) {
    auto cit = commands_staged_.begin();
    auto mit = metric_queries_staged_.begin();
    for (; cit != commands_staged_.end(); cit++, mit++) {
      ZeCommand *cmd = *cit;
      ZeCommandMetricQuery *cmd_query = *mit;

      commands_free_pool_.push_back(cmd);
      if (cmd_query != nullptr) {
        metric_queries_free_pool_.push_back(cmd_query);
      }
    }
    commands_staged_.clear();
    metric_queries_staged_.clear();
  }

  inline void RevertStagedKernelCommandAndMetricQueriesForEvent(ze_event_handle_t event) {
    auto cit = commands_staged_.begin();
    auto mit = metric_queries_staged_.begin();
    while (cit != commands_staged_.end()) {
      ZeCommand *cmd = *cit;
      ZeCommandMetricQuery *cmd_query = *mit;

      if (cmd->event_ != event) {
        cit++;
        mit++;
        continue;
      }

      // remove from staged lists
      cit = commands_staged_.erase(cit);
      mit = metric_queries_staged_.erase(mit);

      commands_free_pool_.push_back(cmd);
      if (cmd_query != nullptr) {
        metric_queries_free_pool_.push_back(cmd_query);
      }
    }
  }

  inline ZeCommandMetricQuery *GetCommandMetricQuery(void) {
    ZeCommandMetricQuery *query;

    if (metric_queries_free_pool_.empty()) {
      query = new ZeCommandMetricQuery;
      UniMemory::ExitIfOutOfMemory((void *)(query));
    }
    else {
      query = metric_queries_free_pool_.front();
      metric_queries_free_pool_.pop_front();
    }

    query->instance_id_ = 0;
    query->metric_query_ = nullptr;
    query->metric_query_event_ = nullptr;
    query->device_ = nullptr;

    return query;
  }

  inline void CollectHostFunctionTimeStats(uint32_t id, uint64_t host_time) {
    auto it = host_time_stats_.find(id);
    if (it == host_time_stats_.end()){
      ZeFunctionTime stat;
      stat.total_time_ = host_time;
      stat.min_time_ = host_time;
      stat.max_time_ = host_time;
      stat.call_count_ = 1;
      host_time_stats_.insert({id, std::move(stat)});
    }
    else {
      it->second.total_time_ += host_time;
      if (host_time > it->second.max_time_) {
        it->second.max_time_ = host_time;
      }
      if (host_time < it->second.min_time_) {
        it->second.min_time_ = host_time;
      }
      it->second.call_count_ += 1;
    }
  }

  inline void CollectKernelCommandTimeStats(const ZeCommand *command, uint64_t kernel_start, uint64_t kernel_end, int tile) {
    ZeKernelCommandNameKey key {command->kernel_command_id_, command->mem_size_, tile, command->group_count_};
    uint64_t kernel_time = kernel_end - kernel_start;
    auto it = device_time_stats_.find(key);
    if (it == device_time_stats_.end()){
      ZeKernelCommandTime stat;
      stat.append_time_ = command->submit_time_ - command->append_time_;
      stat.submit_time_ = kernel_start - command->submit_time_;
      stat.execute_time_ = kernel_time;
      stat.min_time_ = kernel_time;
      stat.max_time_ = kernel_time;
      stat.call_count_ = 1;
      device_time_stats_.insert({std::move(key), std::move(stat)});
    }
    else {
      it->second.append_time_ += (command->submit_time_ - command->append_time_);
      it->second.submit_time_ +=  (kernel_start - command->submit_time_);
      it->second.execute_time_ += kernel_time;
      if (kernel_time > it->second.max_time_) {
        it->second.max_time_ = kernel_time;
      }
      if (kernel_time < it->second.min_time_) {
        it->second.min_time_ = kernel_time;
      }
      it->second.call_count_ += 1;
    }
  }

  inline bool IsFinalized(void) {
    return finalized_.load(std::memory_order_acquire);
  }

  inline void Finalize(void) {
    // caller holds exclusive global_device_submissions_mutex_ lock
    finalized_.store(true, std::memory_order_release);
    SweepKernelCommandTimeStats(device_time_stats_);
    SweepHostFunctionTimeStats(host_time_stats_);
    SweepKernelProfiles(kernel_profiles_);
  }
};

thread_local ZeDeviceSubmissions local_device_submissions_;

struct ZeKernelCommandProperties {
  uint64_t id_;    // unique identifier
  uint64_t size_;  // kernel binary size
  uint64_t base_addr_;  // kernel base address
  ze_device_handle_t device_;
  int32_t device_id_;
  uint32_t simd_width_;  // SIMD
  uint32_t nargs_;  // number of kernel arguments
  uint32_t nsubgrps_;  // maximal number of subgroups
  uint32_t slmsize_;  // SLM size
  uint32_t private_mem_size_;  // private memory size for each thread
  uint32_t spill_mem_size_;  // spill memory size for each thread
  ZeKernelGroupSize group_size_;  // group size
  ZeKernelCommandType type_;
  uint32_t regsize_;  // GRF size per thread
  bool aot_;    // AOT or JIT
  std::string name_;  // kernel or command name
  bool skip_;  // skip this kernel in the trace
};

// these will not go away when ZeCollector is destructed
static std::shared_mutex kernel_command_properties_mutex_;
static std::map<uint64_t, ZeKernelCommandProperties> *kernel_command_properties_ = nullptr;
static std::map<ze_kernel_handle_t, ZeKernelCommandProperties> *active_kernel_properties_ = nullptr;
static std::map<uint64_t, ZeKernelCommandProperties> *active_command_properties_ = nullptr;

struct ZeModule {
  ze_device_handle_t device_;
  size_t size_;
  bool aot_;  // AOT or JIT
};

static std::shared_mutex modules_on_devices_mutex_;
static std::map<ze_module_handle_t, ZeModule> modules_on_devices_; //module to ZeModule map

enum ZeQueueEngineType {
  ZE_QUEUE_COMPUTE_ENGINE,
  ZE_QUEUE_COPY_ENGINE,
  // Add any new engine type
  ZE_QUEUE_UNKNOWN_ENGINE
};

struct ZeDevice {
  ze_device_handle_t device_;
  ze_device_handle_t parent_device_;
  uint64_t host_time_origin_;  // in ns
  uint64_t device_timer_frequency_;
  uint64_t device_timer_mask_;
  double device_ns_per_cycle_;
  ze_driver_handle_t driver_;
  ze_context_handle_t context_;
  zet_metric_group_handle_t metric_group_;
  int32_t id_;
  int32_t parent_id_;
  int32_t subdevice_id_;
  int32_t num_subdevices_;
  ze_pci_ext_properties_t pci_properties_;
  std::string device_name_;
  std::vector<ZeQueueEngineType> queue_engine_prop_;
};

// these will no go away when ZeCollector is destructed
static std::shared_mutex devices_mutex_;
static std::map<ze_device_handle_t, ZeDevice> *devices_ = nullptr;

struct ZeCommandQueue {
  ze_command_queue_handle_t queue_;
  ze_context_handle_t context_;
  ze_device_handle_t device_;
  uint32_t engine_ordinal_;
  uint32_t engine_index_;
};


constexpr static int number_timestamps_per_slice_ = 128;
constexpr static int cache_line_size_ = 64;

struct ZeCommandList {
  ze_command_list_handle_t cmdlist_;
  ze_context_handle_t context_;
  ze_device_handle_t device_;
  uint64_t host_time_origin_;  // in ns
  uint64_t device_timer_frequency_;
  uint64_t device_timer_mask_;
  double device_ns_per_cycle_;
  uint32_t engine_ordinal_;  // valid if immediate command list
  uint32_t engine_index_;  // valid if immediate command list
  bool immediate_;
  bool implicit_scaling_;
  bool in_order_;
  std::vector<ZeCommand *> commands_;  // if non-immediate command list
  std::vector<ZeCommandMetricQuery *> metric_queries_;  // if non-immediate command list
  std::vector<ze_kernel_timestamp_result_t *> timestamps_on_event_reset_; // timestamps queried on event reset
  ze_kernel_timestamp_result_t *timestamps_on_commands_completion_; // timestamps queried on commands completion
  int num_timestamps_;  // total number of timestamps
  int num_timestamps_on_event_reset_;  // total number of timestamps queried on event reset
  std::map<ze_event_handle_t, int> event_to_timestamp_seq_; // map event to timestamp sequence in command list
  std::vector<int> index_timestamps_on_commands_completion_;  // indices to timestamps_on_commands_completion_ for each command
  std::vector<int> index_timestamps_on_event_reset_;  // indices to timestamps_on_event_reset_ for each command
  std::vector<uint64_t *> device_global_timestamps_;  // device timestamps on host
  int num_device_global_timestamps_;
  ze_event_handle_t timestamp_event_to_signal_;
  bool graph_capturing_ = false;
  ZeGraph* graph_capture_target_ = nullptr;
  ZeGraph* pending_graph_capture_ = nullptr;  // Heap-allocated for BeginGraphCaptureExp path
};

typedef void (*OnZeFunctionFinishCallback)(std::vector<uint64_t> *kids, FLOW_DIR flow_dir, API_TRACING_ID api_id, uint64_t started, uint64_t ended);

// T14/A5 (UNITRACE_TRACE_META): collector self-annotation. Wired to
// ChromeLogger::MetaLoggingCallback by tracer.h. |name| is the full record
// name ("unitrace.emit"); the window is UniTimer::GetHostTimestamp() ns (an
// instant when end <= start); |args_json| is a pre-rendered JSON object body
// or nullptr; |counter_ms| >= 0 additionally emits the per-step debt counter
// sample. See IMPL_NOTES_meta.md for the record table.
typedef void (*OnZeMetaRecordCallback)(const char* name, uint64_t start_ns, uint64_t end_ns, const char* args_json, double counter_ms);

typedef void (*OnZeKernelFinishCallback)(uint64_t kid, uint64_t tid, uint64_t start, uint64_t end, uint32_t ordinal, uint32_t index, int32_t tile, const ze_device_handle_t device, const uint64_t kernel_command_id, bool implicit_scaling, const ze_group_count_t& group_count, size_t mem_size);

ze_result_t (*ZexKernelGetBaseAddress)(ze_kernel_handle_t hKernel, uint64_t *baseAddress) = nullptr;

// O2 (tax reduction): formatting a kernel command name costs ~13us per record
// (Demangle + to_string x8 + concatenation, all under lock_shared) and the
// vLLM trim24L bench emits ~350 records per step while only ~114 distinct
// (kernel, grid) shapes exist. Cache the fully formatted name per shape;
// misses and cache-bypass paths fall through to the original formatting.
struct UniKernelNameCacheKey {
  uint64_t id;
  uint32_t gx;
  uint32_t gy;
  uint32_t gz;
  size_t size;
  bool detailed;
  bool operator==(const UniKernelNameCacheKey& o) const {
    return id == o.id && gx == o.gx && gy == o.gy && gz == o.gz &&
           size == o.size && detailed == o.detailed;
  }
};
struct UniKernelNameCacheHash {
  size_t operator()(const UniKernelNameCacheKey& k) const {
    size_t h = std::hash<uint64_t>()(k.id);
    auto mix = [&h](size_t v) {
      h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(std::hash<uint32_t>()(k.gx));
    mix(std::hash<uint32_t>()(k.gy));
    mix(std::hash<uint32_t>()(k.gz));
    mix(std::hash<size_t>()(k.size));
    mix(std::hash<bool>()(k.detailed));
    return h;
  }
};
// Bounded by shape count: beyond this many shapes the cache stops admitting
// (no eviction logic, no unbounded growth); new shapes keep the direct path.
static constexpr size_t kUniKernelNameCacheMax = 8192;

inline std::string BuildZeKernelCommandName(uint64_t id, const ze_group_count_t& group_count, size_t size, bool detailed) {
  std::string str;
  kernel_command_properties_mutex_.lock_shared();
  auto it = kernel_command_properties_->find(id);
  if (it != kernel_command_properties_->end()) {
    str = "\"";
    str += std::move(utils::Demangle(it->second.name_.c_str()));  // quote kernel name which may contain ","
    if (detailed) {
      if (it->second.type_ == KERNEL_COMMAND_TYPE_COMPUTE) {
        if (it->second.simd_width_ > 0) {
          str += "[SIMD";
          if (it->second.simd_width_ == 1) {
            str += "_ANY";
          } else {
            str += std::to_string(it->second.simd_width_);
          }
        }
        str = str + " {" +
          std::to_string(group_count.groupCountX) + "; " +
          std::to_string(group_count.groupCountY) + "; " +
          std::to_string(group_count.groupCountZ) + "} {" +
          std::to_string(it->second.group_size_.x) + "; " +
          std::to_string(it->second.group_size_.y) + "; " +
          std::to_string(it->second.group_size_.z) + "}]";
      }
      else if ((it->second.type_ == KERNEL_COMMAND_TYPE_MEMORY) && (size > 0)) {
        str = str + "[" + std::to_string(size) + "]";
      }
    }
    str += "\"";  // quote kernel name
  }

  kernel_command_properties_mutex_.unlock_shared();

  return str;
}

inline std::string GetZeKernelCommandName(uint64_t id, const ze_group_count_t& group_count, size_t size, bool detailed = true) {
  static std::shared_mutex cache_mutex;
  static std::unordered_map<UniKernelNameCacheKey, std::string, UniKernelNameCacheHash>* cache = nullptr;
  static std::atomic<bool> cache_alloc_failed{false};
  static std::atomic<bool> cache_warn_logged{false};

  UniKernelNameCacheKey key{id, group_count.groupCountX, group_count.groupCountY,
                            group_count.groupCountZ, size, detailed};
  if (cache != nullptr) {
    std::shared_lock<std::shared_mutex> lk(cache_mutex);
    auto cit = cache->find(key);
    if (cit != cache->end()) {
      return cit->second;
    }
  }
  std::string str = BuildZeKernelCommandName(id, group_count, size, detailed);
  // Only cache well-formed names: an empty result means the properties entry
  // is not registered yet and would poison the cache if it appears later.
  if (str.empty()) {
    return str;
  }
  if (cache == nullptr) {
    if (cache_alloc_failed.load(std::memory_order_relaxed)) {
      return str;  // allocation already failed once; stay on the direct path
    }
    try {
      cache = new std::unordered_map<UniKernelNameCacheKey, std::string, UniKernelNameCacheHash>();
    } catch (const std::bad_alloc&) {
      cache_alloc_failed.store(true, std::memory_order_relaxed);
      std::cerr << "[WARNING] GetZeKernelCommandName: name cache allocation failed, staying on the direct formatting path" << std::endl;
      return str;
    }
  }
  {
    std::unique_lock<std::shared_mutex> lk(cache_mutex);
    if (cache->size() < kUniKernelNameCacheMax) {
      try {
        cache->emplace(key, str);
      } catch (const std::bad_alloc&) {
        if (!cache_warn_logged.exchange(true)) {
          std::cerr << "[WARNING] GetZeKernelCommandName: name cache insert failed, this shape stays on the direct formatting path" << std::endl;
        }
      }
    } else if (!cache_warn_logged.exchange(true)) {
      std::cerr << "[WARNING] GetZeKernelCommandName: name cache full (" << kUniKernelNameCacheMax
                << " shapes), new shapes stay on the direct formatting path" << std::endl;
    }
  }
  return str;
}

inline std::string GetZeKernelCommandName(uint64_t id, ze_group_count_t& group_count, size_t size, bool detailed = true) {
  const ze_group_count_t& gcount = group_count;
  return GetZeKernelCommandName(id, gcount, size, detailed);
}

inline std::string GetZeDeviceName(ze_device_handle_t device) {
  std::string device_name = "";
  devices_mutex_.lock_shared();
  if (devices_ != nullptr) {
    auto it = devices_->find(device);
    if (it != devices_->end()) {
      device_name = it->second.device_name_;
    }
  }
  devices_mutex_.unlock_shared();
  return device_name;
}

inline std::string GetZeEngineName(ze_device_handle_t device, uint32_t ordinal) {
  std::string engine_name = "";
  devices_mutex_.lock_shared();
  if (devices_ != nullptr) {
    auto it = devices_->find(device);
    if (it != devices_->end()) {
      if (it->second.queue_engine_prop_[ordinal] == ZE_QUEUE_COMPUTE_ENGINE) {
        engine_name = "L0 Compute Engine";
      } else if (it->second.queue_engine_prop_[ordinal] == ZE_QUEUE_COPY_ENGINE) {
        engine_name = "L0 Copy Engine";
      } else {
        engine_name = "L0 Engine";
      }
    }
  }
  devices_mutex_.unlock_shared();
  return engine_name;
}

inline ze_pci_ext_properties_t *GetZeDevicePciPropertiesAndId(ze_device_handle_t device, int32_t *parent_device_id, int32_t *device_id, int32_t *subdevice_id){
  devices_mutex_.lock_shared();
  ze_pci_ext_properties_t *props = nullptr;

  if (devices_) {
    auto it = devices_->find(device);
    if (it != devices_->end()) {
      if (parent_device_id) {
        *parent_device_id = it->second.parent_id_;
      }
      if (device_id) {
        *device_id = it->second.id_;
      }
      if (subdevice_id) {
        *subdevice_id = it->second.subdevice_id_;
      }

      props = &(it->second.pci_properties_);
    }
  }
  devices_mutex_.unlock_shared();

  return props;

}

// Forward declaration for global collector pointer
class ZeCollector;

// Global collector pointer for extension API wrappers
static ZeCollector* s_global_ze_collector_ = nullptr;

// Recursion guard for extension API wrappers.
// Extension APIs are intercepted via zeDriverGetExtensionFunctionAddress and do
// not go through the Level Zero tracing layer (zelTracer), so L0's built-in
// ZE_HANDLE_TRACER_RECURSION guard (tracing_layer::tracingInProgress) does not
// apply. This flag prevents zelTracer callbacks from re-entering when L0 APIs
// are called from within extension API wrapper callbacks.
// This could be replaced by ZE_HANDLE_TRACER_RECURSION if Level Zero exposes
// access to tracing_layer::tracingInProgress via a public API.
static thread_local bool s_extension_api_tracing_in_progress_ = false;

static void SetGlobalZeCollector(ZeCollector* collector) {
  if (collector != nullptr && s_global_ze_collector_ != nullptr) {
    std::cerr << "[ERROR] SetGlobalZeCollector: global pointer is already set. "
              << "Only one ZeCollector per process is supported." << std::endl;
    return;
  }
  s_global_ze_collector_ = collector;
}

class ZeCollector {
 private:
    // Helpers
    static bool SampleThisRank() {
      std::set<int> ranks_to_sample;
      GetZeRanksToSample(ranks_to_sample);

      // Check if rank is specified for tracing in MPI environment
      if (ranks_to_sample.size() > 0) {
        const auto &my_MPI_rank = (utils::GetEnv("PMI_RANK").empty()) ? utils::GetEnv("PMIX_RANK") : utils::GetEnv("PMI_RANK");
        if (!my_MPI_rank.empty()) {
            auto my_rank = std::stoi(my_MPI_rank);
            if (ranks_to_sample.find(my_rank) == ranks_to_sample.end()) {
              std::cout << "[INFO] MPI rank " << my_rank << " is not enabled for tracing or profiling" << std::endl;
              return false;
            }
        }
      }
      return true;
    }

    static bool SampleThisDevice(ze_driver_handle_t driver) {
      std::set<int> devices_to_sample;
      GetZeDevicesToSample(devices_to_sample);

      if (devices_to_sample.size() != 0) {
        int global_dev_cnt = 0;
        auto devices = GetDeviceList(driver);
        for (auto device : devices) {
          if (!devices_to_sample.empty()) {
            if (devices_to_sample.find(global_dev_cnt) != devices_to_sample.end()) {
              break;
            }
          }
          global_dev_cnt++;
        }
        if (global_dev_cnt == static_cast<int>(devices.size())) {
          std::cout << "[INFO] Device or devices visible to process " << utils::GetPid() << " are not enabled for tracing or profiling" << std::endl;
          return false;
        }
      }
      return true;
    }
 public: // Interface

  static ZeCollector* Create(
      CollectorOptions options,
      OnZeKernelFinishCallback kcallback = nullptr,
      OnZeFunctionFinishCallback fcallback = nullptr,
      void* callback_data = nullptr,
      OnZeMetaRecordCallback mcallback = nullptr) {
    ze_api_version_t version = GetZeVersion();
    PTI_ASSERT(
        ZE_MAJOR_VERSION(version) >= 1 &&
        ZE_MINOR_VERSION(version) >= 2);

    if (!SampleThisRank()) {
      return nullptr;
    }

    ze_driver_handle_t driver;
    uint32_t count = 1;
    if (ZE_FUNC(zeDriverGet)(&count, &driver) == ZE_RESULT_SUCCESS) {
      if (!SampleThisDevice(driver)) {
        return nullptr;
      }
      if (ZE_FUNC(zeDriverGetExtensionFunctionAddress)(driver, "zexKernelGetBaseAddress", (void **)&ZexKernelGetBaseAddress) != ZE_RESULT_SUCCESS) {
        ZexKernelGetBaseAddress = nullptr;
      }
    }
    else {
      std::cerr << "[ERROR] Unable to get Level Zero driver" << std::endl;
      return nullptr;
  }

    std::string data_dir_name = utils::GetEnv("UNITRACE_DataDir");
    bool reset_event_on_device = true;
    std::string reset_event_env = utils::GetEnv("UNITRACE_ResetEventOnDevice");
    if (!reset_event_env.empty() && reset_event_env == "0") {
      reset_event_on_device = false;
    }

    std::vector<std::string> include_kernels_vec = ParseFilterList(
        utils::GetEnv("UNITRACE_IncludeKernelsFile"),
        utils::GetEnv("UNITRACE_IncludeKernels")
    );
    std::vector<std::string> exclude_kernels_vec = ParseFilterList(
        utils::GetEnv("UNITRACE_ExcludeKernelsFile"),
        utils::GetEnv("UNITRACE_ExcludeKernels")
    );

    ZeCollector* collector = new ZeCollector(
      options, kcallback, fcallback, mcallback, callback_data, data_dir_name, reset_event_on_device, include_kernels_vec, exclude_kernels_vec);

    UniMemory::ExitIfOutOfMemory((void *)(collector));

    ze_result_t status = ZE_RESULT_SUCCESS;
    zel_tracer_desc_t tracer_desc = {
        ZEL_STRUCTURE_TYPE_TRACER_EXP_DESC, nullptr, collector};
    zel_tracer_handle_t tracer = nullptr;
    status = ZE_FUNC(zelTracerCreate)(&tracer_desc, &tracer);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[WARNING] Unable to create Level Zero tracer" << std::endl;
      delete collector;
      return nullptr;
    }

    collector->EnableTracing(tracer);

    collector->tracer_ = tracer;

    // Initialize extension API tracing (graph APIs, etc.)
    SetGlobalZeCollector(collector);

    return collector;
  }

  ZeCollector(const ZeCollector& that) = delete;

  ZeCollector& operator=(const ZeCollector& that) = delete;

  void FlushData() {
    // Fix B: stop the background completer first — it drains and joins, so the
    // final drain below runs entirely on the original inline path and no
    // completer thread outlives the collector teardown.
    StopDeferredCompleter();
    PrintBatchQktDiag();  // T6'/E6: batch counters (env UNITRACE_DEBUG_BATCHQKT)
    ProcessAllCommandsSubmitted(nullptr);

    global_device_submissions_mutex_.lock();
    if (global_device_submissions_) {
      for (auto it = global_device_submissions_->begin(); it != global_device_submissions_->end();) {
        (*it)->Finalize();
        it = global_device_submissions_->erase(it);
      }
    }
    global_device_submissions_mutex_.unlock();

    DumpKernelProfiles();
  }

  void Finalize() {
    FlushData();

#ifndef _WIN32
    // Not done on Windows: the process randomly dies inside zelTracerDestroy().
    // Teardown runs from DllMain(DLL_PROCESS_DETACH) at process exit, and the call
    // intermittently never returns -- it takes the process down before the trace is
    // closed, silently truncating the output. Only a fraction of runs are affected,
    // which is what made it look like malformed JSON rather than a crash.
    if (tracer_ != nullptr) {
      ze_result_t status = ZE_FUNC(zelTracerDestroy)(tracer_);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[WARNING] Failed to destroy tracer (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
      }
    }
#endif /* _WIN32 */

    // Reset the global collector pointer for extension API tracing
    SetGlobalZeCollector(nullptr);

    if (options_.metric_query) {
      for (auto it = metric_activations_.begin(); it != metric_activations_.end(); it++) {
        auto status = ZE_FUNC(zetContextActivateMetricGroups)(it->first, it->second, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
#ifndef _WIN32
          // on Windows, it is very possible that L0 has been unloaded or is being unloaded at this point and L0 calls may fail safely
          // so ignore the error
          std::cerr << "[WARNING] Failed to deactivate metric groups (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
#endif /* _WIN32 */
        }
      }
      metric_activations_.clear();
      for (auto& context : metric_contexts_) {
        auto status = ZE_FUNC(zeContextDestroy)(context);
        if (status != ZE_RESULT_SUCCESS) {
#ifndef _WIN32
          // on Windows, it is very possible that L0 has been unloaded or is being unloaded at this point and L0 calls may fail safely
          // so ignore the error
          std::cerr << "[WARNING] Failed to destroy context for metrics query (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
#endif /* _WIN32 */
        }
      }
      metric_contexts_.clear();
    }

    // T6'/E6: release the collector-owned immediate command list and staging
    // buffer. Runs after the tracer is destroyed and all submissions are
    // finalized, so these L0 calls cannot re-enter a live sweep.
    ReleaseBatchQktResources();
  }

  uint64_t CalculateTotalKernelTime() const {
    uint64_t total_time = 0;

    global_device_time_stats_mutex_.lock();
    if (global_device_time_stats_) {
      for (auto it = global_device_time_stats_->begin(); it != global_device_time_stats_->end(); it++) {
        total_time += it->second.execute_time_;
      }
    }
    global_device_time_stats_mutex_.unlock();

    return total_time;
  }

  void PrintKernelsTable(std::shared_ptr<Logger> logger) const {
    uint64_t total_time = 0;
    std::vector<std::string> knames;
    size_t max_name_size = 0;
    global_device_time_stats_mutex_.lock();

    AggregateDeviceTimeStats();

    std::set<std::pair<ZeKernelCommandNameKey, ZeKernelCommandTime>, utils::Comparator> sorted_list(
        global_device_time_stats_->begin(), global_device_time_stats_->end());

    for (auto& it : sorted_list) {
      total_time += it.second.execute_time_;
      std::string kname;
      if (it.first.tile_ >= 0) {
        kname = "Tile #" + std::to_string(it.first.tile_) + ": " + GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      else {
        kname = GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      if (kname.size() > max_name_size) {
        max_name_size = kname.size();
      }
      knames.push_back(kname);
    }

    if (total_time != 0) {
      // sizeof("Kernel") is 7, not 6
      std::string str(std::max(int(max_name_size - sizeof("Kernel") + 1), 0), ' ');
      str += "Kernel, " +
      std::string(std::max(int(kCallsLength - sizeof("Calls") + 1), 0), ' ') + "Calls, " +
      std::string(std::max(int(kTimeLength - sizeof("Time (ns)") + 1), 0), ' ') + "Time (ns), " +
        "    Time (%), " +
      std::string(std::max(int(kTimeLength - sizeof("Average (ns)") + 1), 0), ' ') + "Average (ns), " +
      std::string(std::max(int(kTimeLength - sizeof("Min (ns)") + 1), 0), ' ') + "Min (ns), " +
      std::string(std::max(int(kTimeLength - sizeof("Max (ns)") + 1), 0), ' ') + "Max (ns)\n";
      logger->Log(str);
      int i = 0;
      for (auto& it : sorted_list) {
        uint64_t call_count = it.second.call_count_;
        uint64_t time = it.second.execute_time_;
        uint64_t avg_time = time / call_count;
        uint64_t min_time = it.second.min_time_;
        uint64_t max_time = it.second.max_time_;
        float percent_time = (100.0f * time / total_time);

        str = std::string(std::max(int(max_name_size - knames[i].length()), 0), ' ');
        str += knames[i] + ", " +
        std::string(std::max(int(kCallsLength - std::to_string(call_count).length()), 0), ' ') +  std::to_string(call_count) + ", " +
        std::string(std::max(int(kTimeLength - std::to_string(time).length()), 0), ' ') + std::to_string(time) + ", " +
        std::string(std::max(int(sizeof("   Time (%)") - std::to_string(percent_time).length()), 0), ' ') +
        std::to_string(percent_time) + ", " +
        std::string(std::max(int(kTimeLength - std::to_string(avg_time).length()), 0), ' ') + std::to_string(avg_time) + ", " +
        std::string(std::max(int(kTimeLength - std::to_string(min_time).length()), 0), ' ') + std::to_string(min_time) + ", " +
        std::string(std::max(int(kTimeLength - std::to_string(max_time).length()), 0), ' ') + std::to_string(max_time) + "\n";
        logger->Log(str);
        i++;
      }

      str = "\n\n=== Kernel Properties ===\n\n";
      str = str + std::string(std::max(int(max_name_size - sizeof("Kernel") + 1), 0), ' ') +
        "Kernel, Compiled, SIMD, Number of Arguments, SLM Per Work Group, Private Memory Per Thread, Spill Memory Per Thread, Register File Size Per Thread\n";
      logger->Log(str);

      i = -1;
      kernel_command_properties_mutex_.lock_shared();

      for (auto& it : sorted_list) {
        ++i;
        auto kit = kernel_command_properties_->find(it.first.kernel_command_id_);
        if (kit == kernel_command_properties_->end()) {
          continue;
        }
        if (kit->second.type_ != KERNEL_COMMAND_TYPE_COMPUTE) {
          continue;
        }

        str = std::string(std::max(int(max_name_size - knames[i].length()), 0), ' ');
        str = str + knames[i] + "," +
          std::string(sizeof("Compiled") - sizeof("AOT") + 1, ' ') +
          (kit->second.aot_ ? "AOT" : "JIT") + "," +
          std::string(std::max(int(sizeof("SIMD") - ((kit->second.simd_width_ != 1) ? std::to_string(kit->second.simd_width_).length() : sizeof("ANY") - 1)), 0), ' ') +
          ((kit->second.simd_width_ != 1) ? std::to_string(kit->second.simd_width_) : "ANY") + "," +
          std::string(std::max(int(sizeof("Number of Arguments") - std::to_string(kit->second.nargs_).length()), 0), ' ') +
          std::to_string(kit->second.nargs_) + "," +
          std::string(std::max(int(sizeof("SLM Per Work Group") - std::to_string(kit->second.slmsize_).length()), 0), ' ') +
          std::to_string(kit->second.slmsize_) + "," +
          std::string(std::max(int(sizeof("Private Memory Per Thread") - std::to_string(kit->second.private_mem_size_).length()), 0), ' ') +
          std::to_string(kit->second.private_mem_size_) + "," +
          std::string(std::max(int(sizeof("Spill Memory Per Thread") - std::to_string(kit->second.spill_mem_size_).length()), 0), ' ') +
          std::to_string(kit->second.spill_mem_size_) + ",";
        if (kit->second.regsize_) {
          // report size if size is available
          str += std::string(std::max(int(sizeof("Register File Size Per Thread") - std::to_string(kit->second.regsize_).length()), 0), ' ') +
                 std::to_string(kit->second.regsize_) + "\n";
          }
        else {
          // report "unknown" otherwise
          str += std::string(sizeof("Register File Size Per Thread") - sizeof("unknown") + 1, ' ') +
                 "unknown\n";
        }
        logger->Log(str);
      }
      kernel_command_properties_mutex_.unlock_shared();
    }

    global_device_time_stats_mutex_.unlock();

  }

  void PrintSubmissionTable(std::shared_ptr<Logger> logger) const {
    uint64_t total_submit_time = 0;
    uint64_t total_append_time = 0;
    uint64_t total_device_time = 0;
    std::vector<std::string> knames;
    size_t max_name_size = 0;
    global_device_time_stats_mutex_.lock();

    AggregateDeviceTimeStats();

    std::set<std::pair<ZeKernelCommandNameKey, ZeKernelCommandTime>, utils::Comparator> sorted_list(
        global_device_time_stats_->begin(), global_device_time_stats_->end());

    for (auto& it : sorted_list) {
      total_device_time += it.second.execute_time_;
      total_append_time += it.second.append_time_;
      total_submit_time += it.second.submit_time_;
      std::string kname;
      if (it.first.tile_ >= 0) {
        kname = "Tile #" + std::to_string(it.first.tile_) + ": " + GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      else {
        kname = GetZeKernelCommandName(it.first.kernel_command_id_, it.first.group_count_, it.first.mem_size_, options_.verbose);
      }
      if (kname.size() > max_name_size) {
        max_name_size = kname.size();
      }
      knames.push_back(std::move(kname));
    }

    if (total_device_time != 0) {

      //sizeof("Kernel") is 7, not 6
      std::string str(std::max(int(max_name_size - sizeof("Kernel") + 1), 0), ' ');

      str += "Kernel, " + std::string(std::max(int(kCallsLength - sizeof("Calls") + 1), 0), ' ') +
             "Calls, " + std::string(std::max(int(kTimeLength - sizeof("Append (ns)") + 1), 0), ' ') +
             "Append (ns),  Append (%), " +
             std::string(std::max(int(kTimeLength - sizeof("Submit (ns)") + 1), 0), ' ') +
             "Submit (ns),  Submit (%), " +
             std::string(std::max(int(kTimeLength - sizeof("Execute (ns)") + 1), 0), ' ') +
             "Execute (ns),  Execute (%)\n";

      logger->Log(str);

      int i = 0;
      for (auto& it : sorted_list) {
        uint64_t call_count = it.second.call_count_;
        float append_percent = 100.0f * it.second.append_time_ / total_append_time;
        float submit_percent = 100.0f * it.second.submit_time_ / total_submit_time;
        float device_percent = 100.0f * it.second.execute_time_ / total_device_time;
        str = std::string(std::max(int(max_name_size - knames[i].length()), 0), ' ') + knames[i] + ", ";
        str += std::string(std::max(int(kCallsLength - std::to_string(call_count).length()), 0), ' ') + std::to_string(call_count) + ", " +
               std::string(std::max(int(kTimeLength - std::to_string(it.second.append_time_).length()), 0), ' ') +
               std::to_string(it.second.append_time_) + ", " +
               std::string(std::max(int(sizeof("Append (%)") - std::to_string(append_percent).length()), 0), ' ') +
               std::to_string(append_percent) + ", " +
               std::string(std::max(int(kTimeLength - std::to_string(it.second.submit_time_).length()), 0), ' ') +
               std::to_string(it.second.submit_time_) + ", " +
               std::string(std::max(int(sizeof("Submit (%)") - std::to_string(submit_percent).length()), 0), ' ') +
               std::to_string(submit_percent) + ", " +
               std::string(std::max(int(kTimeLength - std::to_string(it.second.execute_time_).length()), 0), ' ') +
               std::to_string(it.second.execute_time_) + ", " +
               std::string(std::max(int(sizeof("Execute (%)") - std::to_string(device_percent).length()), 0), ' ') +
               std::to_string(device_percent) + "\n";
        logger->Log(str);
        i++;
      }
    }

    global_device_time_stats_mutex_.unlock();

  }

  void DisableTracing() {
    // For Windows, level-zero might have been unloaded hence calling zelTracerSetEnabled may have undefined behavior, so skip calling it.
#ifndef _WIN32
    ze_result_t status = ZE_FUNC(zelTracerSetEnabled)(tracer_, false);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
#endif /* _WIN32 */
  }

  uint64_t CalculateTotalFunctionTime() const {
    global_host_time_stats_mutex_.lock();

    uint64_t total_time = 0;
    for (auto it = global_host_time_stats_->begin(); it != global_host_time_stats_->end(); it++) {
      total_time += it->second.total_time_;
    }

    global_host_time_stats_mutex_.unlock();

    return total_time;
  }

  void PrintFunctionsTable(std::shared_ptr<Logger> logger) const {
    global_host_time_stats_mutex_.lock();
    std::set<std::pair<uint32_t, ZeFunctionTime>, utils::Comparator> sorted_list(
      global_host_time_stats_->begin(), global_host_time_stats_->end());

    uint64_t total_time = 0;
    size_t max_name_size = 0;
    for (auto& stat : sorted_list) {
      total_time += stat.second.total_time_;
      if (get_symbol(API_TRACING_ID(stat.first)).size() > max_name_size) {
        max_name_size = get_symbol(API_TRACING_ID(stat.first)).size();
      }
    }

    if (total_time != 0) {
      std::string str(std::max(int(max_name_size - sizeof("Function") + 1), 0), ' ');
      str += "Function, " + std::string(std::max(int(kCallsLength - sizeof("Calls") + 1), 0), ' ') +
             "Calls, " + std::string(std::max(int(kTimeLength - sizeof("Time (ns)") + 1), 0), ' ') +
             "Time (ns),      Time (%), " + std::string(std::max(int(kTimeLength - sizeof("Average (ns)") + 1), 0), ' ') +
             "Average (ns), " + std::string(std::max(int(kTimeLength - sizeof("Min (ns)") + 1), 0), ' ') +
             "Min (ns), " + std::string(std::max(int(kTimeLength - sizeof("Max (ns)") + 1), 0), ' ') +
             "Max (ns)\n";
      logger->Log(str);
      for (auto& stat : sorted_list) {
        const std::string function = get_symbol(API_TRACING_ID(stat.first));
        uint64_t time = stat.second.total_time_;
        uint64_t call_count = stat.second.call_count_;
        uint64_t avg_time = time / call_count;
        uint64_t min_time = stat.second.min_time_;
        uint64_t max_time = stat.second.max_time_;
        float percent_time = 100.0f * time / total_time;
        str = std::string(std::max(int(max_name_size - function.length()), 0), ' ') + function + ", " +
              std::string(std::max(int(kCallsLength - std::to_string(call_count).length()), 0), ' ') + std::to_string(call_count) + ", " +
              std::string(std::max(int(kTimeLength - std::to_string(time).length()), 0), ' ') + std::to_string(time) + ", " +
              std::string(std::max(int(sizeof("    Time (%)") - std::to_string(percent_time).length()), 0), ' ') +
              std::to_string(percent_time) + ", " +
              std::string(std::max(int(kTimeLength - std::to_string(avg_time).length()), 0), ' ') + std::to_string(avg_time) + ", " +
              std::string(std::max(int(kTimeLength - std::to_string(min_time).length()), 0), ' ') + std::to_string(min_time) + ", " +
              std::string(std::max(int(kTimeLength - std::to_string(max_time).length()), 0), ' ') + std::to_string(max_time) + "\n";
        logger->Log(str);
      }
    }
    global_host_time_stats_mutex_.unlock();
  }

  void ProcessCommandsSubmitted(std::vector<uint64_t> *kids) {

    if (uni_batchqkt_in_batch_) {
      return;  // T6'/E6: re-entry from the batch's own L0 calls (no-op, same pattern as Fix B)
    }

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    global_device_submissions_mutex_.lock_shared();

    // T6'/E6 (v3): this drain runs at command list reset/destroy and at graph
    // release — exactly the places that would strand or free a pending
    // clone's event — so it is a flush point: the accumulated clones whose
    // events are already signaled are batch-read here (per-distinct-event
    // readiness gate), and a failed batch hands the graph clones back to the
    // legacy path for this sweep only.
    ZeBatchQkt batch;
    const bool batchqkt = BatchQktActive();
    bool batchqkt_legacy = false;
    if (batchqkt) {
      // v3: guard the whole flush window (collect + execute + consume) — see
      // the note on uni_batchqkt_in_batch_ above.
      uni_batchqkt_in_batch_ = true;
      // E6-v4b: emit a batch the drain deferred before collecting fresh clones
      // (teardown must not leave a deferred clone behind its released events).
      BatchQktConsumeDeferredEmit(kids, /*at_drain=*/false);
      // E6-v4c: a poll sweep may have armed this step's read. Resolve it here
      // — the graph's events are released right after this sweep, and a live
      // device op must never outlive them. The clones are then consumed inline
      // from the held packets (same tail the readiness-gated batch below
      // uses). Returns false — nothing armed, or the sync failed and the slot
      // was released — in which case the exact v3/v4b collect+execute runs.
      if (!BatchQktCompletePollRead(batch)) {
        BatchQktCollectReady(local_device_submissions_.commands_submitted_, nullptr, batch);
        if (!batch.cmds.empty() && !BatchQktExecute(batch)) {
          batchqkt_legacy = true;
        }
      }
    }

    auto it = local_device_submissions_.commands_submitted_.begin();
    while (it != local_device_submissions_.commands_submitted_.end()) {
      ZeCommand *command = *it;

      bool processed = false;
      bool deferred = false;
      const ze_kernel_timestamp_result_t *pre_ts = BatchQktTake(batch, command);
      if (pre_ts != nullptr) {
        // T6'/E6: batch-read packet; inline consumption on the legacy tail.
        if (kids != nullptr) {
          kids->push_back(command->instance_id_);
        }
        ProcessCommandSubmittedTail(local_device_submissions_, command, true, pre_ts);
        processed = true;
      }
      else if (BatchQktAccumulate(command, batchqkt && !batchqkt_legacy)) {
        // T6'/E6 (v3): stays queued for the next flush.
      }
      else if ((command->device_global_timestamps_ != nullptr) || (command->timestamps_on_event_reset_!= nullptr)) {
        if (command->timestamp_event_ != nullptr &&  // HARDEN: null event from a failed pool creation never reaches the driver
    ZE_FUNC(zeEventQueryStatus)(command->timestamp_event_) == ZE_RESULT_SUCCESS) {
          // Fix B: legacy host-buffer timestamps stay on the inline path.
          ProcessCommandSubmitted(local_device_submissions_, command, kids, false);
          processed = true;
        }
      }
      else {
        if (ZE_FUNC(zeEventQueryStatus)(command->event_) == ZE_RESULT_SUCCESS) {
          deferred = HandleCommandSubmitted(local_device_submissions_, command, kids, true);
          processed = true;
        }
      }
      if (processed) {
        // event_cache_.ReleaseEvent(command->event_) or event_cache_.ResetEvent(command->event_) is already called inside ProcessCommandSubmitted()
        // Fix B: a deferred command belongs to the completer thread — no recycling.
        if (!deferred) {
          local_device_submissions_.commands_free_pool_.push_back(command);
        }
        it = local_device_submissions_.commands_submitted_.erase(it);
        continue;
      }
      ++it;
    }
    if (batchqkt) {
      uni_batchqkt_in_batch_ = false;  // end of the flush window
    }
    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    global_device_submissions_mutex_.unlock_shared();
    FlushGraphEventResets();  // Graph replay fix: end-of-sweep deferred resets
    // Fix B barrier: callers of this path destroy/reset command lists right
    // after the sweep; the completer thread must be done with every queued
    // command before those handles go away.
    FlushDeferredTimestamps();
  }

  void ProcessAllCommandsSubmitted(std::vector<uint64_t> *kids, bool at_gexp = false) {
    if (uni_batchqkt_in_batch_) {
      return;  // T6'/E6: re-entry from the batch's own L0 calls (no-op, same pattern as Fix B)
    }

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    // Fix B barrier (entry): already-queued commands must be fully processed
    // before this drain runs, so the completion state this sweep observes is
    // final (graph replay staging relies on that).
    { UniPhaseTimer ph_d1(uni_pd_flush1_us_, uni_pd_flush1_n_); FlushDeferredTimestamps(); }
    { UniPhaseTimer ph_d2(uni_pd_lock_us_, uni_pd_lock_n_); global_device_submissions_mutex_.lock(); }

    // T6'/E6 (v3): THE per-replay flush point. Graph replay staging runs this
    // drain after host-synchronizing every list with in-flight clones of the
    // graph, so the previous replay's accumulated clones are all complete
    // here and leave in ONE device-side batch (~500us per step) instead of
    // the ~800 per-event queries the app's poll used to pay. Clones whose
    // event is not yet signaled stay queued for the next flush (per-distinct
    // -event readiness gate keeps the not-ready hot spin out); a failed batch
    // hands the graph clones back to the legacy path for this sweep only.
    ZeBatchQkt batch;
    const bool batchqkt = BatchQktActive();
    bool batchqkt_legacy = false;
    if (batchqkt) {
      // v3: guard the whole flush window — the tail resets inside the
      // consuming loop below must not re-enter a sweep while this thread
      // holds the exclusive submission lock (see uni_batchqkt_in_batch_).
      uni_batchqkt_in_batch_ = true;
      // E6-v4b backstop: emit whatever the previous drain deferred before this
      // one arms a new batch (the emit buffer would be overwritten) or stages
      // the next replay. This is what bounds a deferred clone's life when the
      // app never polls between steps: the emit then simply happens here, on
      // the v4 path, and nothing is lost — only the overlap is missed.
      { UniPhaseTimer ph_d3(uni_pd_consume_us_, uni_pd_consume_n_);
        BatchQktConsumeDeferredEmit(kids, /*at_drain=*/true); }
      // E6-v4c: prefer finishing a poll-armed read. Its device-side wait
      // resolved when the step's last kernel signaled, and the list
      // host-synchronize in PrepareGraphExecution above already proved the
      // step complete, so the synchronize here returns immediately and the
      // drain's exposed serial no longer contains the append+sync of the read
      // (gq4: max_batch_us 7042). What it produces is exactly the batch a v4b
      // BatchQktExecute would have, so the arm below — and everything after it
      // — is the unchanged v4b code. With the gate off, or when no sweep armed
      // a read (app never polled mid-step, arm skipped, or the sync failed),
      // this is the untouched v4b drain read.
      { UniPhaseTimer ph_d4(uni_pd_pollread_us_, uni_pd_pollread_n_);
      if (!BatchQktCompletePollRead(batch)) {
        BatchQktCollectStatusGated(batch);
        if (!batch.cmds.empty()) {
          if (BatchQktExecute(batch)) {
            // E6-v4b: the packets are read and safely buffered — park the emit
            // for the first sweep after this drain (the app's poll of the
            // replayed graph, which fires while the device executes the new
            // step) instead of emitting ~770 records here against an idle
            // device. The readiness gate plus the host-synchronize above proved
            // every clone in the batch complete, so the read is the last moment
            // the packets are guaranteed to still be this generation's.
            if (BatchQktAtPollActive()) {
              uni_batchqkt_qkt_at_drain_.fetch_add(batch.cmds.size(),
                                                   std::memory_order_relaxed);
            }
            BatchQktArmDeferredEmit(batch);
          }
          else {
            batchqkt_legacy = true;  // unchanged v3 fallback chain
          }
        }
      }
      else {
        // The armed set is emitted at the next poll, exactly like a v4b batch.
        BatchQktArmDeferredEmit(batch);
        // E6-v4c: this drain may run while OTHER threads' lists still hold
        // pending clones the armed read (which covers only the arming sweep's
        // own list) never saw. They must not outlive this drain: the clone
        // loop below re-signals their physical events, which would overwrite
        // packets that are still unread (silent cross-generation records, no
        // TSBAD). Collect what is left — the just-armed clones are invisible
        // to the collect (deferred_ts_), so this is exactly the residue — and
        // let the loop below consume it INLINE (the v3 shape: read + emit here
        // instead of a second deferred arm, which the single emit buffer of
        // the v4b split does not have a slot for). Leftovers only exist in
        // multi-threaded graph replay; the single-replay app pays one empty
        // collect walk here.
        BatchQktCollectStatusGated(batch);
        if (!batch.cmds.empty()) {
          if (BatchQktExecute(batch)) {
            if (BatchQktAtPollActive()) {
              uni_batchqkt_qkt_at_drain_.fetch_add(batch.cmds.size(),
                                                   std::memory_order_relaxed);
            }
          }
          else {
            batchqkt_legacy = true;  // unchanged v3 fallback chain for the residue
          }
        }
      }
      }
    }

    { UniPhaseTimer ph_d5(uni_pd_loop_us_, uni_pd_loop_n_);
    uint64_t pd_outer = 0, pd_iter = 0, pd_take = 0, pd_acc = 0, pd_lq = 0;
    if (global_device_submissions_) {
      for (auto s : *global_device_submissions_) {
        pd_outer++;
        auto& local_submissions = *s;
        auto it = local_submissions.commands_submitted_.begin();
        while (it != local_submissions.commands_submitted_.end()) {
          pd_iter++;
          ZeCommand *command = *it;

          bool processed = false;
          bool deferred = false;
          const ze_kernel_timestamp_result_t *pre_ts = BatchQktTake(batch, command);
          if (pre_ts != nullptr) {
            pd_take++;
            // T6'/E6: readiness was proven by the collection pass and the
            // packet is already read; inline consumption on the legacy tail.
            if (kids != nullptr) {
              kids->push_back(command->instance_id_);
            }
            ProcessCommandSubmittedTail(local_submissions, command, true, pre_ts);
            processed = true;
          }
          else if (BatchQktAccumulate(command, batchqkt && !batchqkt_legacy)) {
            pd_acc++;
            // T6'/E6 (v3): not ready (or not part of) this flush — stays
            // queued for the next one instead of being consumed here.
          }
          else if ((command->device_global_timestamps_ != nullptr) || (command->timestamps_on_event_reset_ != nullptr)) {
            if (command->timestamp_event_ != nullptr &&  // HARDEN: null event from a failed pool creation never reaches the driver
    ZE_FUNC(zeEventQueryStatus)(command->timestamp_event_) == ZE_RESULT_SUCCESS) {
              pd_lq++;
              // Fix B: legacy host-buffer timestamps stay on the inline path.
              ProcessCommandSubmitted(local_submissions, command, kids, false);
              processed = true;
            }
          }
          else {
            if (ZE_FUNC(zeEventQueryStatus)(command->event_) == ZE_RESULT_SUCCESS) {
              deferred = HandleCommandSubmitted(local_submissions, command, kids, true);
              processed = true;
            }
          }
          if (processed) {
            // event_cache_.ReleaseEvent(command->event_) or event_cache_.ResetEvent(command->event_) is already called inside ProcessCommandSubmitted()
            // Fix B: a deferred command belongs to the completer thread — no recycling.
            if (!deferred) {
              local_submissions.commands_free_pool_.push_back(command);
            }
            it = local_submissions.commands_submitted_.erase(it);
            continue;
          }
          ++it;
        }
        if (options_.metric_query) {
          ProcessCommandMetricQueriesSubmitted();
        }
      }
    }
    if (at_gexp) {
      // TAX-OPT drain2: one store per drain (single submit thread, one drain
      // per step) -- snapshot the loop-shape counters for the meta record.
      uni_pd_outer_n_.store(pd_outer, std::memory_order_relaxed);
      uni_pd_iter_n_.store(pd_iter, std::memory_order_relaxed);
      uni_pd_take_n_.store(pd_take, std::memory_order_relaxed);
      uni_pd_acc_n_.store(pd_acc, std::memory_order_relaxed);
      uni_pd_lq_n_.store(pd_lq, std::memory_order_relaxed);
    }
    if (batchqkt) {
      uni_batchqkt_in_batch_ = false;  // end of the flush window
    }
    global_device_submissions_mutex_.unlock();
    }
    { UniPhaseTimer ph_d6(uni_pd_resets_us_, uni_pd_resets_n_);
    FlushGraphEventResets(); }  // Graph replay fix: end-of-sweep deferred resets
    // Fix B barrier (exit): this drain may just have handed commands to the
    // completer thread; callers (context/list teardown, graph replay staging,
    // final flush) assume everything is done when this returns.
    // TAX-OPT (deferred-ts drain tail): the gexp staging drain SKIPS this
    // barrier. The commands it deferred (the eager commands whose events the
    // poll sweeps saw not-yet-signaled) keep completing on the completer
    // thread while the next replay executes (~38ms of overlap on node3);
    // the next drain's ENTRY barrier (FlushDeferredTimestamps above) is the
    // correctness point, and every non-gexp caller (teardown, finalize,
    // fence backstops) still gets the full wait. Measured on node3 trim24L:
    // the exit barrier waited 261us/step for the completer's tail while the
    // entry barrier of the next drain was ~1us.
    if (!at_gexp) {
      { UniPhaseTimer ph_d7(uni_pd_flush2_us_, uni_pd_flush2_n_); FlushDeferredTimestamps(); }
    }
  }

  void FinalizeDeviceSubmissions(std::vector<uint64_t> *kids) {

    // Do not acquire any locks!
    auto it = local_device_submissions_.commands_submitted_.begin();
    while (it != local_device_submissions_.commands_submitted_.end()) {
      ZeCommand *command = *it;

      bool processed = false;
      bool deferred = false;
      // E6-v4b: a clone whose packet the drain already read emits from the
      // held packet here too — never a fresh query, never a second event
      // release (the event was released at arm time).
      const ze_kernel_timestamp_result_t *held_ts = command->deferred_ts_;
      command->deferred_ts_ = nullptr;
      if (held_ts != nullptr) {
        if (kids != nullptr) {
          kids->push_back(command->instance_id_);
        }
        ProcessCommandSubmittedTail(local_device_submissions_, command, true, held_ts,
                                    /*deferred_emit=*/true);
        processed = true;
      }
      else if ((command->device_global_timestamps_ != nullptr) || (command->timestamps_on_event_reset_ != nullptr)) {
        if (command->timestamp_event_ != nullptr &&  // HARDEN: null event from a failed pool creation never reaches the driver
    ZE_FUNC(zeEventQueryStatus)(command->timestamp_event_) == ZE_RESULT_SUCCESS) {
          // Fix B: legacy host-buffer timestamps stay on the inline path.
          ProcessCommandSubmitted(local_device_submissions_, command, kids, false);
          processed = true;
        }
      }
      else {
        if (ZE_FUNC(zeEventQueryStatus)(command->event_) == ZE_RESULT_SUCCESS) {
          deferred = HandleCommandSubmitted(local_device_submissions_, command, kids, true);
          processed = true;
        }
      }
      if (processed) {
        // event_cache_.ReleaseEvent(command->event_) or event_cache_.ResetEvent(command->event_) is already called inside ProcessCommandSubmitted()
        // Fix B: a deferred command belongs to the completer thread — no recycling.
        if (!deferred) {
          local_device_submissions_.commands_free_pool_.push_back(command);
        }
        it = local_device_submissions_.commands_submitted_.erase(it);
        continue;
      }
      ++it;
    }
    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    FlushGraphEventResets();  // Graph replay fix: end-of-sweep deferred resets
    FlushDeferredTimestamps();  // Fix B barrier: this is a final per-thread drain
  }

 private: // Implementation

  ZeCollector(
      CollectorOptions options,
      OnZeKernelFinishCallback kcallback,
      OnZeFunctionFinishCallback fcallback,
      OnZeMetaRecordCallback mcallback,
      void* /* callback_data */,
      std::string& data_dir_name,
      bool reset_event_on_device,
      const std::vector<std::string>& include_kernels = {},
      const std::vector<std::string>& exclude_kernels = {})
      : logger_factory_(LoggerFactory::Create()),
        options_(options),
        kcallback_(kcallback),
        fcallback_(fcallback),
        mcallback_(mcallback),
        reset_event_on_device_(reset_event_on_device),
        event_cache_(ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP),
        include_kernels_(include_kernels),
        exclude_kernels_(exclude_kernels),
        uni_defer_ts_enabled_(DeferredTsEnvEnabled()),
        uni_batchqkt_enabled_(BatchQktEnvEnabled()) {
    data_dir_name_ = data_dir_name;
    // Create loggers using the factory
    if (options_.call_logging) {
      logger_ = logger_factory_->GetLogger(LOGGER_TYPE_TRACE_CALL_LOGGING);
    }
    if (options_.device_timeline) {
      logger_device_timeline_ = logger_factory_->GetLogger(LOGGER_TYPE_TRACE_DEVICE_TIMELINE);
    }
    EnumerateAndSetupDevices();
    InitializeKernelCommandProperties();
  }

  void InitializeKernelCommandProperties(void) {
    kernel_command_properties_mutex_.lock();
    if (active_command_properties_ == nullptr) {
      active_command_properties_ = new std::map<uint64_t, ZeKernelCommandProperties>;
      UniMemory::ExitIfOutOfMemory((void *)(active_command_properties_));
    }
    if (active_kernel_properties_ == nullptr) {
      active_kernel_properties_ = new std::map<ze_kernel_handle_t, ZeKernelCommandProperties>;
      UniMemory::ExitIfOutOfMemory((void *)(active_kernel_properties_));
    }
    if (kernel_command_properties_ == nullptr) {
      kernel_command_properties_ = new std::map<uint64_t, ZeKernelCommandProperties>;
      UniMemory::ExitIfOutOfMemory((void *)(kernel_command_properties_));
    }

    for (uint32_t i = 0; i <= uint32_t(ZeDeviceCommandHandle::LastCommand); i++) {
      ZeKernelCommandProperties desc;

      desc.name_ = device_command_names[i];
      desc.id_ = UniKernelId::GetKernelId();
      if (i < uint32_t(ZeDeviceCommandHandle::Barrier)) {
        desc.type_ = KERNEL_COMMAND_TYPE_MEMORY;
      }
      else {
        desc.type_ = KERNEL_COMMAND_TYPE_COMMAND;
      }

      ZeKernelCommandProperties desc2;
      desc2 = desc;

      active_command_properties_->insert({uint64_t(i), std::move(desc)});
      kernel_command_properties_->insert({desc2.id_, std::move(desc2)});
    }
    kernel_command_properties_mutex_.unlock();
  }

  void EnumerateAndSetupDevices() {
    if (devices_ == nullptr) {
      devices_ = new std::map<ze_device_handle_t, ZeDevice>;
      UniMemory::ExitIfOutOfMemory((void *)(devices_));
    }

    ze_result_t status = ZE_RESULT_SUCCESS;
    uint32_t num_drivers = 0;
    status = ZE_FUNC(zeDriverGet)(&num_drivers, nullptr);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[ERROR] Unable to get driver" << std::endl;
      exit(-1);
    }

    if (num_drivers > 0) {
      int32_t did = 0;
      std::vector<ze_driver_handle_t> drivers(num_drivers);
      std::vector<ze_context_handle_t> contexts;
      status = ZE_FUNC(zeDriverGet)(&num_drivers, drivers.data());
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[ERROR] Unable to get driver" << std::endl;
        exit(-1);
      }

      for (auto driver : drivers) {
        ze_context_handle_t context = nullptr;
        if (options_.metric_query) {
          ze_context_desc_t cdesc = {ZE_STRUCTURE_TYPE_CONTEXT_DESC, nullptr, 0};

          status = ZE_FUNC(zeContextCreate)(driver, &cdesc, &context);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Unable to create context for metrics" << std::endl;
            exit(-1);
          }
          metric_contexts_.push_back(context);
        }

        uint32_t num_devices = 0;
        status = ZE_FUNC(zeDeviceGet)(driver, &num_devices, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Unable to get device" << std::endl;
          num_devices = 0;
        }
        if (num_devices) {
          std::vector<ze_device_handle_t> devices(num_devices);
          status = ZE_FUNC(zeDeviceGet)(driver, &num_devices, devices.data());
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[WARNING] Unable to get device" << std::endl;
            devices.clear();
          }
          for (auto device : devices) {
            ZeDevice desc;

            desc.device_ = device;
            desc.id_ = did;
            desc.parent_id_ = -1;  // no parent
            desc.parent_device_ = nullptr;
            desc.subdevice_id_ = -1;  // not a subdevice

            ze_device_properties_t props{ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES_1_2, };
            ze_result_t status = ZE_FUNC(zeDeviceGetProperties)(device, &props);
            PTI_ASSERT(status == ZE_RESULT_SUCCESS);
            PTI_ASSERT(props.timerResolution != 0);

            desc.device_timer_frequency_ = props.timerResolution;
            desc.device_timer_mask_ = (props.kernelTimestampValidBits == 64) ? (std::numeric_limits<uint64_t>::max)() : ((1ull << props.kernelTimestampValidBits) - 1ull);
            desc.device_ns_per_cycle_ = static_cast<double>(NSEC_IN_SEC) / static_cast<double>(props.timerResolution);

            ze_pci_ext_properties_t pci_device_properties;
            status = ZE_FUNC(zeDevicePciGetPropertiesExt)(device, &pci_device_properties);
            if (status != ZE_RESULT_SUCCESS) {
              std::cerr << "[WARNING] Unable to get device PCI properties" << std::endl;
              memset(&pci_device_properties, 0, sizeof(pci_device_properties));  // dummy device properties
            }
            desc.pci_properties_ = pci_device_properties;

            desc.driver_ = driver;
            desc.context_ = context;

            uint32_t num_sub_devices = 0;
            status = ZE_FUNC(zeDeviceGetSubDevices)(device, &num_sub_devices, nullptr);

            if (status != ZE_RESULT_SUCCESS) {
              std::cerr << "[WARNING] Unable to get sub-devices" << std::endl;
              desc.num_subdevices_ = 0;
            }
            else {
              desc.num_subdevices_ = num_sub_devices;
            }

            if (options_.metric_query) {
              uint32_t num_groups = 0;
              zet_metric_group_handle_t group = nullptr;
              status = ZE_FUNC(zetMetricGroupGet)(device, &num_groups, nullptr);
              if (status != ZE_RESULT_SUCCESS) {
                std::cerr << "[ERROR] Unable to get metric group" << std::endl;
                exit(-1);
              }
              if (num_groups > 0) {
                std::vector<zet_metric_group_handle_t> groups(num_groups, nullptr);
                status = ZE_FUNC(zetMetricGroupGet)(device, &num_groups, groups.data());
                if (status != ZE_RESULT_SUCCESS) {
                  std::cerr << "[ERROR] Unable to get metric group" << std::endl;
                  exit(-1);
                }

                for (uint32_t k = 0; k < num_groups; ++k) {
                  zet_metric_group_properties_t group_props{};
                  group_props.stype = ZET_STRUCTURE_TYPE_METRIC_GROUP_PROPERTIES;
                  status = ZE_FUNC(zetMetricGroupGetProperties)(groups[k], &group_props);
                  if (status != ZE_RESULT_SUCCESS) {
                    std::cerr << "[ERROR] Unable to get metric group properties" << std::endl;
                    exit(-1);
                  }

                  if ((strcmp(group_props.name, utils::GetEnv("UNITRACE_MetricGroup").c_str()) == 0) && (group_props.samplingType & ZET_METRIC_GROUP_SAMPLING_TYPE_FLAG_EVENT_BASED)) {
                    group = groups[k];
                    break;
                  }
                }

                if (group == nullptr) {
                  std::cerr << "[ERROR] Unable to get metric group " << utils::GetEnv("UNITRACE_MetricGroup") << ". Please make sure the metric group is valid and supported" << std::endl;
                  exit(-1);
                }
              }
              else {
                std::cerr << "[ERROR] Unable to get metric group " << utils::GetEnv("UNITRACE_MetricGroup") << ". Please make sure the metric group is valid and supported" << std::endl;
                exit(-1);
              }

              status = ZE_FUNC(zetContextActivateMetricGroups)(context, device, 1, &group);
              if (status != ZE_RESULT_SUCCESS) {
                std::cerr << "[ERROR] Unable to activate metric groups" << std::endl;
                exit(-1);
              }
              metric_activations_.insert({context, device});

              desc.metric_group_ = group;
            }
            else {
              desc.metric_group_ = nullptr;
            }

            uint64_t host_time;
            uint64_t ticks;

            status = ZE_FUNC(zeDeviceGetGlobalTimestamps)(device, &host_time, &ticks);
            if (status != ZE_RESULT_SUCCESS) {
              std::cerr << "[ERROR] Unable to get global timestamps" << std::endl;
              exit(-1);
            }

            desc.host_time_origin_ = host_time;

            // Get device name
            ze_device_properties_t device_properties = {};
            device_properties.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
            status = ZE_FUNC(zeDeviceGetProperties)(device, &device_properties);
            if (status == ZE_RESULT_SUCCESS) {
              desc.device_name_ = std::move(std::string(device_properties.name));
            } else {
              desc.device_name_ = "";
              std::cerr << "[WARNING] zeDeviceGetProperties failed with error code : " << status << std::endl;
            }

            // query group ordinal
            uint32_t count = 0;
            status = ZE_FUNC(zeDeviceGetCommandQueueGroupProperties)(device, &count, nullptr);
            if (status != ZE_RESULT_SUCCESS || count == 0) {
              std::cerr << "[WARNING] zeDeviceGetCommandQueueGroupProperties failed with error code : " << status << std::endl;
            } else {
              std::vector<ze_command_queue_group_properties_t> props(count);
              status = ZE_FUNC(zeDeviceGetCommandQueueGroupProperties)(device, &count, props.data());
              if (status != ZE_RESULT_SUCCESS) {
                std::cerr << "[WARNING] zeDeviceGetCommandQueueGroupProperties failed with error code : " << status << std::endl;
              } else {
                desc.queue_engine_prop_.resize(count);
                for (uint32_t i = 0; i < count; ++i) {
                  if (props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE) {
                    desc.queue_engine_prop_[i] = ZE_QUEUE_COMPUTE_ENGINE;
                  } else if (props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) {
                    desc.queue_engine_prop_[i] = ZE_QUEUE_COPY_ENGINE;
                  } else {
                    desc.queue_engine_prop_[i] = ZE_QUEUE_UNKNOWN_ENGINE;
                  }
                }
              }
            }

            devices_->insert({device, std::move(desc)});

            if (num_sub_devices > 0) {
              std::vector<ze_device_handle_t> sub_devices(num_sub_devices);

              status = ZE_FUNC(zeDeviceGetSubDevices)(device, &num_sub_devices, sub_devices.data());
              if (status != ZE_RESULT_SUCCESS) {
                std::cerr << "[WARNING] Unable to get sub-devices" << std::endl;
                num_sub_devices = 0;
              }

              for (uint32_t j = 0; j < num_sub_devices; j++) {
                ZeDevice sub_desc;

                sub_desc.device_ = sub_devices[j];
                sub_desc.parent_id_ = did;
                sub_desc.parent_device_ = device;
                sub_desc.num_subdevices_ = 0;
                sub_desc.subdevice_id_ = j;
                sub_desc.id_ = did;  // take parent device's id

                ze_device_properties_t props{ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES_1_2, };
                ze_result_t status = ZE_FUNC(zeDeviceGetProperties)(sub_devices[j], &props);
                PTI_ASSERT(status == ZE_RESULT_SUCCESS);
                PTI_ASSERT(props.timerResolution != 0);

                sub_desc.device_timer_frequency_ = props.timerResolution;
                sub_desc.device_timer_mask_ = (props.kernelTimestampValidBits == 64) ? (std::numeric_limits<uint64_t>::max)() : ((1ull << props.kernelTimestampValidBits) - 1ull);
                sub_desc.device_ns_per_cycle_ = static_cast<double>(NSEC_IN_SEC) / static_cast<double>(props.timerResolution);

                ze_pci_ext_properties_t pci_device_properties;
                status = ZE_FUNC(zeDevicePciGetPropertiesExt)(sub_devices[j], &pci_device_properties);
                if (status != ZE_RESULT_SUCCESS) {
                  std::cerr << "[WARNING] Unable to get device PCI properties" << std::endl;
                  memset(&pci_device_properties, 0, sizeof(pci_device_properties)); // dummy device properties
                }
                sub_desc.pci_properties_ = pci_device_properties;

                uint64_t ticks;
                uint64_t host_time;
                status = ZE_FUNC(zeDeviceGetGlobalTimestamps)(sub_devices[j], &host_time, &ticks);
                if (status != ZE_RESULT_SUCCESS) {
                  std::cerr << "[ERROR] Unable to get global timestamps" << std::endl;
                  exit(-1);
                }

                sub_desc.host_time_origin_ = host_time;

                sub_desc.driver_ = driver;
                sub_desc.context_ = context;

                sub_desc.metric_group_ = nullptr;

                // Get sub-device name
                ze_device_properties_t device_properties = {};
                device_properties.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
                status = ZE_FUNC(zeDeviceGetProperties)(sub_devices[j], &device_properties);
                if (status == ZE_RESULT_SUCCESS) {
                  sub_desc.device_name_ = std::move(std::string(device_properties.name));
                } else {
                  sub_desc.device_name_ = "";
                  std::cerr << "[WARNING] zeDeviceGetProperties failed with error code : " << status << std::endl;
                }

                // query group ordinal
                count = 0;
                status = ZE_FUNC(zeDeviceGetCommandQueueGroupProperties)(sub_devices[j], &count, nullptr);
                if (status != ZE_RESULT_SUCCESS || count == 0) {
                  std::cerr << "[WARNING] zeDeviceGetCommandQueueGroupProperties failed with error code : " << status << std::endl;
                } else {
                  std::vector<ze_command_queue_group_properties_t> props(count);
                  status = ZE_FUNC(zeDeviceGetCommandQueueGroupProperties)(sub_devices[j], &count, props.data());
                  if (status != ZE_RESULT_SUCCESS) {
                    std::cerr << "[WARNING] zeDeviceGetCommandQueueGroupProperties failed with error code : " << status << std::endl;
                  } else {
                    sub_desc.queue_engine_prop_.resize(count);
                    for (uint32_t i = 0; i < count; ++i) {
                      if (props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE) {
                        sub_desc.queue_engine_prop_[i] = ZE_QUEUE_COMPUTE_ENGINE;
                      } else if (props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY) {
                        sub_desc.queue_engine_prop_[i] = ZE_QUEUE_COPY_ENGINE;
                      } else {
                        sub_desc.queue_engine_prop_[i] = ZE_QUEUE_UNKNOWN_ENGINE;
                      }
                    }
                  }
                }

                devices_->insert({sub_devices[j], std::move(sub_desc)});
              }
            }
            did++;
          }
        }
      }
    }
  }

  static std::string PrintTypedValue(const zet_typed_value_t& typed_value) {
    switch (typed_value.type) {
      case ZET_VALUE_TYPE_UINT32:
        return std::to_string(typed_value.value.ui32);
      case ZET_VALUE_TYPE_UINT64:
        return std::to_string(typed_value.value.ui64);
      case ZET_VALUE_TYPE_FLOAT32:
        return std::to_string(typed_value.value.fp32);
      case ZET_VALUE_TYPE_FLOAT64:
        return std::to_string(typed_value.value.fp64);
      case ZET_VALUE_TYPE_BOOL8:
        return std::to_string(static_cast<uint32_t>(typed_value.value.b8));
      default:
        PTI_ASSERT(0);
        break;
    }
    return "";   //in case of error returns empty string.
  }

  inline static std::string GetMetricUnits(const char* units) {
    PTI_ASSERT(units != nullptr);

    std::string result = units;
    if (result.find("null") != std::string::npos) {
      result = "";
    } else if (result.find("percent") != std::string::npos) {
      result = "%";
    }

    return result;
  }

  static uint32_t GetMetricCount(zet_metric_group_handle_t group) {
    PTI_ASSERT(group != nullptr);

    zet_metric_group_properties_t group_props{};
    group_props.stype = ZET_STRUCTURE_TYPE_METRIC_GROUP_PROPERTIES;
    ze_result_t status = ZE_FUNC(zetMetricGroupGetProperties)(group, &group_props);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[ERROR] Failed to get metric group properties (status = 0x" << std::hex << status << std::dec << ")." << std::endl;
      exit(-1);
    }

    return group_props.metricCount;
  }

  static std::vector<std::string> GetMetricNames(zet_metric_group_handle_t group) {
    PTI_ASSERT(group != nullptr);

    uint32_t metric_count = GetMetricCount(group);
    PTI_ASSERT(metric_count > 0);

    std::vector<zet_metric_handle_t> metrics(metric_count);
    ze_result_t status = ZE_FUNC(zetMetricGet)(group, &metric_count, metrics.data());
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    PTI_ASSERT(metric_count == metrics.size());

    std::vector<std::string> names;
    for (auto metric : metrics) {
      zet_metric_properties_t metric_props{
          ZET_STRUCTURE_TYPE_METRIC_PROPERTIES, };
      status = ZE_FUNC(zetMetricGetProperties)(metric, &metric_props);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      std::string units = GetMetricUnits(metric_props.resultUnits);
      std::string name = metric_props.name;
      if (!units.empty()) {
        name += "[" + units + "]";
      }
      names.push_back(std::move(name));
    }

    return names;
  }

  bool QueryKernelCommandMetrics(ZeDeviceSubmissions& submissions, ZeCommandMetricQuery *command_metric_query, bool collect_data) {

    ze_result_t status;
    if ((status = ZE_FUNC(zeEventQueryStatus)(command_metric_query->metric_query_event_)) == ZE_RESULT_SUCCESS) {

      auto it = submissions.kernel_profiles_.find(command_metric_query->instance_id_);
      if (it != submissions.kernel_profiles_.end()) {
        if (collect_data) {
          size_t size = 0;
          status = ZE_FUNC(zetMetricQueryGetData)(command_metric_query->metric_query_, &size, nullptr);
          if ((status == ZE_RESULT_SUCCESS) && (size > 0)) {

            std::vector<uint8_t> *kmetrics = new std::vector<uint8_t>(size);
            UniMemory::ExitIfOutOfMemory((void *)(kmetrics));
            size_t size2 = size;
            status = ZE_FUNC(zetMetricQueryGetData)(command_metric_query->metric_query_, &size2, kmetrics->data());
            if (size2 == size) {
              it->second.metrics_ = kmetrics;
            }
            else {
              delete kmetrics;
            }
          }
        }
      } else {
        return false;
      }
      event_cache_.ResetEvent(command_metric_query->metric_query_event_);
      query_pools_.ResetQuery(command_metric_query->metric_query_);
      if (command_metric_query->immediate_) {
        event_cache_.ReleaseEvent(command_metric_query->metric_query_event_);
        query_pools_.PutQuery(command_metric_query->metric_query_);
      }
      command_metric_query->metric_query_event_ = nullptr;
      command_metric_query->metric_query_ = nullptr;

      return true;
    }
    else {
      return false;
    }
  }

  void ProcessCommandMetricQueriesSubmitted(void) {
    // This runs while a caller (ProcessAllCommandsSubmitted/ProcessCommandsSubmitted)
    // holds global_device_submissions_mutex_. Querying the collection state must
    // not synchronously fire the session-stopped flush here: OnSessionStopped() ->
    // UniTracer::Flush() -> FlushData() -> ProcessAllCommandsSubmitted() would
    // re-enter the collector and deadlock on that non-recursive mutex
    // (std::system_error / EDEADLK). Defer the notification so it fires from a safe
    // top-level call site (the next OnEnter*Append* callback) instead.
    bool collect_data = UniController::IsCollectionEnabled(/*defer_callback=*/true);
    for (auto it = local_device_submissions_.metric_queries_submitted_.begin(); it != local_device_submissions_.metric_queries_submitted_.end();) {
      if (QueryKernelCommandMetrics(local_device_submissions_, *it, collect_data)) {
          local_device_submissions_.metric_queries_free_pool_.push_back(*it);
          it = local_device_submissions_.metric_queries_submitted_.erase(it);
      }
      else {
        it++;
      }
    }
  }

  void DumpKernelProfiles(void) {

    if (options_.stall_sampling) {
      kernel_command_properties_mutex_.lock();
      std::map<int32_t, std::map<uint64_t, ZeKernelCommandProperties *>> device_kprops; // sorted by device id then base address;
      for (auto it = kernel_command_properties_->begin(); it != kernel_command_properties_->end(); it++) {
        if (it->second.type_ != KERNEL_COMMAND_TYPE_COMPUTE) {
          continue;
        }
        auto dkit = device_kprops.find(it->second.device_id_);
        if (dkit == device_kprops.end()) {
          std::map<uint64_t, ZeKernelCommandProperties *> kprops;
          kprops.insert({it->second.base_addr_, &(it->second)});
          device_kprops.insert({it->second.device_id_, std::move(kprops)});
        }
        else {
          if (dkit->second.find(it->second.base_addr_) != dkit->second.end()) {
            // already inserted
            continue;
          }
          dkit->second.insert({it->second.base_addr_, &(it->second)});
        }
      }

      // TODO: add optional argument to control whether to delete.
      for (auto& props : device_kprops) {
        std::shared_ptr<Logger> kpfs_logger = logger_factory_->GetDeviceLogger(LOGGER_TYPE_KPROPS, props.first, true, true);
        if (kpfs_logger == nullptr) {
              std::cerr << "[ERROR] Failed to create kernel properties data file" << std::endl;
              exit(-1);
        }
        uint64_t prev_base = 0;
        for (auto it = props.second.crbegin(); it != props.second.crend(); it++) {
          // quote kernel name which may contain ","
          kpfs_logger->Log("\"" + utils::Demangle(it->second->name_.c_str()) + "\"\n");
          kpfs_logger->Log(std::to_string(it->second->base_addr_) + "\n");
          if (prev_base == 0) {
            kpfs_logger->Log(std::to_string(it->second->size_) + "\n");
          }
          else {
            size_t size = prev_base - it->second->base_addr_;
            if (size > it->second->size_) {
              size = it->second->size_;
            }
            kpfs_logger->Log(std::to_string(size) + "\n");
          }
          prev_base = it->second->base_addr_;
        }
        kpfs_logger->Flush();
      }

      kernel_command_properties_mutex_.unlock();
    }

    const std::lock_guard<std::mutex> lock(global_kernel_profiles_mutex_);
    if (global_kernel_profiles_.size() == 0) {
      return;
    }

    if (options_.metric_stream) {
      devices_mutex_.lock_shared();
      std::map<int32_t, std::vector<ZeKernelProfileRecord *>> device_kprofiles; // kernel profiles by device;
      for (auto it = global_kernel_profiles_.begin(); it != global_kernel_profiles_.end(); it++) {
        int32_t device_id = -1;
        auto dit = devices_->find(it->second.device_);
        if (dit != devices_->end()) {
          device_id = dit->second.id_;
        }
        if (device_id == -1) {
          continue;
        }
        auto dpit = device_kprofiles.find(device_id);
        if (dpit == device_kprofiles.end()) {
          std::vector<ZeKernelProfileRecord *> kprofiles;
          kprofiles.push_back(&(it->second));
          device_kprofiles.insert({device_id, std::move(kprofiles)});
        }
        else {
          dpit->second.push_back(&(it->second));
        }
      }
      devices_mutex_.unlock_shared();

      for (auto& profiles : device_kprofiles) {
        std::shared_ptr<Logger> ktime_logger = logger_factory_->GetDeviceLogger(LOGGER_TYPE_KTIME, profiles.first, true, true);
        if (ktime_logger == nullptr) {
              std::cerr << "[ERROR] Failed to create kernel times data file" << std::endl;
              exit(-1);
        }
        for (auto& prof : profiles.second) {
          for (auto& ts : prof->timestamps_) {
            std::string kname = GetZeKernelCommandName(prof->kernel_command_id_, prof->group_count_, prof->mem_size_);
            ktime_logger->Log(std::to_string(ts.subdevice_id) + "\n");
            ktime_logger->Log(std::to_string(prof->instance_id_) + "\n");
            ktime_logger->Log(std::to_string(ts.metric_start) + "\n");
            ktime_logger->Log(std::to_string(ts.metric_end) + "\n");
            ktime_logger->Log(kname + "\n");
          }
        }
        ktime_logger->Flush();
      }

      return;
    }

    if (!options_.metric_query) {
      return;
    }

    // metric query

#ifdef _WIN32
    // On Windows, L0 may have been unloaded or be being unloaded at this point
    // So we save the metric data in a file and the saved metrics will be computed in the parent process
    // The metric data file path: <data_dir>/.metrics.<pid>.q
    // The format of each entry in the file is: device id (int32_t), size of kernel name (size_t), kernel name, instance (uin64_t), size of metric data (uint64_t), metric data

    std::string fpath = logger_factory_->GenerateLogFileName(LOGGER_TYPE_METRICS_QUERY_TEMP);
    std::ofstream mf(fpath, std::ios::binary);

    if (!mf) {
        std::cerr << "[ERROR] Failed to create metric data file" << std::endl;
        exit(-1);
    }

    while (1) {
      if (global_kernel_profiles_.empty()) {
        break;  // done
      }
      ze_device_handle_t device = nullptr;
      int32_t did = -1;
      for (auto it = global_kernel_profiles_.begin(); it != global_kernel_profiles_.end();) {
        if ((it->second.metrics_ == nullptr) || it->second.metrics_->empty() || (it->second.device_ == nullptr)) {
          // skip empty entries
          it = global_kernel_profiles_.erase(it);
          continue;
        }

        if (device == nullptr) {
          devices_mutex_.lock_shared();
          auto it2 = devices_->find(it->second.device_);

          if (it2 == devices_->end()) {
            devices_mutex_.unlock_shared();
            // should never get here
            it = global_kernel_profiles_.erase(it);
            continue;
          }

          device = it->second.device_;
          did = it2->second.id_;
          devices_mutex_.unlock_shared();
        }
        else {
          if (it->second.device_ != device) {
            it++;  // different device, dump later
            continue;
          }
        }

        std::string kname = GetZeKernelCommandName(it->second.kernel_command_id_, it->second.group_count_, it->second.mem_size_);
        if (kname.empty()) {
          // skip invalid kernels
          // should never get here
          it = global_kernel_profiles_.erase(it);
          continue;
        }

        mf.write(reinterpret_cast<char *>(&did), sizeof(int32_t));
        size_t kname_size = kname.size();
        mf.write(reinterpret_cast<char *>(&(kname_size)), sizeof(size_t));
        mf.write(kname.c_str(), kname_size);
        mf.write(reinterpret_cast<char *>(&(it->second.instance_id_)), sizeof(uint64_t));
        uint64_t metrics_size =  it->second.metrics_->size();
        mf.write(reinterpret_cast<char *>(&(metrics_size)), sizeof(uint64_t));
        mf.write(reinterpret_cast<char *>(it->second.metrics_->data()), it->second.metrics_->size());
        it = global_kernel_profiles_.erase(it);
      }
    }

    mf.close();

#else /* LINUX */

    std::shared_ptr<Logger> metric_logger = nullptr;

    while (1) {
      if (global_kernel_profiles_.empty()) {
        break;  // done
      }
      ze_device_handle_t device = nullptr;
      int did = -1;
      zet_metric_group_handle_t group = nullptr;
      std::vector<std::string> metric_names;
      for (auto it = global_kernel_profiles_.begin(); it != global_kernel_profiles_.end();) {
        if ((it->second.metrics_ == nullptr) || it->second.metrics_->empty()) {
          it = global_kernel_profiles_.erase(it);
          continue;
        }

        if (it->second.device_ == nullptr) {
          // shoule never get here
          it = global_kernel_profiles_.erase(it);
          continue;
        }

        if (device == nullptr) {
          devices_mutex_.lock_shared();
          auto it2 = devices_->find(it->second.device_);

          if (it2 == devices_->end()) {
            devices_mutex_.unlock_shared();
            // should never get here
            it = global_kernel_profiles_.erase(it);
            continue;
          }

          device = it->second.device_;
          did = it2->second.id_;
          group = it2->second.metric_group_;
          metric_names = GetMetricNames(it2->second.metric_group_);
          devices_mutex_.unlock_shared();
          PTI_ASSERT(!metric_names.empty());

          metric_logger = logger_factory_->GetDeviceLogger(LOGGER_TYPE_METRICS_QUERY, did, true, true);
          if (metric_logger == nullptr) {
            std::cerr << "[ERROR] Failed to create metric query data file" << std::endl;
            exit(-1);
          }

          if (logger_factory_->IsLegacy()) {
            metric_logger->Log("\n=== Device #" + std::to_string(did) + " Metrics ===\n");
          }

          std::string header("\nKernel,GlobalInstanceId,SubDeviceId");
          for (auto& metric : metric_names) {
            header += "," + metric;
          }
          header += "\n";
          metric_logger->Log(header);
        }
        else {
          if (it->second.device_ != device) {
            it++;  // different device, dump later
            continue;
          }
        }

        std::string kname = GetZeKernelCommandName(it->second.kernel_command_id_, it->second.group_count_, it->second.mem_size_);
        uint32_t num_samples = 0;
        uint32_t num_metrics = 0;
        ze_result_t status = ZE_FUNC(zetMetricGroupCalculateMultipleMetricValuesExp)(
          group, ZET_METRIC_GROUP_CALCULATION_TYPE_METRIC_VALUES,
          it->second.metrics_->size(), it->second.metrics_->data(), &num_samples, &num_metrics,
          nullptr, nullptr);

        if ((status == ZE_RESULT_SUCCESS) && (num_samples > 0) && (num_metrics > 0)) {
          std::vector<uint32_t> samples(num_samples);
          std::vector<zet_typed_value_t> metrics(num_metrics);

          status = ZE_FUNC(zetMetricGroupCalculateMultipleMetricValuesExp)(
            group, ZET_METRIC_GROUP_CALCULATION_TYPE_METRIC_VALUES,
            it->second.metrics_->size(), it->second.metrics_->data(), &num_samples, &num_metrics,
            samples.data(), metrics.data());

          if (status == ZE_RESULT_SUCCESS) {
            std::string str;
            for (uint32_t i = 0; i < num_samples; ++i) {
              str = kname + ",";
              str += std::to_string(it->second.instance_id_) + ",";
              str += std::to_string(i);

              uint32_t size = samples[i];
              PTI_ASSERT(size == metric_names.size());

              const zet_typed_value_t *value = metrics.data() + i * size;
              for (uint32_t j = 0; j < size; ++j) {
                str += ",";
                str += PrintTypedValue(value[j]);
              }
              str += "\n";
            }
            str += "\n";

            metric_logger->Log(str);
          }
          else {
            std::cerr << "[WARNING] Not able to calculate metrics" << std::endl;
          }
        }
        else {
          std::cerr << "[WARNING] Not able to calculate metrics" << std::endl;
        }
        it = global_kernel_profiles_.erase(it);
        metric_logger->Flush();
      }
    }

    if (logger_factory_->IsLegacy() && metric_logger && metric_logger->IsLogToFile()) {
      std::cerr << "[INFO] Kernel metrics are stored in " << metric_logger->GetLogFileName() << std::endl;
    }
#endif /* _WIN32 */
  }

  void ProcessCommandsSubmittedOnSignaledEvent(ze_event_handle_t event, std::vector<uint64_t> *kids) {
    if (uni_batchqkt_in_batch_) {
      return;  // T6'/E6: re-entry from the batch's own L0 calls (no-op, same pattern as Fix B)
    }

    if (local_device_submissions_.IsFinalized()) {
      return;
    }
    global_device_submissions_mutex_.lock_shared();

    // T6'/E6 (v3): accumulate-only here. No batch is executed from a poll
    // sweep: with the clones' shared events latched until the batch resets
    // them, a matched-event trigger would re-fire on every subsequent poll
    // and re-fragment the batch, and the polled event does not prove the
    // whole replay complete (gq1b: only ~40 of ~770 clones were ready at the
    // one poll sweep per step). Instead every pending clone seen here is
    // skipped by the legacy branches below — no status call, no query, no
    // Fix B handoff — and stays queued for the ONE flush per replay at the
    // staging drain (PrepareGraphExecution host-synchronizes the lists
    // before its pre-staging ProcessAllCommandsSubmitted, which executes the
    // batch). Fence and teardown drains keep readiness-gated flushes as the
    // backstop. This is also what removes the ~51ms per-step sweep giant: the
    // poll loop no longer walks ~800 clones with per-event queries.
    const bool batchqkt = BatchQktActive();

    // E6-v4b: THIS is where the drain's batch is meant to be emitted. The
    // packets were read at the drain, while they were still this generation's
    // (before the new replay's clones re-signaled the same physical events),
    // and their completeness was proven by that drain's list host-synchronize
    // — so the emit pass below needs no readiness gate and cannot read a
    // packet twice. Running it here puts the ~58ms of record machinery under
    // the new step's device execution instead of serializing it against an
    // idle device at the drain (gq3: that placement alone was the +65ms ITL).
    // No-op unless a drain armed a batch since the last emit pass.
    if (batchqkt) {
      BatchQktConsumeDeferredEmit(kids, /*at_drain=*/false);
      // E6-v4c: THIS is also where the step's read is issued, when
      // UNITRACE_GRAPH_QKT_AT_POLL=1. Same anchor as the emit above (the first
      // sweep after the previous drain, device busy running the new step): the
      // append parks device-side on the step's own events, so the packets are
      // captured the moment the step's last kernel signals and the staging
      // drain only synchronizes + copies. Stateless w.r.t. the clones — a skip
      // or a failure here leaves the exact v4b shape in place.
      BatchQktArmPollRead();
    }

    // v3.1: parallel prefetch of the timestamps of commands bound to the waited
    // event (skipped when TSLOG2 debug is active so the diagnosis path keeps its
    // exact inline ordering; the fence twin below mirrors this).
    // T6'/E6: also skipped when the batch gate is on — pending clones are not
    // consumed from poll sweeps anymore, and those helper-thread per-event
    // queries are the tax the batch replaces (their results were never
    // consumed here anyway).
    size_t prefetch_used = 0;
    std::vector<ZeCommand *> prefetch_cmds;
    std::vector<ZePrefetchedTs> prefetch_ts;
    if (!batchqkt && Tslog2Mode() == 0) {
      size_t bound = 0;
      for (auto &command : local_device_submissions_.commands_submitted_) {
        if (command->event_ == event && command->in_order_counter_event_ == nullptr) bound++;
      }
      if (bound >= kTsPrefetchMinCommands) {
        prefetch_cmds.reserve(bound);
        for (auto &command : local_device_submissions_.commands_submitted_) {
          if (command->event_ == event && command->in_order_counter_event_ == nullptr) {
            prefetch_cmds.push_back(command);
          }
        }
        PrefetchTimestampsParallel(prefetch_cmds, prefetch_ts);
      }
    }

    for (auto it = local_device_submissions_.commands_submitted_.begin(); it != local_device_submissions_.commands_submitted_.end();) {
      ZeCommand *command = *it;

      if (command->event_ == event || command->in_order_counter_event_ == event) {
        // T6'/E6 (v3): a pending graph clone stays queued for the staging
        // -drain flush even when its event is the one this sweep was polled
        // for — the batch reads and resets it, in list order, at the drain.
        if (BatchQktAccumulate(command, batchqkt)) {
          it++;
          continue;
        }
        // Fix B: kids (host-API flow arrows) stay synchronous; the timestamp
        // read + emit + reset may be handed to the background completer thread.
        // Merge note: v3.1 prefetch consumption dropped here (proven inert for
        // counter-based events; the prefetch snapshot block above is harmless).
        if (HandleCommandSubmitted(local_device_submissions_, command, kids, true)) {
          // deferred: the command object is owned by the completer thread now
        }
        else {
          local_device_submissions_.commands_free_pool_.push_back(command);
        }
        it = local_device_submissions_.commands_submitted_.erase(it);
        continue;
      }
      else {
        bool processed = false;
        bool deferred = false;
        if (BatchQktAccumulate(command, batchqkt)) {
          // T6'/E6 (v3): stays queued for the next flush.
        }
        else if ((command->device_global_timestamps_ != nullptr) || (command->timestamps_on_event_reset_ != nullptr)) {
          if (command->timestamp_event_ != nullptr &&  // HARDEN: null event from a failed pool creation never reaches the driver
    ZE_FUNC(zeEventQueryStatus)(command->timestamp_event_) == ZE_RESULT_SUCCESS) {
            // Fix B: legacy host-buffer timestamps stay on the inline path.
            ProcessCommandSubmitted(local_device_submissions_, command, nullptr, false);
            processed = true;
          }
        }
        else {
          if (ZE_FUNC(zeEventQueryStatus)(command->event_) == ZE_RESULT_SUCCESS) {
            deferred = HandleCommandSubmitted(local_device_submissions_, command, nullptr, true);
            processed = true;
          }
        }
        if (processed) {
          // event_cache_.ReleaseEvent(command->event_) or event_cache_.ResetEvent(command->event_) is already called inside ProcessCommandSubmitted()
          // Fix B: a deferred command belongs to the completer thread — no recycling.
          if (!deferred) {
            local_device_submissions_.commands_free_pool_.push_back(command);
          }
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
      }
      it++;
    }

    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    global_device_submissions_mutex_.unlock_shared();
    FlushGraphEventResets();  // Graph replay fix: end-of-sweep deferred resets
  }

  void ProcessCommandsSubmittedOnFenceSynchronization(ze_fence_handle_t fence, std::vector<uint64_t> *kids) {

    if (uni_batchqkt_in_batch_) {
      return;  // T6'/E6: re-entry from the batch's own L0 calls (no-op, same pattern as Fix B)
    }

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    global_device_submissions_mutex_.lock_shared();

    // T6'/E6 (v3): backstop flush. The fence this sweep was triggered for may
    // not be the clones' completion object, so the accumulated clones whose
    // events are already signaled are batch-read here (per-distinct-event
    // readiness gate); the rest stay queued for the staging drain. A failed
    // batch hands the graph clones back to the legacy path for this sweep.
    ZeBatchQkt batch;
    const bool batchqkt = BatchQktActive();
    bool batchqkt_legacy = false;
    if (batchqkt) {
      // v3: guard the whole flush window — see uni_batchqkt_in_batch_.
      uni_batchqkt_in_batch_ = true;
      // E6-v4b: emit a batch the drain deferred before collecting fresh clones,
      // so a deferred clone is never re-read and never outlives this sweep.
      BatchQktConsumeDeferredEmit(kids, /*at_drain=*/false);
      // E6-v4c: resolve a poll-armed read here too (its wait resolved when the
      // step ended; a mid-step fence sync would block on it for the step's
      // remainder, which in an in-order queue the app was going to wait out
      // anyway). Consumed inline like the batch below; on skip/failure the
      // unchanged v3/v4b collect+execute runs.
      if (!BatchQktCompletePollRead(batch)) {
        BatchQktCollectReady(local_device_submissions_.commands_submitted_, nullptr, batch);
        if (!batch.cmds.empty() && !BatchQktExecute(batch)) {
          batchqkt_legacy = true;
        }
      }
    }

    // v3.1: parallel prefetch for fence-bound commands (see the event twin above).
    // T6'/E6: skipped when the batched read handled this sweep.
    size_t prefetch_used = 0;
    std::vector<ZeCommand *> prefetch_cmds;
    std::vector<ZePrefetchedTs> prefetch_ts;
    if (!batchqkt && Tslog2Mode() == 0) {
      size_t bound = 0;
      for (auto &command : local_device_submissions_.commands_submitted_) {
        if ((command->fence_ != nullptr) && (command->fence_ == fence) &&
            command->event_ != nullptr && command->in_order_counter_event_ == nullptr) bound++;
      }
      if (bound >= kTsPrefetchMinCommands) {
        prefetch_cmds.reserve(bound);
        for (auto &command : local_device_submissions_.commands_submitted_) {
          if ((command->fence_ != nullptr) && (command->fence_ == fence) &&
              command->event_ != nullptr && command->in_order_counter_event_ == nullptr) {
            prefetch_cmds.push_back(command);
          }
        }
        PrefetchTimestampsParallel(prefetch_cmds, prefetch_ts);
      }
    }

    for (auto it = local_device_submissions_.commands_submitted_.begin(); it != local_device_submissions_.commands_submitted_.end();) {
      ZeCommand *command = *it;
      if ((command->fence_ != nullptr) && (command->fence_ == fence)) {
        // T6'/E6 (v3): a pending graph clone bound to this fence either
        // consumes its batch-read packet or stays queued for the staging
        // drain — it is never re-read per event here.
        const ze_kernel_timestamp_result_t *pre_ts = BatchQktTake(batch, command);
        if (pre_ts != nullptr) {
          if (kids != nullptr) {
            kids->push_back(command->instance_id_);
          }
          ProcessCommandSubmittedTail(local_device_submissions_, command, true, pre_ts);
          local_device_submissions_.commands_free_pool_.push_back(command);
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
        if (BatchQktAccumulate(command, batchqkt && !batchqkt_legacy)) {
          it++;
          continue;
        }
        // Fix B: same handoff rule as the event-matched branch above.
        if (HandleCommandSubmitted(local_device_submissions_, command, kids, true)) {
        // Merge note: v3.1 prefetch consumption dropped here (proven inert for
        // counter-based events; the prefetch snapshot block above is harmless).
          // deferred: the command object is owned by the completer thread now
        }
        else {
          local_device_submissions_.commands_free_pool_.push_back(command);
        }
        it = local_device_submissions_.commands_submitted_.erase(it);
        continue;
      }
      else {
        bool processed = false;
        bool deferred = false;
        // T6'/E6 (v3): a pending clone either consumes its batch-read packet
        // or stays queued for the staging drain.
        const ze_kernel_timestamp_result_t *pre_ts = BatchQktTake(batch, command);
        if (pre_ts != nullptr) {
          if (kids != nullptr) {
            kids->push_back(command->instance_id_);
          }
          ProcessCommandSubmittedTail(local_device_submissions_, command, true, pre_ts);
          processed = true;
        }
        else if (BatchQktAccumulate(command, batchqkt && !batchqkt_legacy)) {
          // stays queued for the next flush
        }
        else if ((command->device_global_timestamps_ != nullptr) || (command->timestamps_on_event_reset_ != nullptr)) {
          if (command->timestamp_event_ != nullptr &&  // HARDEN: null event from a failed pool creation never reaches the driver
    ZE_FUNC(zeEventQueryStatus)(command->timestamp_event_) == ZE_RESULT_SUCCESS) {
            // Fix B: legacy host-buffer timestamps stay on the inline path.
            ProcessCommandSubmitted(local_device_submissions_, command, nullptr, false);
            processed = true;
          }
        }
        else {
          if (ZE_FUNC(zeEventQueryStatus)(command->event_) == ZE_RESULT_SUCCESS) {
            deferred = HandleCommandSubmitted(local_device_submissions_, command, nullptr, true);
            processed = true;
          }
        }
        if (processed) {
          // event_cache_.ReleaseEvent(command->event_) or event_cache_.ResetEvent(command->event_) is already called inside ProcessCommandSubmitted()
          // Fix B: a deferred command belongs to the completer thread — no recycling.
          if (!deferred) {
            local_device_submissions_.commands_free_pool_.push_back(command);
          }
          it = local_device_submissions_.commands_submitted_.erase(it);
          continue;
        }
      }
      it++;
    }
    if (batchqkt) {
      uni_batchqkt_in_batch_ = false;  // end of the flush window
    }
    if (options_.metric_query) {
      ProcessCommandMetricQueriesSubmitted();
    }
    global_device_submissions_mutex_.unlock_shared();
    FlushGraphEventResets();  // Graph replay fix: end-of-sweep deferred resets
  }

  inline uint64_t ComputeDuration(uint64_t start, uint64_t end, uint64_t freq, uint64_t mask, double ns_per_cycle) {
    uint64_t duration = 0;
    if (start <= end) {
      duration = static_cast<double>(end - start) * ns_per_cycle;
    } else { // Timer Overflow
      duration = static_cast<double>(mask - start + 1 + end) * ns_per_cycle;
    }
    return duration;
  }

  inline void GetHostTime(const ZeCommand *command, const ze_kernel_timestamp_result_t& ts, uint64_t& start, uint64_t& end) {
    uint64_t device_freq = command->device_timer_frequency_;
    uint64_t device_mask = command->device_timer_mask_;
    double ns_per_cycle = command->device_ns_per_cycle_;

    uint64_t device_start = ts.global.kernelStart & device_mask;
    uint64_t device_end = ts.global.kernelEnd & device_mask;

    uint64_t device_submit_time = (command->submit_time_device_ & device_mask);

    uint64_t time_shift;

    if (device_start > device_submit_time) {
      time_shift = static_cast<double>(device_start - device_submit_time) * ns_per_cycle;
    }
    else {
      // overflow
      time_shift = static_cast<double>(device_mask - device_submit_time + 1 + device_start) * ns_per_cycle;
    }

    uint64_t duration = ComputeDuration(device_start, device_end, device_freq, device_mask, ns_per_cycle);

    start = command->submit_time_ + time_shift;
    end = start + duration;

    if (std::getenv("UNITRACE_DEBUG_TS") != nullptr) {
      std::cerr << "[TSCONV] inst=" << command->instance_id_
                << " dev_start=" << device_start
                << " dev_end=" << device_end
                << " dev_submit=" << device_submit_time
                << " shift_ns=" << time_shift
                << " dur_ns=" << duration
                << " host_start=" << start
                << " host_end=" << end
                << std::endl;
    }
  }

  void PrintCommandCompleted(const ZeCommand *command, uint64_t kernel_start, uint64_t kernel_end) {
    std::string str("Thread ");
    str += std::to_string(command->tid_) + " Device " + std::to_string(reinterpret_cast<uintptr_t>(command->device_)) +
      " : " + GetZeKernelCommandName(command->kernel_command_id_, command->group_count_, command->mem_size_) + " [ns] " +
      std::to_string(command->append_time_) + " (append) " +
      std::to_string(command->submit_time_) + " (submit) " +
      std::to_string(kernel_start) + " (start) " +
      std::to_string(kernel_end) + " (end)" +
      " engine=" + std::to_string(command->engine_ordinal_) + "." + std::to_string(command->engine_index_) + "\n";
    logger_device_timeline_->Log(str);
  }

  inline void LogCommandCompleted(const ZeCommand *command, const ze_kernel_timestamp_result_t& timestamp, int tile) {

    uint64_t kernel_start = 0, kernel_end = 0;
    GetHostTime(command, timestamp, kernel_start, kernel_end);

    PTI_ASSERT(kernel_start <= kernel_end);

    if (options_.device_timing || options_.kernel_submission) {
      local_device_submissions_.CollectKernelCommandTimeStats(command, kernel_start, kernel_end, tile);
    }

    if (options_.device_timeline) {
      PrintCommandCompleted(command, kernel_start, kernel_end);
    }

    if (kcallback_) {
      bool implicit_scaling = ((tile >= 0) && command->implicit_scaling_);

      kcallback_(command->instance_id_, command->tid_, kernel_start, kernel_end, command->engine_ordinal_, command->engine_index_, tile, command->device_, command->kernel_command_id_, implicit_scaling, command->group_count_, command->mem_size_);
    }
  }

  // ---- TSLOG v2 event history helpers (guarded by events_mutex_) ----
  // Registers the event if unseen and bumps its signal generation.
  // Returns the generation this signal action produces (snapshot for the command).
  uint64_t EventHistoryBumpSignal(ze_event_handle_t event, uint64_t path, uint64_t inst) {
    if (event == nullptr) {
      return 0;
    }
    events_mutex_.lock();
    auto& h = event_history_[event];
    h.gen += 1;
    h.signal_count += 1;
    h.last_signal_path = path;
    h.last_signal_inst = inst;
    uint64_t gen = h.gen;
    events_mutex_.unlock();
    return gen;
  }

  void EventHistoryMarkReset(ze_event_handle_t event) {
    if (event == nullptr) {
      return;
    }
    events_mutex_.lock();
    auto& h = event_history_[event];
    h.gen = 0;
    h.reset_count += 1;
    events_mutex_.unlock();
  }

  uint64_t EventHistoryGetGen(ze_event_handle_t event) {
    if (event == nullptr) {
      return 0;
    }
    events_mutex_.lock();
    auto it = event_history_.find(event);
    uint64_t gen = (it != event_history_.end()) ? it->second.gen : 0;
    events_mutex_.unlock();
    return gen;
  }

  void EventHistoryErase(ze_event_handle_t event) {
    if (event == nullptr) {
      return;
    }
    events_mutex_.lock();
    event_history_.erase(event);
    events_mutex_.unlock();
  }

  // Graph replay fix v3: clones of one replay share one physical event and must
  // all read the same packet, but the host reset must not outlive the batch —
  // a latched event makes the app's next host-wait return early and the
  // resulting read carries the previous generation's packet (the v2d stale
  // signature). These helpers track how many clones of an event are still
  // queued so the reset fires as soon as the last clone is processed.
  void EventHistoryAddPending(ze_event_handle_t event) {
    if (event == nullptr) {
      return;
    }
    events_mutex_.lock();
    event_history_[event].pending_clones += 1;
    events_mutex_.unlock();
  }

  // Returns true when the caller must reset the event now (this was the last
  // pending clone). Returns false when nothing was tracked or clones remain.
  bool EventHistoryConsumePending(ze_event_handle_t event) {
    if (event == nullptr) {
      return false;
    }
    events_mutex_.lock();
    auto it = event_history_.find(event);
    bool reset_now = false;
    if (it != event_history_.end() && it->second.pending_clones > 0) {
      it->second.pending_clones -= 1;
      reset_now = (it->second.pending_clones == 0);
    }
    events_mutex_.unlock();
    return reset_now;
  }

  // E6-v4: parks a shared event whose last pending clone has just been read,
  // so the reset is issued as one device-side batch at the end of the sweep
  // (FlushGraphEventResets -> BatchQktResetEvents) instead of one
  // zeEventHostReset ioctl per consumed clone. Only graph clones with the
  // batch gate on park here: their events live as long as the graph, the
  // parked set is drained by the flush every sweep ends with, and the
  // pending-clone counter above already proved no clone needs the packet
  // anymore — so deferring the reset cannot strand or resurrect a packet.
  void EventHistoryDeferGraphEventReset(ze_context_handle_t context, ze_event_handle_t event) {
    if (event == nullptr) {
      return;
    }
    events_mutex_.lock();
    graph_events_pending_reset_[context].insert(event);
    events_mutex_.unlock();
  }

  bool EventHistoryHasPending(ze_event_handle_t event) {
    if (event == nullptr) {
      return false;
    }
    events_mutex_.lock();
    auto it = event_history_.find(event);
    bool pending = (it != event_history_.end()) && (it->second.pending_clones > 0);
    events_mutex_.unlock();
    return pending;
  }

  // Graph replay fix: erase the kernel-timestamp packets of graph events whose
  // clones were all processed in this sweep. Called at the end of every command
  // sweep so the deferred reset never spans beyond one sweep.
  //
  // E6-v4: with the batch gate on the sweeps no longer pay a zeEventHostReset
  // ioctl per consumed clone (EventHistoryDeferGraphEventReset parks the
  // handle above instead). This flush is where they are executed, as ONE
  // device-side batch per context: one zeCommandListAppendEventReset per event
  // on the collector's persistent immediate command list + a single
  // zeCommandListHostSynchronize (appends are queue writes, the synchronize is
  // the only wait). That takes the staging drain from O(clones) driver ioctls
  // down to O(1) waits. The synchronize completes INSIDE this call, i.e.
  // inside the sweep, so the events are really un-signaled before
  // PrepareGraphExecution's clone loop re-bumps/re-signals them for the next
  // replay — a reset that merely raced the next replay would drop that
  // replay's timestamps. Everything that cannot ride the immediate list (gate
  // off, unknown context, no immediate list, append/synchronize failure, event
  // no longer owned by the cache) falls back to the legacy per-event host
  // reset, so each parked event is reset exactly once either way.
  void FlushGraphEventResets(void) {
    std::map<ze_context_handle_t, std::set<ze_event_handle_t>> pending;
    events_mutex_.lock();
    pending.swap(graph_events_pending_reset_);
    events_mutex_.unlock();
    for (auto &ctx : pending) {
      BatchQktResetEvents(ctx.first, ctx.second);
    }
  }

  // ================= Fix B: deferred timestamp completion =================
  // UNITRACE_DEFERRED_TS=1 moves the expensive half of command completion
  // (zeEventQueryKernelTimestamp, timeline/chrome emit, ConsumePending /
  // ResetEvent) off the application thread onto one background completer
  // thread. The intercepting sweep keeps only the cheap zeEventQueryStatus
  // completion polls, records the host-API flow ids (kids) synchronously, and
  // hands the command over. Because the completer touches events after the
  // app thread moved on, every path that reuses/resets/destroys an event or
  // fully drains submissions must first call FlushDeferredTimestamps().
  struct DeferredTsItem {
    ZeCommand *command_;
    bool on_event_;
  };

  // Reentrancy guard. The completer thread runs the tail of the original
  // inline path, which calls L0 entry points (zeEventDestroy inside
  // ZeEventCache::ReleaseEvent, zeEventHostReset inside ResetEvent, ...) that
  // go through the tracing layer and therefore fire the collector's own
  // callbacks on the completer thread. Those callbacks must never wait on the
  // completer queue (the worker would wait for itself, deadlocking against
  // FlushData's join) and must never hand new commands back to it.
  static inline thread_local bool uni_defer_on_completer_thread_ = false;

  // Shipping default ON: eager (non-graph) commands' timestamp query+read+emit
  // runs on the completer thread instead of inline in the submit-thread drain
  // (measured -0.48ms/step on node3 trim24L). Set UNITRACE_DEFERRED_TS=0 to
  // restore the inline drain shape for A/B testing.
  static bool DeferredTsEnvEnabled(void) {
    static const bool enabled = []() {
      const char *e = std::getenv("UNITRACE_DEFERRED_TS");
      return (e == nullptr || !(e[0] == '0' && e[1] == '\0'));
    }();
    return enabled;
  }

  static bool DeferredTsDiagEnabled(void) {
    static const bool enabled = (std::getenv("UNITRACE_DEBUG_DEFER") != nullptr);
    return enabled;
  }

  // Whether this completed command may be handed to the completer thread.
  // Only commands whose timestamps come from querying an event are eligible:
  // the legacy host-buffer timestamp paths (device_global_timestamps_ /
  // timestamps_on_event_reset_) keep the original inline behaviour because
  // those buffers belong to the command list and may be freed while the item
  // is queued. TSLOG2 diagnosis modes force the original path to keep their
  // exact inline ordering.
  bool ShouldDeferCommand(const ZeCommand *command) const {
    if (uni_defer_on_completer_thread_) {
      return false;  // nested sweep on the completer thread: never re-defer
    }
    if (!uni_defer_ts_enabled_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (Tslog2Mode() != 0) {
      return false;
    }
    if (options_.metric_query || options_.metric_stream) {
      return false;  // metric completion keeps its own submission-thread path
    }
    if (command->graph_command_ && uni_batchqkt_enabled_.load(std::memory_order_relaxed)) {
      // T6'/E6: graph replay clones are consumed inline by the intercepting
      // sweep (their shared completion event is batch-read there). Handing
      // them to the completer would only relocate the per-event query tax onto
      // the worker thread and delay the shared event's reset — the batch read
      // removes the tax instead, so graph clones never take the deferred path
      // while the batch gate is on. With the gate off this predicate is
      // untouched and clones keep deferring as before.
      return false;
    }
    if (command->event_ == nullptr) {
      return false;
    }
    if (command->device_global_timestamps_ != nullptr ||
        command->timestamps_on_event_reset_ != nullptr) {
      return false;
    }
    if (uni_defer_worker_stop_.load(std::memory_order_acquire)) {
      return false;
    }
    return UniController::IsCollectionEnabled(/*defer_callback=*/true);
  }

  // Returns true when the command was handed to the completer thread (caller
  // must NOT recycle it into commands_free_pool_), false when it was fully
  // processed inline (original behaviour; caller recycles as before).
  // kids are recorded synchronously in both cases: the host-API flow arrows
  // are only valid inside the intercepted call.
  bool HandleCommandSubmitted(ZeDeviceSubmissions& submissions, ZeCommand *command,
                              std::vector<uint64_t> *kids, bool on_event) {
    if (ShouldDeferCommand(command)) {
      if (kids != nullptr) {
        kids->push_back(command->instance_id_);
      }
      QueueDeferredCommand(command, on_event);
      return true;
    }
    ProcessCommandSubmitted(submissions, command, kids, on_event);
    return false;
  }

  void EnsureDeferredCompleter(void) {
    if (uni_defer_worker_started_.load(std::memory_order_acquire)) {
      return;
    }
    std::lock_guard<std::mutex> lk(uni_defer_q_mutex_);
    if (uni_defer_worker_started_.load(std::memory_order_relaxed)) {
      return;
    }
    uni_defer_worker_started_.store(true, std::memory_order_release);
    uni_defer_worker_ = std::thread(&ZeCollector::DeferredCompleterMain, this);
    if (DeferredTsDiagEnabled()) {
      std::cerr << "[DEFER] completer thread started (UNITRACE_DEFERRED_TS=1)" << std::endl;
    }
  }

  void QueueDeferredCommand(ZeCommand *command, bool on_event) {
    EnsureDeferredCompleter();
    std::unique_lock<std::mutex> lk(uni_defer_q_mutex_);
    if (uni_defer_worker_stop_.load(std::memory_order_acquire)) {
      // Shutdown won the race: finish this command inline so nothing is ever
      // queued behind the joined completer thread.
      lk.unlock();
      uni_defer_inline_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      ProcessCommandSubmittedTail(local_device_submissions_, command, on_event);
      delete command;
      return;
    }
    uni_defer_queue_.push_back({command, on_event});
    uni_defer_handoff_count_.fetch_add(1, std::memory_order_relaxed);
    lk.unlock();
    uni_defer_q_cv_.notify_one();
  }

  void DeferredCompleterMain(void) {
    uni_defer_on_completer_thread_ = true;
    std::deque<DeferredTsItem> batch;
    std::unique_lock<std::mutex> lk(uni_defer_q_mutex_);
    for (;;) {
      uni_defer_q_cv_.wait(lk, [&] {
        return uni_defer_worker_stop_.load(std::memory_order_acquire) ||
               !uni_defer_queue_.empty();
      });
      if (uni_defer_queue_.empty()) {
        // stop requested and everything handed over has been processed
        uni_defer_q_cv_.notify_all();
        break;
      }
      batch.swap(uni_defer_queue_);
      uni_defer_worker_busy_ = true;
      uint64_t batch_size = batch.size();
      uni_defer_batch_count_.fetch_add(1, std::memory_order_relaxed);
      uint64_t prev_max = uni_defer_max_batch_.load(std::memory_order_relaxed);
      while (batch_size > prev_max &&
             !uni_defer_max_batch_.compare_exchange_weak(prev_max, batch_size, std::memory_order_relaxed)) {
      }
      lk.unlock();
      // T14/A5: the eager-side Fix B debt -- the deferred timestamp read + emit
      // + reset burst this completer batch runs. It lives on the completer
      // thread (not inside an app API call), so its meta slice has no host
      // parent: it renders on the completer thread's own track.
      const bool meta = MetaOn();
      const uint64_t mt0 = meta ? UniTimer::GetHostTimestamp() : 0;
      for (DeferredTsItem &item : batch) {
        ProcessCommandSubmittedTail(local_device_submissions_, item.command_, item.on_event_);
        delete item.command_;
        uni_defer_processed_count_.fetch_add(1, std::memory_order_relaxed);
      }
      batch.clear();
      if (meta) {
        const uint64_t mt1 = UniTimer::GetHostTimestamp();
        MetaRecord("unitrace.handoff", mt0, mt1,
                   "\"n\": " + std::to_string(batch_size) +
                   ", \"us\": " + std::to_string(UniTimer::GetTimeInUs(mt1 - mt0)) + "");
      }
      lk.lock();
      uni_defer_worker_busy_ = false;
      uni_defer_q_cv_.notify_all();
    }
  }

  // Barrier: block until every command handed to the completer thread so far
  // has been fully processed (timestamp read, emit, event release/reset).
  // Must run before any event/pool/context/command-list teardown, and before
  // graph replay staging re-signals shared events. Near-zero cost once the
  // queue has drained (steady state: the completer digests during the device
  // busy window).
  void FlushDeferredTimestamps(void) {
    if (uni_defer_on_completer_thread_) {
      return;  // reentrant call from the worker's own L0 call: never self-wait
    }
    if (!uni_defer_ts_enabled_.load(std::memory_order_relaxed)) {
      return;
    }
    if (!uni_defer_worker_started_.load(std::memory_order_acquire) ||
        uni_defer_worker_joined_.load(std::memory_order_acquire)) {
      return;
    }
    auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lk(uni_defer_q_mutex_);
      if (uni_defer_worker_joined_.load(std::memory_order_relaxed)) {
        return;
      }
      uni_defer_flush_count_.fetch_add(1, std::memory_order_relaxed);
      uni_defer_q_cv_.notify_all();
      uni_defer_q_cv_.wait(lk, [&] {
        return !uni_defer_worker_busy_ && uni_defer_queue_.empty();
      });
    }
    uint64_t wait_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    uint64_t prev_max = uni_defer_flush_max_wait_us_.load(std::memory_order_relaxed);
    while (wait_us > prev_max &&
           !uni_defer_flush_max_wait_us_.compare_exchange_weak(prev_max, wait_us, std::memory_order_relaxed)) {
    }
    if (DeferredTsDiagEnabled() && wait_us > 5000) {
      std::cerr << "[DEFER] flush waited " << wait_us << "us (barrier drain)" << std::endl;
    }
  }

  // Final stop: no more handoffs (ShouldDeferCommand checks the stop flag),
  // the worker drains the queue before exiting, then we join it.
  void StopDeferredCompleter(void) {
    if (!uni_defer_worker_started_.load(std::memory_order_acquire) ||
        uni_defer_worker_joined_.load(std::memory_order_acquire)) {
      return;
    }
    uni_defer_worker_stop_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lk(uni_defer_q_mutex_);
      uni_defer_q_cv_.notify_all();
    }
    if (uni_defer_worker_.joinable()) {
      uni_defer_worker_.join();
    }
    uni_defer_worker_joined_.store(true, std::memory_order_release);
    PrintDeferredTsDiag();
  }

  void PrintDeferredTsDiag(void) {
    if (!DeferredTsDiagEnabled()) {
      return;
    }
    std::cerr << "[DEFER] summary: enabled=" << uni_defer_ts_enabled_.load(std::memory_order_relaxed)
              << " handoff=" << uni_defer_handoff_count_.load(std::memory_order_relaxed)
              << " worker_processed=" << uni_defer_processed_count_.load(std::memory_order_relaxed)
              << " inline_fallback=" << uni_defer_inline_fallback_count_.load(std::memory_order_relaxed)
              << " flush_calls=" << uni_defer_flush_count_.load(std::memory_order_relaxed)
              << " flush_max_wait_us=" << uni_defer_flush_max_wait_us_.load(std::memory_order_relaxed)
              << " batches=" << uni_defer_batch_count_.load(std::memory_order_relaxed)
              << " max_batch=" << uni_defer_max_batch_.load(std::memory_order_relaxed)
              << std::endl;
  }

  // Graph replay fix v3: a graph clone that leaves the processing pipeline
  // without emitting a record (timestamp query failure) must still be accounted
  // for, or the shared event never reaches its reset point and stays latched.
  // E6-v4: the reset itself goes through the same gate as the tail — batched
  // at the sweep end with the gate on, inline host reset otherwise.
  void ReleaseGraphCommandEvent(ZeCommand *command) {
    if (command->graph_command_ && command->event_ != nullptr &&
        EventHistoryConsumePending(command->event_)) {
      if (BatchQktResetBatchActive()) {
        EventHistoryDeferGraphEventReset(command->context_, command->event_);
      }
      else {
        event_cache_.ResetEvent(command->event_);
      }
    }
  }

  static inline uint64_t Tslog2HostMonoNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
  }

  // TSLOG v2: snapshot of signal state taken immediately BEFORE a timestamp
  // query. Joinable to chrome slices via inst == args.id.
  // Modes (env UNITRACE_DEBUG_TS2):
  //   unset/0 : off
  //   1       : full logging (extra zeEventQueryStatus + one stderr line per
  //             query) — perturbs timing, use only for smoke tests
  //   2       : anomaly-only — no extra driver calls, no per-query printing;
  //             prints a full line ONLY when the query result is anomalous
  //             (low-32 sentinel, end<start, stale end, gen mismatch)
  struct TsSnapshotV2 {
    bool valid = false;
    int mode = 0;
    uint64_t gen = 0;
  };

  static inline int Tslog2Mode(void) {
    static const int mode = []() {
      const char *e = std::getenv("UNITRACE_DEBUG_TS2");
      return e ? std::atoi(e) : 0;
    }();
    return mode;
  }

  inline TsSnapshotV2 SnapshotTsQueryV2(ZeCommand *command) {
    TsSnapshotV2 snap;
    int mode = Tslog2Mode();
    if (mode == 0 || command->event_ == nullptr) {
      return snap;
    }
    snap.valid = true;
    snap.mode = mode;
    snap.gen = EventHistoryGetGen(command->event_);
    return snap;
  }

  inline void LogTsAnomalyV2(ZeCommand *command, bool on_event,
                             const ze_kernel_timestamp_result_t &timestamp,
                             const TsSnapshotV2 &snap) {
    if (!snap.valid || command->event_ == nullptr) {
      return;
    }
    if (snap.mode >= 2) {
      // Anomaly-only mode: decide from the raw query result + gen snapshot.
      bool anom =
          (timestamp.global.kernelStart & 0xFFFFFFFFull) == 0xFFFFFFFFull ||
          (timestamp.global.kernelEnd & 0xFFFFFFFFull) == 0xFFFFFFFFull ||
          timestamp.global.kernelEnd < timestamp.global.kernelStart ||
          (command->submit_time_device_ != 0 &&
           timestamp.global.kernelEnd + 1000000ull < command->submit_time_device_) ||
          snap.gen != command->signal_gen_;
      if (!anom) {
        return;
      }
    }
    ze_result_t pre_status = ZE_FUNC(zeEventQueryStatus)(command->event_);
    uint32_t idx = event_cache_.GetEventIndex(command->event_);
    events_mutex_.lock();
    ZeEventHistory h = {0, 0, 0, 0, 0};
    auto it = event_history_.find(command->event_);
    if (it != event_history_.end()) {
      h = it->second;
    }
    events_mutex_.unlock();
    std::cerr << "[TSLOG2] " << (on_event ? "on_event" : "fallback")
              << " inst=" << command->instance_id_
              << " ev=" << static_cast<const void*>(command->event_)
              << " idx=" << std::hex << idx << std::dec
              << " gen=" << snap.gen
              << " exp_gen=" << command->signal_gen_
              << " st=" << pre_status
              << " graph=" << command->graph_command_
              << " imm=" << command->immediate_
              << " cl=" << static_cast<const void*>(command->command_list_)
              << " tid=" << command->tid_
              << " submit_host=" << command->submit_time_
              << " submit_dev=" << command->submit_time_device_
              << " raw_start=" << timestamp.global.kernelStart
              << " raw_end=" << timestamp.global.kernelEnd
              << " sig_cnt=" << h.signal_count
              << " rst_cnt=" << h.reset_count
              << " last_path=" << h.last_signal_path
              << " last_inst=" << h.last_signal_inst
              << " mono=" << Tslog2HostMonoNs()
              << std::endl;
  }

  inline void LogTsQueryV2(ZeCommand *command, bool on_event) {
    static const bool enabled = (Tslog2Mode() == 1);
    if (!enabled || command->event_ == nullptr) {
      return;
    }
    ze_result_t pre_status = ZE_FUNC(zeEventQueryStatus)(command->event_);
    uint64_t gen = EventHistoryGetGen(command->event_);
    uint32_t idx = event_cache_.GetEventIndex(command->event_);
    events_mutex_.lock();
    ZeEventHistory h = {0, 0, 0, 0, 0};
    auto it = event_history_.find(command->event_);
    if (it != event_history_.end()) {
      h = it->second;
    }
    events_mutex_.unlock();
    std::cerr << "[TSLOG2] " << (on_event ? "on_event" : "fallback")
              << " inst=" << command->instance_id_
              << " ev=" << static_cast<const void*>(command->event_)
              << " idx=" << std::hex << idx << std::dec
              << " gen=" << gen
              << " exp_gen=" << command->signal_gen_
              << " st=" << pre_status
              << " graph=" << command->graph_command_
              << " imm=" << command->immediate_
              << " cl=" << static_cast<const void*>(command->command_list_)
              << " tid=" << command->tid_
              << " submit_host=" << command->submit_time_
              << " submit_dev=" << command->submit_time_device_
              << " sig_cnt=" << h.signal_count
              << " rst_cnt=" << h.reset_count
              << " last_path=" << h.last_signal_path
              << " last_inst=" << h.last_signal_inst
              << " mono=" << Tslog2HostMonoNs()
              << std::endl;
  }

  // -----------------------------------------------------------------------
  // Fix D (collection tax): read the latest timestamp packet through
  // zeEventQueryKernelTimestampsExt instead of the legacy per-call ioctl
  // path. Micro-bench on the affected driver (idle device): legacy
  // zeEventQueryKernelTimestamp p50 ~2.7us (an ioctl; ~9us under load, ~50us
  // at default CPU power), Ext p50 ~0.04us (mapped-packet read) with
  // bit-identical packets (0/2048 events mismatched). Ext returns ONLY the
  // latest packet per event — exactly the generation every consumer here
  // already assumes (v3 sweep reads the current generation). Resolved once
  // via dlsym (zeDriverGetExtensionFunctionAddress is broken on this
  // loader/driver pair); falls back to the legacy query on any mismatch or
  // error. Enabled with UNITRACE_TS_EXT=1.
  // -----------------------------------------------------------------------
  static bool UseExtKernelTimestamps() {
    static const bool use = [] {
      const char *e = getenv("UNITRACE_TS_EXT");
      return e != nullptr && e[0] == '1';
    }();
    return use;
  }

  static ze_result_t QueryLatestTimestamp(ze_event_handle_t event,
                                          ze_kernel_timestamp_result_t *ts) {
    if (UseExtKernelTimestamps()) {
      using pfn_t = ze_result_t (*)(ze_event_handle_t, ze_device_handle_t, uint32_t *,
                                    ze_event_query_kernel_timestamps_results_ext_properties_t *);
      static pfn_t pfn = []() -> pfn_t {
        return reinterpret_cast<pfn_t>(dlsym(RTLD_DEFAULT, "zeEventQueryKernelTimestampsExt"));
      }();
      static ze_device_handle_t cached_dev = []() -> ze_device_handle_t {
        ze_device_handle_t d = nullptr;
        devices_mutex_.lock_shared();
        if (devices_ != nullptr && !devices_->empty()) d = devices_->begin()->first;
        devices_mutex_.unlock_shared();
        return d;
      }();
      if (pfn != nullptr && cached_dev != nullptr) {
        uint32_t cnt = 0;
        ze_event_query_kernel_timestamps_results_ext_properties_t res{};
        res.stype = ZE_STRUCTURE_TYPE_EVENT_QUERY_KERNEL_TIMESTAMPS_RESULTS_EXT_PROPERTIES;
        if (pfn(event, cached_dev, &cnt, NULL) == ZE_RESULT_SUCCESS && cnt > 0) {
          res.pKernelTimestampsBuffer = ts;
          if (pfn(event, cached_dev, &cnt, &res) == ZE_RESULT_SUCCESS) {
            return ZE_RESULT_SUCCESS;
          }
        }
      }
    }
    return ZE_FUNC(zeEventQueryKernelTimestamp)(event, ts);
  }

  // Graph replay fix: a kernel-timestamp packet read that cannot belong to this
  // command's own execution — low-32 sentinel (erased start), end<start (torn
  // read across packet writes), or end far before this replay's submit tick
  // (stale content from an earlier generation). Such a read must never reach
  // the timeline: end minus an erased start produced multi-hour bogus durations.
  static inline bool TsReadIsBad(const ZeCommand *command,
                                 const ze_kernel_timestamp_result_t &timestamp) {
    if ((timestamp.global.kernelStart & 0xFFFFFFFFull) == 0xFFFFFFFFull ||
        (timestamp.global.kernelEnd & 0xFFFFFFFFull) == 0xFFFFFFFFull ||
        timestamp.global.kernelEnd < timestamp.global.kernelStart) {
      return true;
    }
    return command->submit_time_device_ != 0 &&
           timestamp.global.kernelEnd + 1000000ull < command->submit_time_device_;
  }

  // -----------------------------------------------------------------------
  // Graph replay fix v3.1 (collection tax): a step-boundary sweep used to run
  // one zeEventQueryKernelTimestamp ioctl per replayed command serially inside
  // the app's sync call (measured 40-60ms per decode step on vLLM XPU-graph
  // replay). The commands bound to the event the app waited on are
  // known-complete, so their timestamps are prefetched with helper threads;
  // consumption below stays serial and in list order, preserving the v3
  // pending-clone semantics. Read-only on collector state.
  // -----------------------------------------------------------------------
  static constexpr size_t kTsPrefetchMinCommands = 32;  // only bother past this many bound commands
  static constexpr size_t kTsPrefetchChunk = 32;        // min commands per helper thread
  static constexpr unsigned kTsPrefetchThreads = 8;     // workers incl. the caller

  struct ZePrefetchedTs {
    bool valid = false;
    ze_result_t status = ZE_RESULT_NOT_READY;
    ze_kernel_timestamp_result_t ts{};
    TsSnapshotV2 snap{};
  };

  void PrefetchTimestampsParallel(const std::vector<ZeCommand *> &cmds,
                                  std::vector<ZePrefetchedTs> &out) {
    const size_t n = cmds.size();
    out.assign(n, ZePrefetchedTs{});
    unsigned hw = std::thread::hardware_concurrency();
    unsigned nthreads = std::min<unsigned>(kTsPrefetchThreads, hw ? hw : 1u);
    nthreads = std::min<unsigned>(static_cast<unsigned>((n + kTsPrefetchChunk - 1) / kTsPrefetchChunk), nthreads);
    if (nthreads < 1) nthreads = 1;
    std::atomic<size_t> next(0);
    auto worker = [&]() {
      for (;;) {
        size_t i = next.fetch_add(1);
        if (i >= n) break;
        out[i].status = QueryLatestTimestamp(cmds[i]->event_, &out[i].ts);
        out[i].valid = (out[i].status == ZE_RESULT_SUCCESS);
      }
    };
    std::vector<std::thread> threads;
    threads.reserve(nthreads - 1);
    for (unsigned t = 1; t < nthreads; t++) threads.emplace_back(worker);
    worker();
    for (auto &th : threads) th.join();
  }

  // -----------------------------------------------------------------------
  // T6' (plan E6, collection tax): per-replay batched kernel-timestamp read
  // for graph replay commands. Enabled with UNITRACE_GRAPH_BATCH_QKT=1
  // (default OFF = byte-identical legacy behaviour). Diagnostics with
  // UNITRACE_DEBUG_BATCHQKT=1 ([BATCHQKT] lines, Fix B [DEFER] style).
  //
  // Graph replay clones all captured commands onto ONE shared completion
  // event, so a step-boundary sweep used to run one
  // zeEventQueryKernelTimestamp ioctl per clone (~60us each -> ~51ms per
  // decode step at ~800 commands). Because every sweep that participates here
  // only processes a clone after its event is proven signaled, all clones of
  // the replay are complete at that point and their packets can be read with a
  // single device-side batch: zeCommandListAppendQueryKernelTimestamps (QA
  // form: no signal event) on a collector-owned persistent immediate command
  // list + one zeCommandListHostSynchronize. The driver allocates ~2MB of
  // internal staging per call — acceptable at per-replay frequency (per-kernel
  // frequency is what killed Fix C). Verified against the per-event path in
  // the T3 e2 micro bench: 828 events, p50 488us, packets byte-identical,
  // 1000-iteration longrun without hang. That signaled-first ordering is also
  // the protection against the driver's not-ready hot spin.
  //
  // Scope: only clones with graph_command_ set. Eager / immediate commands
  // keep the Fix B / inline paths untouched.
  //
  // v3 — accumulate early, consume late. The gq1b run showed why per-sweep
  // collect+execute cannot reach the micro-proven shape: the batch armed at
  // whichever sweep happened to run mid-step and only the ~40 clones complete
  // at that instant joined it (n=37..55, ~88us/event once the ~2-4ms fixed
  // staging cost per append is amortized over so few), while the loop then
  // paid the full legacy per-event tax for the remaining ~95%. v3 inverts the
  // flow: in EVERY sweep a pending graph clone is skipped by the legacy
  // branches (no status call, no query — it simply stays queued) and the
  // accumulated clones are flushed as ONE batch per replay at the point where
  // the whole step is provably complete: the graph replay staging drain.
  // PrepareGraphExecution host-synchronizes every list holding in-flight
  // clones of this graph BEFORE its pre-staging
  // ProcessAllCommandsSubmitted(nullptr), so at that drain every pending
  // clone's shared event is signaled and one numEvents=pending batch replaces
  // the step's ~800 ioctls (~500us, micro T3_e2micro). OnSignaledEvent sweeps
  // deliberately do NOT execute the batch: with events latched until the
  // batch resets them, a matched-event trigger would re-fire on every poll
  // and fragment the batch again, and the polled event is not proven to gate
  // the whole step. Fence/teardown drains keep a readiness-gated flush as the
  // backstop so pending clones never outlive a destroy (see
  // ReleaseGraphResources). Pending is naturally bounded: a staging drain
  // always precedes the next replay's staging, so at most one replay's clones
  // (<= ~800) can be queued at a time — v4b relaxes this to at most two
  // replays (the drain-deferred emit of replay N + the live clones of N+1),
  // still bounded, see the v4b note below. Emit order is list order (cursor
  // match); TSBAD, kids, the pending-clone event reset and the Fix B
  // interaction are unchanged — only WHEN the packet is read moved.
  //
  // v4 — batch the resets too. The QKT read is ~4ms per replay, but the
  // consume loop that follows it still reset every clone's shared event with
  // its own zeEventHostReset ioctl (~770 of them, ~82us each): at the staging
  // drain the device is idle, so those ioctls are fully serial on the app's
  // critical path — ~63ms per step, i.e. the whole +65ms ITL the gate was
  // measured to cost. Legacy mode pays the same ioctls but spreads them over
  // poll sweeps where they overlap device execution, which is why only the
  // batched shape exposes them. v4 parks the handle in
  // graph_events_pending_reset_ when the last pending clone of an event has
  // read it (EventHistoryDeferGraphEventReset) and resets the whole parked set
  // at the sweep end with N zeCommandListAppendEventReset appends on the SAME
  // collector-owned immediate list the QKT batch uses + ONE
  // zeCommandListHostSynchronize (FlushGraphEventResets -> BatchQktResetEvents).
  // Ordering is what makes that safe: the flush runs before the sweep returns,
  // so the events are un-signaled before PrepareGraphExecution's clone loop
  // re-signals them, and it runs after the packet read, so no clone of a
  // parked event can lose its packet. Everything that cannot be batched falls
  // back to the exact v3 host reset. UNITRACE_GRAPH_BATCH_QKT_RESET=0 puts
  // the resets back on the v3 per-event path (A/B knob, default on).
  //
  // v4b — split the flush: read at the drain, emit at the next poll. gq3
  // (v4 in, reset batching verified: reset_batched == cmds, fallback=0,
  // reset_max_us=1313) DISPROVED the reset-dominance theory: ITL stayed at
  // ~149 with the drain window still ~68ms and 100% device-idle. The drain's
  // cost decomposes into ~7ms batch read + ~1.3ms batched resets + ~58ms
  // consume+emit (~770 clones x ~75us of record machinery; FLOW=0 did not
  // move it, so it is the base record path, not flow arrows). That emit is
  // the OUTPUT — irreducible, only relocateable — and legacy's only advantage
  // was that poll sweeps happen while the device is busy, so the same host
  // work overlapped instead of serializing. v4b keeps the READ at the drain,
  // because that is the last moment the packets are provably still this
  // generation's: the captured commands reuse the same physical events, so
  // once the next replay is staged and submitted the device overwrites them
  // (a read taken at a later sweep would silently attach the new generation's
  // ticks to the previous one's instances — no TSBAD, just wrong records).
  // The drain's list host-synchronize proves the whole replay complete before
  // that read, and that proof travels with the batch: the armed packets are
  // final, so the emit pass needs no readiness gate and cannot read a packet
  // twice. The EMIT then runs at the top of the first sweep after the drain —
  // the app's poll of the replayed graph, which fires while the device
  // executes the new step — i.e. in legacy's hiding spot, without v2's
  // fragmentation (the v3 skip-legacy still keeps intermediate sweeps from
  // consuming clones; only the armed, already-read batch is emitted). If the
  // app never polls between steps, the next drain's backstop pass emits the
  // armed batch itself: same cost as v4, nothing lost but the overlap.
  // See BatchQktArmDeferredEmit / BatchQktConsumeDeferredEmit.
  //
  // v4c — move the READ's wait off the drain too (UNITRACE_GRAPH_QKT_AT_POLL,
  // default OFF = byte-identical v4b). New IFWI made the device faster
  // (bare-graph ITL 82.0 -> 73.46) while v4b's absolute cost stayed put, so
  // the drain's ~6.4ms QKT read became the tax body (q14l/q14t: +15.6% > the
  // 10% line, vs +3.1% before). The naive shape — read step N's events at a
  // later sweep, after step N+1 is staged — is WRONG for the same reason v4b
  // kept the read at the drain: the captured commands reuse the same physical
  // events, so a later read returns the NEXT generation's ticks (or a
  // not-ready event), with no TSBAD to catch it. What CAN move is the wait:
  // the batch append is issued at a device-busy poll sweep while the replay
  // it belongs to is still executing, with numWaitEvents = the very event set
  // it reads, so the DEVICE parks the query until the step's last kernel
  // signals and then reads the packets — still this generation's, because
  // nothing has reset or re-signaled them yet. The drain then only pays one
  // HostSynchronize (the wait resolved when the step ended) + the memcpy and
  // runs the UNCHANGED v4b arm. Ordering proof is the same chain as v4b, now
  // host-sequenced: sync(poll read) -> memcpy -> arm/release ->
  // FlushGraphEventResets (append + sync on the regular imm list) -> clone
  // loop re-signals -> next graph submit. The read can never overlap a reset
  // or a re-signal, because the host does not append the reset until the read
  // completed. Armed clones are NOT touched by the arm (no deferred_ts_, no
  // event release, no pending-counter change, no readiness status call), so
  // every skip/failure path falls back to the exact v4b drain read with
  // nothing to unwind; liveness of the handles comes from the event cache
  // (the same QueryEvent gate BatchQktResetEvents uses). See
  // BatchQktArmPollRead / BatchQktCompletePollRead.
  // -----------------------------------------------------------------------
  static constexpr size_t kBatchQktMinCommands = 2;  // below this the append+sync round trip is not worth it
  // Stack budget for per-sweep readiness memoization over DISTINCT events
  // (one status call per event, not per clone). Beyond it the check degrades
  // to per-clone — never unsound.
  static constexpr size_t kBatchQktMaxDistinctEvents = 64;

  // Reentrancy guard for the collector's own L0 calls made inside the batch
  // (the collection pass' zeEventQueryStatus and the immediate-list
  // append/synchronize): their tracing callbacks re-enter these sweeps, which
  // would deadlock on the submission lock the batch already holds. Same
  // pattern as Fix B's uni_defer_on_completer_thread_.
  //
  // v3: the flush sweeps hold this guard across their WHOLE collect+execute
  // +consume window, not just the batch's own L0 calls. Inside that window a
  // tail's inline event reset (or eager ReleaseEvent) comes back through the
  // tracing layer (zeEventHostResetOnEnter / zeEventDestroyOnEnter) and used
  // to re-enter a sweep here; under the exclusive-locked drain that nested
  // sweep takes lock_shared on a mutex this thread holds exclusively
  // (deadlock), and under the shared-locked sweeps it can re-match the
  // command that is mid-consumption in the outer loop (double tail, iterator
  // erase twice). The pending-clone counter already guarantees nothing else
  // needs consuming when the last clone's reset fires, so the nested rescan
  // is pure hazard; suppressing it changes no observable output. The helpers
  // save/restore the flag so the window composes with their own inner guard.
  static inline thread_local bool uni_batchqkt_in_batch_ = false;

  // Shipping default ON: batched graph-timestamp read (one batched query
  // list per step instead of per-node timestamp events; measured net
  // -4.25ms/step on node3 trim24L vs the legacy per-node shape). Set
  // UNITRACE_GRAPH_BATCH_QKT=0 to restore the legacy per-node shape for
  // A/B testing.
  static bool BatchQktEnvEnabled(void) {
    static const bool enabled = []() {
      const char *e = std::getenv("UNITRACE_GRAPH_BATCH_QKT");
      return (e == nullptr || !(e[0] == '0' && e[1] == '\0'));
    }();
    return enabled;
  }

  static bool BatchQktDiagEnabled(void) {
    static const bool enabled = (std::getenv("UNITRACE_DEBUG_BATCHQKT") != nullptr);
    return enabled;
  }

  // HARDEN (UNITRACE_TEST_ALLOC_FAIL=1): force every BATCHQKT allocation
  // (host staging buffer, immediate command list) to fail so the
  // graceful-skip path can be exercised on a healthy node. Read once.
  static bool BatchQktTestAllocFail(void) {
    static const bool enabled = []() {
      const char *e = std::getenv("UNITRACE_TEST_ALLOC_FAIL");
      return (e != nullptr && e[0] == '1' && e[1] == '\0');
    }();
    return enabled;
  }

  // HARDEN: true while any command list is inside a graph capture window.
  inline bool BatchQktCaptureHold(void) {
    return uni_batchqkt_capture_depth_.load(std::memory_order_acquire) > 0;
  }

  // HARDEN: someone requested BATCHQKT resources for a context handle the
  // app already destroyed (L0 recycles handle values, so this is the exact
  // pre-fix path that served a dead list/buffer to a fresh context and
  // programmed it into the CCS stream). Counted, printed once.
  void BatchQktNoteDeadContext(void) {
    uni_batchqkt_dead_ctx_skips_.fetch_add(1, std::memory_order_relaxed);
    if (!uni_batchqkt_notice_deadctx_.exchange(true, std::memory_order_relaxed)) {
      std::cerr << "[BATCHQKT] resource request for a destroyed context handle, "
                << "staying on the legacy per-event path (see dead_ctx_skips)" << std::endl;
    }
  }

  // -----------------------------------------------------------------------
  // T14/A5 (UNITRACE_TRACE_META): the collector's own debt becomes visible in
  // the chrome trace as cat="unitrace_meta" child slices of the host API call
  // it runs inside (see IMPL_NOTES_meta.md for the record table). Default ON;
  // exactly "0" disables -- the call sites then skip the arg-string build and
  // the output stays byte-identical to the pre-meta collector. The writer
  // (ChromeLogger::MetaLoggingCallback) re-checks the same gate.
  // -----------------------------------------------------------------------
  static bool TraceMetaEnvEnabled(void) {
    static const bool enabled = []() {
      const char *e = std::getenv("UNITRACE_TRACE_META");
      return (e == nullptr || !(e[0] == '0' && e[1] == '\0'));
    }();
    return enabled;
  }

  // Hot-path check: one atomic-free branch when the gate is off (the callback
  // is always wired, so the gate decides).
  inline bool MetaOn(void) const {
    return TraceMetaEnvEnabled() && (mcallback_ != nullptr);
  }

  // One meta record. |name| is the full record name; |mt0|/|mt1| are the
  // UniTimer::GetHostTimestamp() bounds of the measured window (an instant
  // when equal); |args| is the JSON object body; |counter_ms| >= 0 additionally
  // emits the ph=C unitrace_overhead_ms debt sample (see PrepareGraphExecution).
  inline void MetaRecord(const char* name, uint64_t mt0, uint64_t mt1,
                         const std::string& args, double counter_ms = -1.0) {
    if (MetaOn()) {
      // Debt ledger: every meta slice feeds the per-step overhead counter.
      if (mt1 > mt0) {
        uni_meta_debt_us_.fetch_add((mt1 - mt0) / 1000, std::memory_order_relaxed);
      }
      mcallback_(name, mt0, mt1, args.c_str(), counter_ms);
    }
  }

  // T14/A5: 1-based graph-replay step number (the "unitrace.step <N>" landmark)
  // and the meta microseconds accumulated since the last step sample.
  std::atomic<uint64_t> uni_meta_step_{0};
  std::atomic<uint64_t> uni_meta_debt_us_{0};

  // TAX-OPT (enqueue-phase attribution): the exposed tax lives in the
  // urEnqueueGraphExp enqueue phase (node3 trim24L: 1.53ms/step in the device
  // gap, vs 5.80ms for the legacy u0 arm). The meta records cover emit/reset/
  // qkt but not the append path, so time the collector's own work per phase
  // and admit it as one "unitrace.enqueue" record at the end of every replay's
  // PrepareGraphExecution. All relaxed -- attribution only.
  std::atomic<uint64_t> uni_ph_evcollect_us_{0};  // graph_events + lists_to_wait collection
  std::atomic<uint64_t> uni_ph_drain_us_{0};      // ProcessAllCommandsSubmitted (collect+execute+reset batch)
  std::atomic<uint64_t> uni_ph_clone_us_{0};      // per-replay command clone loop
  std::atomic<uint64_t> uni_ph_appendk_us_{0};    // eager kernel append: exit-side AppendLaunchKernel
  std::atomic<uint64_t> uni_ph_prepk_us_{0};      // eager kernel append: enter-side PrepareToAppendKernelCommand
  std::atomic<uint64_t> uni_ph_appendk_n_{0};
  std::atomic<uint64_t> uni_ph_prepk_n_{0};
  std::atomic<uint64_t> uni_ph_evcollect_n_{0};
  std::atomic<uint64_t> uni_ph_drain_n_{0};
  std::atomic<uint64_t> uni_ph_clone_n_{0};

  // TAX-OPT drain2 (sub-drain attribution): the drain itself is 1.41ms/step
  // on node3 while its known tenants (reset batch ~0.51, qkt read ~0.37) leave
  // ~0.53ms unexplained. Time each statement group inside
  // ProcessAllCommandsSubmitted at the drain call site only -- the same
  // functions also run from poll sweeps, so call-site timers (not in-function
  // ones) keep the poll-sweep work out of these counters. All relaxed.
  std::atomic<uint64_t> uni_pd_flush1_us_{0};   // entry FlushDeferredTimestamps
  std::atomic<uint64_t> uni_pd_lock_us_{0};     // exclusive submissions lock acquire
  std::atomic<uint64_t> uni_pd_consume_us_{0};  // BatchQktConsumeDeferredEmit(at_drain)
  std::atomic<uint64_t> uni_pd_pollread_us_{0}; // CompletePollRead / Collect+Execute chain
  std::atomic<uint64_t> uni_pd_loop_us_{0};     // global submissions clone loop
  std::atomic<uint64_t> uni_pd_resets_us_{0};   // FlushGraphEventResets (reset batch)
  std::atomic<uint64_t> uni_pd_flush2_us_{0};   // exit FlushDeferredTimestamps
  std::atomic<uint64_t> uni_pd_flush1_n_{0};
  std::atomic<uint64_t> uni_pd_lock_n_{0};
  std::atomic<uint64_t> uni_pd_consume_n_{0};
  std::atomic<uint64_t> uni_pd_pollread_n_{0};
  std::atomic<uint64_t> uni_pd_loop_n_{0};
  std::atomic<uint64_t> uni_pd_resets_n_{0};
  std::atomic<uint64_t> uni_pd_flush2_n_{0};
  // drain2 loop-shape counters (gexp-site drain only; store-per-drain):
  // outer = containers in the global set, iter = commands visited, take =
  // batch-packet inline consumes, acc = clones kept queued (accumulate),
  // lq = legacy timestamp-event status calls.
  std::atomic<uint64_t> uni_pd_outer_n_{0};
  std::atomic<uint64_t> uni_pd_iter_n_{0};
  std::atomic<uint64_t> uni_pd_take_n_{0};
  std::atomic<uint64_t> uni_pd_acc_n_{0};
  std::atomic<uint64_t> uni_pd_lq_n_{0};

  // RAII accumulator for the phase timers above: every exit path (early
  // returns included) lands in the counter. Deliberately not gated by MetaOn()
  // -- two relaxed atomic adds per call are noise next to the work measured.
  struct UniPhaseTimer {
    std::atomic<uint64_t>& us_;
    std::atomic<uint64_t>& n_;
    std::chrono::steady_clock::time_point t0_;
    UniPhaseTimer(std::atomic<uint64_t>& us, std::atomic<uint64_t>& n)
        : us_(us), n_(n), t0_(std::chrono::steady_clock::now()) {}
    ~UniPhaseTimer() {
      us_.fetch_add(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - t0_).count()),
          std::memory_order_relaxed);
      n_.fetch_add(1, std::memory_order_relaxed);
    }
    UniPhaseTimer(const UniPhaseTimer&) = delete;
    UniPhaseTimer& operator=(const UniPhaseTimer&) = delete;
  };

  // E6-v4c knob, shipping default ON: arm the step's batch read at a
  // device-busy poll sweep (device-side wait on the step's own events)
  // instead of appending + synchronizing it at the staging drain. Set
  // UNITRACE_GRAPH_QKT_AT_POLL=0 to keep the v4b shape (read resolved at the
  // staging drain) for A/B testing.
  static bool QktAtPollEnvEnabled(void) {
    static const bool enabled = []() {
      const char *e = std::getenv("UNITRACE_GRAPH_QKT_AT_POLL");
      return (e == nullptr || !(e[0] == '0' && e[1] == '\0'));
    }();
    return enabled;
  }

  // Master gate of the poll-armed read. Inherits the batch gate's TSLOG2
  // force-off (diagnosis modes keep their exact inline ordering) and is false
  // whenever UNITRACE_GRAPH_BATCH_QKT=0 disables the batch gate.
  inline bool BatchQktAtPollActive(void) {
    return BatchQktActive() && QktAtPollEnvEnabled();
  }

  // Master gate. TSLOG2 diagnosis modes force the legacy per-event query path
  // so their exact inline-ordering guarantees hold (one notice line).
  inline bool BatchQktActive(void) {
    if (!uni_batchqkt_enabled_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (Tslog2Mode() != 0) {
      if (!uni_batchqkt_ts2_notice_.exchange(true, std::memory_order_relaxed)) {
        std::cerr << "[BATCHQKT] UNITRACE_GRAPH_BATCH_QKT ignored: UNITRACE_DEBUG_TS2="
                  << Tslog2Mode() << " active, keeping the legacy per-event query path" << std::endl;
      }
      return false;
    }
    return true;
  }

  // Readiness of one clone's shared event, memoized per sweep over DISTINCT
  // events: a graph whose clones share one step event costs ONE status call
  // for the whole pending set no matter how many clones carry it, while
  // multi-event graphs still get every event checked (and only clones whose
  // event is proven signaled join the batch — that ordering stays the
  // not-ready hot-spin protection). Past the set capacity the memoization
  // degrades to per-clone calls, which is slower but never unsound.
  bool BatchQktEventReady(ze_event_handle_t event, ze_event_handle_t known_signaled,
                          ze_event_handle_t (&seen)[kBatchQktMaxDistinctEvents],
                          bool (&ready)[kBatchQktMaxDistinctEvents],
                          size_t &seen_count) {
    if (event == known_signaled) {
      return true;
    }
    for (size_t i = 0; i < seen_count; i++) {
      if (seen[i] == event) {
        return ready[i];
      }
    }
    const bool is_ready = (ZE_FUNC(zeEventQueryStatus)(event) == ZE_RESULT_SUCCESS);
    if (seen_count < kBatchQktMaxDistinctEvents) {
      seen[seen_count] = event;
      ready[seen_count] = is_ready;
      seen_count++;
    }
    return is_ready;
  }

  // Collects graph replay clones from one submission list, in list order,
  // whose shared events are proven signaled (per-distinct-event gate, see
  // BatchQktEventReady). `known_signaled` is the event whose signal the sweep
  // caller already proved (the polled event); clones bound to it join without
  // a status call. Eager commands are never collected. The reentrancy guard
  // keeps the status calls' tracing callbacks from re-entering a sweep while
  // the submission lock is held.
  void BatchQktCollectReady(std::list<ZeCommand *> &commands_submitted,
                            ze_event_handle_t known_signaled, ZeBatchQkt &batch) {
    batch.ok = false;
    batch.cursor = 0;
    batch.cmds.clear();
    batch.ts.clear();
    size_t seen = 0;
    ze_event_handle_t seen_events[kBatchQktMaxDistinctEvents] = {};
    bool seen_ready[kBatchQktMaxDistinctEvents] = {};
    size_t seen_count = 0;
    const bool prev_guard = uni_batchqkt_in_batch_;
    uni_batchqkt_in_batch_ = true;
    for (ZeCommand *command : commands_submitted) {
      if (!command->graph_command_ || command->event_ == nullptr ||
          command->device_global_timestamps_ != nullptr ||
          command->timestamps_on_event_reset_ != nullptr ||
          command->deferred_ts_ != nullptr) {  // E6-v4b: packet read, emit pending
        continue;
      }
      seen++;
      if (BatchQktEventReady(command->event_, known_signaled, seen_events, seen_ready, seen_count)) {
        batch.cmds.push_back(command);
      }
    }
    uni_batchqkt_in_batch_ = prev_guard;
    BatchQktAccountSweep(seen, batch.cmds.size());
  }

  // All-submissions variant of BatchQktCollectReady for the generic drain
  // (same per-distinct-event readiness gate, no known-signaled shortcut).
  // Called with the sweep's exclusive submission lock held.
  void BatchQktCollectStatusGated(ZeBatchQkt &batch) {
    batch.ok = false;
    batch.cursor = 0;
    batch.cmds.clear();
    batch.ts.clear();
    size_t seen = 0;
    ze_event_handle_t seen_events[kBatchQktMaxDistinctEvents] = {};
    bool seen_ready[kBatchQktMaxDistinctEvents] = {};
    size_t seen_count = 0;
    const bool prev_guard = uni_batchqkt_in_batch_;
    uni_batchqkt_in_batch_ = true;
    if (global_device_submissions_ != nullptr) {
      for (auto s : *global_device_submissions_) {
        for (ZeCommand *command : s->commands_submitted_) {
          if (!command->graph_command_ || command->event_ == nullptr ||
              command->device_global_timestamps_ != nullptr ||
              command->timestamps_on_event_reset_ != nullptr ||
              command->deferred_ts_ != nullptr) {  // E6-v4b: packet read, emit pending
            continue;
          }
          seen++;
          if (BatchQktEventReady(command->event_, nullptr, seen_events, seen_ready, seen_count)) {
            batch.cmds.push_back(command);
          }
        }
      }
    }
    uni_batchqkt_in_batch_ = prev_guard;
    BatchQktAccountSweep(seen, batch.cmds.size());
  }

  // Diagnostics: how many batch-enabled sweeps saw graph clones, how many
  // clones were seen in total and how many actually joined a batch. With the
  // gq1 zero-batch signature these three counters localize the failing stage:
  // sweeps=0 -> the gate never armed; seen>0 collected=0 -> clones present but
  // never ready at sweep time; collected>0 batches=0 -> BatchQktExecute bailed
  // (its one-shot [BATCHQKT] skip lines name the reason).
  void BatchQktAccountSweep(size_t seen, size_t collected) {
    if (seen == 0) {
      return;
    }
    uni_batchqkt_sweeps_.fetch_add(1, std::memory_order_relaxed);
    uni_batchqkt_seen_.fetch_add(seen, std::memory_order_relaxed);
    uni_batchqkt_collected_.fetch_add(collected, std::memory_order_relaxed);
    if (collected == 0 &&
        !uni_batchqkt_notice_notsig_.exchange(true, std::memory_order_relaxed) &&
        BatchQktDiagEnabled()) {
      // v3: expected for mid-step accumulate sweeps — the clones flush at the
      // next staging drain, so this only names a sweep where nothing was
      // ready to join a flush that ran.
      std::cerr << "[BATCHQKT] seen=" << seen
                << " graph clones, none signaled at this sweep (accumulate mode: flush at the next drain)" << std::endl;
    }
  }

  // Cursor match: returns the batched packet when `command` is the next
  // batched command in sweep order, nullptr otherwise (O(1) per command; the
  // collection and the consuming loop walk the same lists in the same order).
  inline const ze_kernel_timestamp_result_t *BatchQktTake(ZeBatchQkt &batch, ZeCommand *command) {
    if (!batch.ok || batch.cursor >= batch.cmds.size() || batch.cmds[batch.cursor] != command) {
      return nullptr;
    }
    return &batch.ts[batch.cursor++];
  }

  // v3 accumulate predicate: this clone's packet belongs to the pending batch
  // and is consumed at the next flush (staging drain or a fence/teardown
  // backstop), so the sweep must NOT touch it — no status call, no query, no
  // legacy consumption. Passing `armed=false` (gate off, TS2 force-off, or a
  // batch that just failed) keeps the clone on the legacy inline path, which
  // is the whole fallback chain. Clones reading host buffers never accumulate
  // (they were never batchable).
  // E6-v4b: a clone whose packet is already read and whose emit is deferred
  // (deferred_ts_) accumulates regardless of `armed` — its packet must come
  // from the held buffer, never from a fresh query, and it must stay queued
  // until its emit runs. deferred_ts_ is only ever set with the gate on, so
  // the gate-off path is unchanged.
  inline bool BatchQktAccumulate(const ZeCommand *command, bool armed) const {
    // HARDEN: a null submission entry can never be dereferenced (defensive;
    // a false here just leaves the step on the legacy path).
    return command != nullptr &&
           (armed || command->deferred_ts_ != nullptr) &&
           command->graph_command_ && command->event_ != nullptr &&
           command->device_global_timestamps_ == nullptr &&
           command->timestamps_on_event_reset_ == nullptr;
  }

  // E6-v4c: the clone set a poll-armed read may cover. Same batchable shape
  // as BatchQktAccumulate, but a clone whose packet is already read and whose
  // emit is deferred (deferred_ts_) is EXCLUDED here — it is waiting for its
  // emit pass, not for a read, and re-arming over it would double-consume its
  // pending counter and overwrite the emit slot the emit pass is about to
  // read. Unlike the drain's collect there is NO readiness gate: the device
  // waits for the events (numWaitEvents == numEvents), which is the whole
  // point — the read is issued while the step is still running. The protection
  // a readiness gate used to give a dead handle is replaced by the event-cache
  // ownership check (same gate BatchQktResetEvents applies before appending a
  // parked handle), so a clone stranded by a destroyed graph can never reach
  // the poll list.
  bool BatchQktPollBatchable(ZeCommand *command) {
    if (command == nullptr || command->deferred_ts_ != nullptr) {
      return false;  // null is never batchable; E6-v4b: packet read, emit pending — not ours to read
    }
    if (!command->graph_command_ || command->event_ == nullptr ||
        command->device_global_timestamps_ != nullptr ||
        command->timestamps_on_event_reset_ != nullptr) {
      return false;
    }
    return event_cache_.QueryEvent(command->event_);
  }

  // One compute-capable queue ordinal per device, for the immediate list. The
  // bool says whether one was found; 0 is a valid ordinal, hence the wrapper.
  bool BatchQktComputeOrdinal(ze_device_handle_t device, uint32_t &ordinal) {
    auto it = uni_batchqkt_ordinals_.find(device);
    if (it != uni_batchqkt_ordinals_.end()) {
      ordinal = it->second.first;
      return it->second.second;
    }
    uint32_t count = 0;
    bool found = false;
    uint32_t pick = 0;
    ze_result_t status = ZE_FUNC(zeDeviceGetCommandQueueGroupProperties)(device, &count, nullptr);
    if (status == ZE_RESULT_SUCCESS && count > 0) {
      std::vector<ze_command_queue_group_properties_t> props(count);
      status = ZE_FUNC(zeDeviceGetCommandQueueGroupProperties)(device, &count, props.data());
      if (status == ZE_RESULT_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
          if (props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE) {
            pick = i;
            found = true;
            break;
          }
        }
      }
    }
    if (!found) {
      std::cerr << "[BATCHQKT] no compute queue ordinal found, graph replay timestamps stay on the per-event path" << std::endl;
    }
    uni_batchqkt_ordinals_[device] = {pick, found};
    ordinal = pick;
    return found;
  }

  // Collector-owned persistent immediate command list, lazily created per
  // context. A nullptr entry means creation failed: permanently fall back to
  // the legacy per-event path for that context (no retry storm, one line).
  ze_command_list_handle_t BatchQktEnsureImmList(ze_context_handle_t context, ze_device_handle_t device) {
    // HARDEN: a destroyed context's handle value is never trusted again.
    if (uni_batchqkt_dead_contexts_.count(context) != 0) {
      BatchQktNoteDeadContext();
      return nullptr;
    }
    auto it = uni_batchqkt_imm_lists_.find(context);
    if (it != uni_batchqkt_imm_lists_.end()) {
      return it->second;
    }
    uint32_t ordinal = 0;
    ze_command_list_handle_t imm = nullptr;
    ze_result_t status = ZE_RESULT_SUCCESS;
    if (BatchQktTestAllocFail()) {
      // HARDEN: forced failure (UNITRACE_TEST_ALLOC_FAIL=1); counted below.
      status = ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }
    else if (BatchQktComputeOrdinal(device, ordinal)) {
      ze_command_queue_desc_t qdesc = {};
      qdesc.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC;
      qdesc.ordinal = ordinal;
      qdesc.index = 0;
      qdesc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
      qdesc.priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL;
      status = ZE_FUNC(zeCommandListCreateImmediate)(context, device, &qdesc, &imm);
      if (status != ZE_RESULT_SUCCESS) {
        imm = nullptr;
      }
    }
    if (imm == nullptr) {
      // HARDEN: counted, visible with the driver status; the sticky nullptr
      // fallback below keeps this context on the legacy per-event path
      // (never a bad enqueue).
      uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
      std::cerr << "[BATCHQKT] immediate command list creation failed (status = 0x"
                << std::hex << status << std::dec
                << "), graph replay timestamps stay on the per-event path" << std::endl;
    }
    uni_batchqkt_imm_lists_[context] = imm;
    return imm;
  }

  // E6-v4c: a SECOND collector-owned immediate list per context, used only by
  // the poll-armed read. Keeping it off the drain/reset list means nothing
  // else the collector appends (reset batches, a fallback BatchQktExecute) can
  // ever end up behind a waiting read — only this list ever hosts a
  // device-side wait, and only the drain that owns the arm synchronizes it.
  ze_command_list_handle_t BatchQktEnsurePollList(ze_context_handle_t context,
                                                  ze_device_handle_t device) {
    // HARDEN: a destroyed context's handle value is never trusted again.
    if (uni_batchqkt_dead_contexts_.count(context) != 0) {
      BatchQktNoteDeadContext();
      return nullptr;
    }
    auto it = uni_batchqkt_poll_lists_.find(context);
    if (it != uni_batchqkt_poll_lists_.end()) {
      return it->second;
    }
    uint32_t ordinal = 0;
    ze_command_list_handle_t imm = nullptr;
    ze_result_t status = ZE_RESULT_SUCCESS;
    if (BatchQktTestAllocFail()) {
      // HARDEN: forced failure (UNITRACE_TEST_ALLOC_FAIL=1); counted below.
      status = ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }
    else if (BatchQktComputeOrdinal(device, ordinal)) {
      ze_command_queue_desc_t qdesc = {};
      qdesc.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC;
      qdesc.ordinal = ordinal;
      qdesc.index = 0;
      qdesc.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
      qdesc.priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL;
      status = ZE_FUNC(zeCommandListCreateImmediate)(context, device, &qdesc, &imm);
      if (status != ZE_RESULT_SUCCESS) {
        imm = nullptr;
      }
    }
    if (imm == nullptr) {
      // HARDEN: counted, visible; sticky fallback, never a bad enqueue.
      uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
      if (!uni_batchqkt_notice_polllist_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] poll immediate command list creation failed (status = 0x"
                  << std::hex << status << std::dec << "), "
                  << "QKT_AT_POLL stays on the drain read for this context" << std::endl;
      }
    }
    uni_batchqkt_poll_lists_[context] = imm;
    return imm;
  }

  // Persistent host staging buffer for the batch packets, grown on demand and
  // reused across replays. At most one batch is ever in flight: the next
  // append is only issued after the previous zeCommandListHostSynchronize
  // returned, and the packets are copied out before this returns.
  // E6-v4c: the core is shared with the poll read's own buffer map, so a poll
  // read's packets can never be clobbered by a drain-side execute (they live
  // in different allocations for the whole life of an arm).
  void *BatchQktEnsureBufferIn(std::map<ze_context_handle_t, void *> &bufs,
                               std::map<ze_context_handle_t, size_t> &caps,
                               ze_context_handle_t context, size_t bytes) {
    // HARDEN: a destroyed context's handle value is never trusted again (its
    // host allocations died with the context; serving them to a recycled
    // handle would hand the device a dangling dst).
    if (uni_batchqkt_dead_contexts_.count(context) != 0) {
      BatchQktNoteDeadContext();
      return nullptr;
    }
    void *&buf = bufs[context];
    size_t &cap = caps[context];
    if (buf != nullptr && cap >= bytes) {
      return buf;
    }
    size_t want = ((bytes + 4095) / 4096) * 4096;  // page-granular, cache-line safe
    if (buf != nullptr) {
      ze_result_t status = ZE_FUNC(zeMemFree)(context, buf);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[WARNING] Failed to free grown staging buffer (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
      }
      buf = nullptr;
      cap = 0;
    }
    // HARDEN (UNITRACE_TEST_ALLOC_FAIL=1): pretend the allocation failed so
    // the graceful-skip path can be exercised without a degraded device.
    if (BatchQktTestAllocFail()) {
      uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
      buf = nullptr;
      cap = 0;
      return nullptr;
    }
    ze_host_mem_alloc_desc_t host_desc = {};
    host_desc.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC;
    ze_result_t status = ZE_FUNC(zeMemAllocHost)(context, &host_desc, want, 64, &buf);
    if (status != ZE_RESULT_SUCCESS) {
      // HARDEN: counted + one-shot visible line. Callers (BatchQktExecute /
      // BatchQktArmPollRead) see dst == nullptr and skip the batch -- a clone
      // read is never enqueued with a bad handle.
      uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
      if (!uni_batchqkt_notice_allocfail_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] host staging allocation failed (status=0x" << std::hex << status
                  << std::dec << "), batch reads skip gracefully (see qkt_skip_alloc_fail)" << std::endl;
      }
      buf = nullptr;
      cap = 0;
      return nullptr;
    }
    cap = want;
    return buf;
  }

  void *BatchQktEnsureBuffer(ze_context_handle_t context, size_t bytes) {
    return BatchQktEnsureBufferIn(uni_batchqkt_bufs_, uni_batchqkt_buf_caps_, context, bytes);
  }

  // Reads the packets of all collected commands in one device-side batch, then
  // copies them out. Returns true on success (batch.ok set, consume via
  // BatchQktTake); false = this step falls back to the legacy per-event path
  // (the batch is emptied so the cursor never matches).
  bool BatchQktExecute(ZeBatchQkt &batch) {
    batch.ok = false;
    const size_t n = batch.cmds.size();
    if (n < kBatchQktMinCommands) {
      // v2: no longer silent — a persistent below-min batch on a graph ladder
      // means the replay step is fragmented into tiny clone groups.
      if (!uni_batchqkt_notice_min_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] skip: n=" << n << " below min=" << kBatchQktMinCommands << std::endl;
      }
      batch.cmds.clear();
      return false;
    }
    if (n > 0xFFFFFFFFull) {
      // HARDEN: the append API takes uint32_t; this cannot happen by
      // construction (one replay step's clones), never cast blindly.
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] skip: batch size overflows uint32 (n=" << n << ")" << std::endl;
      }
      batch.cmds.clear();
      return false;
    }
    if (!ZE_HAVE_FUNC(zeCommandListCreateImmediate) ||
        !ZE_HAVE_FUNC(zeCommandListAppendQueryKernelTimestamps) ||
        !ZE_HAVE_FUNC(zeCommandListHostSynchronize) ||
        !ZE_HAVE_FUNC(zeMemAllocHost) || !ZE_HAVE_FUNC(zeMemFree)) {
      if (!uni_batchqkt_notice_syms_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] skip: loader symbols missing" << std::endl;
      }
      batch.cmds.clear();
      return false;
    }
    // All batched clones must share one context/device: they come from one
    // graph replay, so this only bails out for exotic mixed-graph sweeps.
    ze_context_handle_t context = batch.cmds.front()->context_;
    ze_device_handle_t device = batch.cmds.front()->device_;
    if (context == nullptr || device == nullptr) {
      if (!uni_batchqkt_notice_ctx_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] skip: clone context/device unresolved (context_=0 on staged clones)"
                  << std::endl;
      }
      batch.cmds.clear();
      return false;
    }
    std::vector<ze_event_handle_t> events(n);
    for (size_t i = 0; i < n; i++) {
      // HARDEN: a null command pointer in the batch can never be dereferenced
      // (cannot happen by construction -- batches hold collected commands --
      // but the deref below would be a straight crash if that ever changed).
      if (batch.cmds[i] == nullptr) {
        if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&
            BatchQktDiagEnabled()) {
          std::cerr << "[BATCHQKT] skip: null command pointer in batch (i=" << i << ")" << std::endl;
        }
        batch.cmds.clear();
        return false;
      }
      if (batch.cmds[i]->context_ != context || batch.cmds[i]->device_ != device) {
        if (!uni_batchqkt_notice_mixed_.exchange(true, std::memory_order_relaxed) &&
            BatchQktDiagEnabled()) {
          std::cerr << "[BATCHQKT] skip: mixed context/device in one sweep" << std::endl;
        }
        batch.cmds.clear();
        return false;
      }
      // HARDEN: a null event handle must never reach
      // zeCommandListAppendQueryKernelTimestamps -- the collector gates
      // clones on event_ != nullptr at collect time, but the batch is
      // consumed a few lines later; re-check at the last moment and drop the
      // whole batch instead of enqueueing a null handle.
      if (batch.cmds[i]->event_ == nullptr) {
        uni_batchqkt_alloc_fail_.fetch_add(1, std::memory_order_relaxed);
        batch.cmds.clear();
        return false;
      }
      events[i] = batch.cmds[i]->event_;
    }

    // HARDEN: never issue collector-side device work while any command list
    // is inside a graph capture window. Capture appends create no clones, so
    // whatever is pending belongs to an earlier replay and can wait for the
    // first drain after capture ends (ready clones are re-collected there).
    if (BatchQktCaptureHold()) {
      uni_batchqkt_capture_skip_.fetch_add(1, std::memory_order_relaxed);
      batch.cmds.clear();
      return false;
    }

    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    ze_command_list_handle_t imm = BatchQktEnsureImmList(context, device);
    void *dst = (imm != nullptr) ? BatchQktEnsureBuffer(context, n * sizeof(ze_kernel_timestamp_result_t)) : nullptr;
    if (imm == nullptr || dst == nullptr) {
      batch.cmds.clear();
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    const bool prev_guard = uni_batchqkt_in_batch_;
    uni_batchqkt_in_batch_ = true;  // our L0 calls must not re-enter the sweeps
    // T14/A5: this read runs at the staging drain, on the app's critical path.
    const bool meta = MetaOn();
    const uint64_t mt0 = meta ? UniTimer::GetHostTimestamp() : 0;
    auto t0 = std::chrono::steady_clock::now();
    ze_result_t status = ZE_FUNC(zeCommandListAppendQueryKernelTimestamps)(
        imm, static_cast<uint32_t>(n), events.data(), dst,
        /*pOffsets=*/nullptr, /*hSignalEvent=*/nullptr, /*numWaitEvents=*/0, /*phWaitEvents=*/nullptr);
    if (status == ZE_RESULT_SUCCESS) {
      status = ZE_FUNC(zeCommandListHostSynchronize)(imm, UINT64_MAX);
    }
    auto t1 = std::chrono::steady_clock::now();
    const uint64_t mt1 = meta ? UniTimer::GetHostTimestamp() : 0;
    uni_batchqkt_in_batch_ = prev_guard;
    if (status != ZE_RESULT_SUCCESS) {
      // Append may have succeeded and the sync failed (device-lost class of
      // errors): nothing was consumed from dst, the next batch re-appends.
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      if (BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] append/synchronize failed (status=0x" << std::hex << status << std::dec
                  << "), this step falls back to the per-event path" << std::endl;
      }
      batch.cmds.clear();
      return false;
    }

    batch.ts.resize(n);
    std::memcpy(batch.ts.data(), dst, n * sizeof(ze_kernel_timestamp_result_t));
    uint64_t batch_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    uint64_t prev = uni_batchqkt_max_batch_us_.load(std::memory_order_relaxed);
    while (batch_us > prev &&
           !uni_batchqkt_max_batch_us_.compare_exchange_weak(prev, batch_us, std::memory_order_relaxed)) {
    }
    prev = uni_batchqkt_max_n_.load(std::memory_order_relaxed);
    while (n > prev && !uni_batchqkt_max_n_.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {
    }
    uni_batchqkt_batch_count_.fetch_add(1, std::memory_order_relaxed);
    uni_batchqkt_cmd_count_.fetch_add(n, std::memory_order_relaxed);
    batch.ok = true;
    if (meta) {
      // T14/A5: the v4b/legacy batch read, spent serially at the staging drain.
      MetaRecord("unitrace.qkt_batch", mt0, mt1,
                 "\"n\": " + std::to_string(n) +
                 ", \"us\": " + std::to_string(batch_us) +
                 ", \"at\": \"drain\"");
    }
    if (BatchQktDiagEnabled()) {
      std::cerr << "[BATCHQKT] n=" << n << " batch_us=" << batch_us << std::endl;
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // E6-v4c (UNITRACE_GRAPH_QKT_AT_POLL=1): arm the step's batch read at a
  // device-busy sweep instead of at the staging drain. The append is issued
  // with numWaitEvents == numEvents (the SAME event set the query reads), so
  // the device parks the query until every event of the running replay has
  // signaled and only then captures the packets into the poll buffer. The
  // packets are therefore read exactly once, at the same generation boundary
  // v4b reads them (all events signaled, nothing reset, nothing re-signaled
  // yet) — only the WAIT of that read is spent while the device is busy
  // instead of serially at the drain.
  //
  // Deliberately stateless for the clones: no ZeCommand field is written, no
  // event is released, no pending counter is consumed and no packet is copied
  // here. Every skip and every failure below simply leaves the clones queued
  // for the exact v4b drain read, so the fallback chain has nothing to
  // unwind. Called with the sweep's shared submission lock held (the drain
  // takes it exclusive, which is what serializes this against
  // BatchQktCompletePollRead); uni_batchqkt_read_pending_ is exchanged FIRST,
  // so two sweeps on two lists can never both arm.
  //
  // No readiness gate on purpose: gating would re-create v2's fragmentation
  // (only the clones ready at the one early poll sweep would join, ~2-6% of
  // the step). The not-ready hot spin the v3 gate protects against cannot
  // happen here — the wait is device-side (semaphore), not a host status poll.
  // -----------------------------------------------------------------------
  void BatchQktArmPollRead(void) {
    if (!BatchQktAtPollActive() || uni_batchqkt_in_batch_) {
      return;
    }
    // HARDEN: no device-side wait op is armed while a graph capture is open
    // (the arm's poll list could otherwise be the next boot's stale handle).
    if (BatchQktCaptureHold()) {
      uni_batchqkt_capture_skip_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (uni_batchqkt_read_pending_.load(std::memory_order_acquire)) {
      return;  // this or another thread's step already has its read armed
    }
    ZeBatchQktPollRead pr;
    for (ZeCommand *command : local_device_submissions_.commands_submitted_) {
      // HARDEN: never dereference a null submission entry (defensive; the
      // submission vectors only hold collected commands by construction).
      if (command == nullptr) {
        continue;
      }
      if (!BatchQktPollBatchable(command)) {
        continue;
      }
      if (pr.cmds.empty()) {
        pr.context = command->context_;
        pr.device = command->device_;
      }
      else if (command->context_ != pr.context || command->device_ != pr.device) {
        // Mixed pending set (another graph's stranded clones): leave the whole
        // step to the drain's readiness-gated collect, which handles that.
        if (!uni_batchqkt_notice_pollmixed_.exchange(true, std::memory_order_relaxed) &&
            BatchQktDiagEnabled()) {
          std::cerr << "[BATCHQKT] skip poll arm: mixed context/device in one list" << std::endl;
        }
        return;
      }
      pr.cmds.push_back(command);
      pr.events.push_back(command->event_);
    }
    const size_t n = pr.cmds.size();
    if (n < kBatchQktMinCommands) {
      return;  // nothing worth a batch — the drain path handles it as usual
    }
    if (n > 0xFFFFFFFFull) {
      // HARDEN: the append API takes uint32_t; leave the step to the drain.
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] skip poll arm: batch size overflows uint32 (n=" << n << ")" << std::endl;
      }
      return;
    }
    if (!ZE_HAVE_FUNC(zeCommandListCreateImmediate) ||
        !ZE_HAVE_FUNC(zeCommandListAppendQueryKernelTimestamps) ||
        !ZE_HAVE_FUNC(zeCommandListHostSynchronize) ||
        !ZE_HAVE_FUNC(zeMemAllocHost) || !ZE_HAVE_FUNC(zeMemFree)) {
      if (!uni_batchqkt_notice_syms_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] skip: loader symbols missing" << std::endl;
      }
      return;
    }
    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    // The guard covers the list/buffer creation too (first-time zeMemAllocHost
    // / zeCommandListCreateImmediate also come back through the tracing layer),
    // so no nested sweep can observe the half-published arm: the slot flag is
    // still false until the very end of this function.
    const bool prev_guard = uni_batchqkt_in_batch_;
    uni_batchqkt_in_batch_ = true;
    ze_command_list_handle_t imm = BatchQktEnsurePollList(pr.context, pr.device);
    void *dst = (imm != nullptr)
        ? BatchQktEnsureBufferIn(uni_batchqkt_poll_bufs_, uni_batchqkt_poll_buf_caps_,
                                 pr.context, n * sizeof(ze_kernel_timestamp_result_t))
        : nullptr;
    if (imm == nullptr || dst == nullptr) {
      uni_batchqkt_in_batch_ = prev_guard;
      return;  // sticky per-context fallback, counted at the drain if it happens
    }
    // T14/A5: the v4c arm issues the read inside a device-busy poll sweep --
    // its append cost is real host work in that sweep, so it is self-admitted.
    const bool meta = MetaOn();
    const uint64_t mt0 = meta ? UniTimer::GetHostTimestamp() : 0;
    auto t0 = std::chrono::steady_clock::now();
    ze_result_t status = ZE_FUNC(zeCommandListAppendQueryKernelTimestamps)(
        imm, static_cast<uint32_t>(n), pr.events.data(), dst,
        /*pOffsets=*/nullptr, /*hSignalEvent=*/nullptr,
        /*numWaitEvents=*/static_cast<uint32_t>(n), /*phWaitEvents=*/pr.events.data());
    auto t1 = std::chrono::steady_clock::now();
    const uint64_t mt1 = meta ? UniTimer::GetHostTimestamp() : 0;
    uni_batchqkt_in_batch_ = prev_guard;
    if (status != ZE_RESULT_SUCCESS) {
      // Driver refused the wait-events form (or the list): nothing was
      // consumed, the events are untouched, the drain does the v4b read.
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      if (!uni_batchqkt_notice_pollappend_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] poll append failed (status=0x" << std::hex << status << std::dec
                  << "), this and later steps stay on the drain read" << std::endl;
      }
      return;
    }
    pr.list = imm;
    pr.dst = dst;
    pr.n = n;
    pr.append_us =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    if (meta) {
      // T14/A5: the poll-side half of the v4c split read. The slice lies in
      // the poll sweep's host API slice (device busy), the drain-side half is
      // the "poll_sync" record BatchQktCompletePollRead emits.
      MetaRecord("unitrace.qkt_batch", mt0, mt1,
                 "\"n\": " + std::to_string(n) +
                 ", \"us\": " + std::to_string(pr.append_us) +
                 ", \"at\": \"poll\"");
    }
    pr.live = true;
    // E6-v4c: this sweep is the one that "saw" and collected the step's clones
    // now (the drain's BatchQktCollectStatusGated is skipped for an armed
    // read), so the seen/collected/sweeps funnel is accounted here — that is
    // what keeps seen == collected == cmds comparable with v4b. A failed
    // append does NOT account: those clones fall through to the drain's own
    // collect, which counts them exactly once.
    BatchQktAccountSweep(n, n);
    uni_batchqkt_poll_read_ = std::move(pr);
    uni_batchqkt_qkt_append_us_.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    // Published last: the flag is what a completing sweep checks (release/acquire).
    uni_batchqkt_read_pending_.store(true, std::memory_order_release);
  }

  // Drain-side half of the v4c split: synchronize the armed read (the device
  // resolved its wait when the step's last kernel signaled, and the drain only
  // runs after PrepareGraphExecution host-synchronized the graph's lists, so
  // this returns immediately in the healthy case), copy the packets out and
  // hand the caller a batch that is byte-for-byte what a v4b BatchQktExecute
  // would have produced. Returns false (having released the slot) when nothing
  // was armed or the sync failed — the caller then runs the unchanged v4b
  // collect + execute, which is safe because an armed read never touched any
  // clone state. Called with the exclusive submission lock held.
  bool BatchQktCompletePollRead(ZeBatchQkt &batch) {
    if (!BatchQktAtPollActive() || !uni_batchqkt_read_pending_.load(std::memory_order_acquire)) {
      return false;
    }
    batch.ok = false;
    batch.cursor = 0;
    batch.ts.clear();
    batch.cmds.clear();
    // The descriptor's single slot is owned under uni_batchqkt_mutex_ (the
    // arm writes it under the same mutex), so a fence/teardown sweep on
    // another thread — which holds only the SHARED submission lock — can
    // never race the drain's completion here: the second taker finds the flag
    // cleared and runs the caller's normal path.
    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    if (!uni_batchqkt_read_pending_.load(std::memory_order_acquire)) {
      return false;
    }
    ZeBatchQktPollRead pr = std::move(uni_batchqkt_poll_read_);
    uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
    uni_batchqkt_read_pending_.store(false, std::memory_order_release);
    if (!pr.live || pr.list == nullptr || pr.dst == nullptr || pr.cmds.empty()) {
      return false;
    }
    if (pr.n != pr.cmds.size() || pr.n != pr.events.size()) {
      // Cannot happen by construction; never let a desynchronized descriptor
      // reach the memcpy or the cursor match.
      uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
      if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&
          BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] poll descriptor desynchronized (n=" << pr.n
                  << " cmds=" << pr.cmds.size() << "), step falls back to the drain read" << std::endl;
      }
      return false;
    }
    for (size_t i = 0; i < pr.n; i++) {
      // HARDEN: a null command or event element must not reach the consumer
      // (cursor match would deref the command) or the packet copy below.
      if (pr.cmds[i] == nullptr || pr.events[i] == nullptr) {
        batch.cmds.clear();
        batch.ts.clear();
        if (!uni_batchqkt_notice_invariant_.exchange(true, std::memory_order_relaxed) &&
            BatchQktDiagEnabled()) {
          std::cerr << "[BATCHQKT] poll descriptor has a null element (i=" << i
                    << "), step falls back to the drain read" << std::endl;
        }
        return false;
      }
    }
    const size_t n = pr.n;
    batch.cmds = std::move(pr.cmds);
    batch.ts.resize(n);
    const bool prev_guard = uni_batchqkt_in_batch_;
    uni_batchqkt_in_batch_ = true;  // our L0 calls must not re-enter the sweeps
    // T14/A5: the drain-side half of the poll-armed read (sync + copy).
    const bool meta = MetaOn();
    const uint64_t mt0 = meta ? UniTimer::GetHostTimestamp() : 0;
    auto t0 = std::chrono::steady_clock::now();
    ze_result_t status = ZE_FUNC(zeCommandListHostSynchronize)(pr.list, UINT64_MAX);
    auto t1 = std::chrono::steady_clock::now();
    const uint64_t mt1 = meta ? UniTimer::GetHostTimestamp() : 0;
    uni_batchqkt_in_batch_ = prev_guard;
    if (status != ZE_RESULT_SUCCESS) {
      // Nothing was consumed from dst and no clone state changed: the drain
      // falls through to the v4b collect + execute for these same clones.
      batch.cmds.clear();
      batch.ts.clear();
      uni_batchqkt_fallback_count_.fetch_add(1, std::memory_order_relaxed);
      if (BatchQktDiagEnabled()) {
        std::cerr << "[BATCHQKT] poll read synchronize failed (status=0x" << std::hex << status
                  << std::dec << "), this step falls back to the drain read" << std::endl;
      }
      return false;
    }
    std::memcpy(batch.ts.data(), pr.dst, n * sizeof(ze_kernel_timestamp_result_t));
    const uint64_t batch_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    uni_batchqkt_qkt_sync_us_.fetch_add(batch_us, std::memory_order_relaxed);
    // max_batch_us keeps its v4b semantic: the SLOWEST SINGLE batch's
    // append+sync. The append half was measured at the arm and travels in the
    // descriptor — do NOT feed the run-cumulative qkt_batch_us here (the
    // first node3 run reported max_batch_us=136499 exactly because the
    // cumulative total leaked into this max).
    const uint64_t one_batch_us = pr.append_us + batch_us;
    uint64_t prev = uni_batchqkt_max_batch_us_.load(std::memory_order_relaxed);
    while (one_batch_us > prev &&
           !uni_batchqkt_max_batch_us_.compare_exchange_weak(prev, one_batch_us,
                                                             std::memory_order_relaxed)) {
    }
    // qkt_batch_us stays the run TOTAL (cost that left the drain).
    uint64_t total = uni_batchqkt_qkt_append_us_.load(std::memory_order_relaxed) +
                     uni_batchqkt_qkt_sync_us_.load(std::memory_order_relaxed);
    uni_batchqkt_qkt_batch_us_.store(total, std::memory_order_relaxed);
    prev = uni_batchqkt_max_n_.load(std::memory_order_relaxed);
    while (n > prev && !uni_batchqkt_max_n_.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {
    }
    uni_batchqkt_batch_count_.fetch_add(1, std::memory_order_relaxed);
    uni_batchqkt_cmd_count_.fetch_add(n, std::memory_order_relaxed);
    uni_batchqkt_qkt_at_poll_.fetch_add(n, std::memory_order_relaxed);
    batch.ok = true;
    if (meta) {
      // T14/A5: "poll_sync" = the drain-side half of the poll-armed read: the
      // wait resolved during the step, so this is only the sync + copy that
      // landed at the staging drain. The poll-side append is the "poll"
      // record; args carry both halves' costs.
      MetaRecord("unitrace.qkt_batch", mt0, mt1,
                 "\"n\": " + std::to_string(n) +
                 ", \"us\": " + std::to_string(batch_us) +
                 ", \"at\": \"poll_sync\", \"append_us\": " + std::to_string(pr.append_us) + "");
    }
    if (BatchQktDiagEnabled()) {
      std::cerr << "[BATCHQKT] n=" << n << " poll_read_us=" << batch_us << std::endl;
    }
    return true;
  }

  // -----------------------------------------------------------------------
  // E6-v4b: split the flush. gq3 (reset batching already in, TSBAD=0,
  // fallback=0) showed the drain's ~66ms is NOT the resets (1.3ms) and mostly
  // not the batch read (~7ms) but the consume+emit of ~770 clones (~75us each
  // of record machinery: call/itt/kernel records + args + fwrite). Legacy hid
  // that host work inside poll sweeps where the device was busy; v3/v4 run it
  // at the staging drain, where the device is idle, so it lands on the app's
  // critical path a second time (+65ms ITL).
  //
  // The split point is dictated by packet integrity, not by preference: the
  // captured commands reuse the SAME physical events on every replay, so once
  // the next replay is staged/submitted the device overwrites those packets
  // with the new generation. The READ must therefore happen while the packets
  // are still this generation's — i.e. at the staging drain, right after the
  // list host-synchronize proved the whole replay complete and before the
  // clone loop re-signals the events. The EMIT is pure host work on the
  // already-copied packets and can run anywhere: it is deferred to the first
  // sweep after the drain (the app's poll of the replayed graph, which fires
  // while the device executes the new step), so it overlaps device execution
  // instead of serializing against an idle device.
  //
  // Mechanism: BatchQktArmDeferredEmit copies the batch packets into
  // uni_batchqkt_emit_buf_ and hangs each command's slot on deferred_ts_
  // (cleared by GetKernelCommand on recycle, so a recycled command can never
  // inherit a stale slot). BatchQktConsumeDeferredEmit is the emit pass; it
  // runs at the top of a sweep, walks the submission lists in the same order
  // the drain collected them, and consumes exactly the deferred_ts_ commands
  // through the pre_ts tail — emit order, TSBAD semantics, pending-clone
  // accounting and the v4 batched resets are unchanged, only WHEN the emit
  // runs moved. uni_batchqkt_flush_pending_ is the fast-path hint that a sweep
  // has something to find; the drain's backstop pass ignores it and always
  // scans, so clones deferred by another thread's staging are never stranded.
  // Only one arm can be live at a time: arming happens only at the drain,
  // right after its backstop pass consumed the previous arm, and the
  // submission lock (shared for a poll pass, exclusive for a drain pass)
  // serializes arming against consuming.
  // -----------------------------------------------------------------------

  // Parks a just-executed batch for a later sweep's emit. Called with the
  // exclusive submission lock held, immediately after BatchQktExecute
  // succeeded and after the backstop pass consumed any previous arm (so
  // overwriting uni_batchqkt_emit_buf_ cannot invalidate a live slot).
  //
  // This is also where the clone's EVENT is released, not at the emit: the
  // packets are buffered, so no clone needs the event anymore, and releasing
  // here keeps the v3/v4 reset timing exactly (the batched reset runs at the
  // end of THIS drain sweep, before PrepareGraphExecution's clone loop
  // re-signals the events for the next replay). Releasing at the emit instead
  // would (a) leave the events latched across the staging — the readiness gate
  // of the next drain would then collect clones whose packets the device has
  // already overwritten — and (b) ratchet the per-event pending counter up by
  // one per generation, so it would never reach zero and never reset.
  void BatchQktArmDeferredEmit(ZeBatchQkt &batch) {
    const size_t n = batch.cmds.size();
    uni_batchqkt_emit_buf_ = batch.ts;  // packets must outlive this sweep
    for (size_t i = 0; i < n; i++) {
      ZeCommand *command = batch.cmds[i];
      if (command == nullptr) {
        continue;  // HARDEN: cannot happen (batches are element-validated), never write through null
      }
      command->deferred_ts_ = &uni_batchqkt_emit_buf_[i];
      BatchQktReleaseArmedEvent(command);
    }
    batch.ok = false;  // the drain's own loop must not consume them (BatchQktTake)
    batch.cursor = 0;
    batch.cmds.clear();
    batch.ts.clear();
    uni_batchqkt_flush_pending_.store(true, std::memory_order_release);
  }

  // The tail's event-release block, run at arm time for a clone whose emit is
  // deferred. Identical semantics to the inline path — MarkReset, pending-clone
  // accounting, and the v4 park-or-host-reset gate — just earlier: the emit
  // reads the buffered packet, so the event is free the moment the batch read
  // returned. ProcessCommandSubmittedTail skips this block for a deferred emit
  // (deferred_emit=true).
  void BatchQktReleaseArmedEvent(ZeCommand *command) {
    if (command->event_ == nullptr) {
      return;
    }
    EventHistoryMarkReset(command->event_);  // TSLOG v2: signal state zeroed before packet reset
    bool reset_now = EventHistoryConsumePending(command->event_);
    if (!reset_now && !EventHistoryHasPending(command->event_)) {
      // No pending clones tracked (eager command or untracked event) —
      // original immediate reset.
      reset_now = true;
    }
    if (reset_now) {
      if (BatchQktResetBatchActive()) {
        EventHistoryDeferGraphEventReset(command->context_, command->event_);
      }
      else {
        event_cache_.ResetEvent(command->event_);
      }
    }
    // else: clones of this event are still queued — they must read their
    // packet; the last one to be armed performs the reset.
  }

  // Emit pass for the deferred clones: consumes every deferred_ts_ command of
  // the lists this pass may touch, in list order, through the pre_ts tail.
  // Callers hold the submission lock (shared for a poll/fence/teardown sweep —
  // which then only touches its own thread-local list — and exclusive for the
  // drain backstop, which touches every list). `at_drain` only selects the
  // counter and whether the pending hint is trusted: the drain never trusts it
  // (it must reclaim clones no matter which thread deferred them), a poll
  // sweep does (it is the hot path and its list is usually the only one with
  // deferred clones).
  void BatchQktConsumeDeferredEmit(std::vector<uint64_t> *kids, bool at_drain) {
    if (at_drain) {
      uni_batchqkt_flush_pending_.store(false, std::memory_order_release);
    }
    else if (!uni_batchqkt_flush_pending_.exchange(false, std::memory_order_acq_rel)) {
      return;  // nothing armed, or another sweep won this flush
    }
    // T14/A5: the ~58ms/step record machinery this pass runs IS the emit debt
    // v4b hid inside poll sweeps -- measure it and self-admit it as a child
    // slice of whichever host API call this sweep runs inside.
    const bool meta = MetaOn();
    const uint64_t mt0 = meta ? UniTimer::GetHostTimestamp() : 0;
    const bool prev_guard = uni_batchqkt_in_batch_;
    uni_batchqkt_in_batch_ = true;  // the tails below must not re-enter a sweep
    uint64_t consumed = 0;
    auto consume_list = [&](ZeDeviceSubmissions &submissions) {
      auto it = submissions.commands_submitted_.begin();
      while (it != submissions.commands_submitted_.end()) {
        ZeCommand *command = *it;
        if (command == nullptr) {
          // HARDEN: a null entry can never be dereferenced -- drop it (it is
          // not a valid submitted command and would poison every later sweep).
          it = submissions.commands_submitted_.erase(it);
          continue;
        }
        if (command->deferred_ts_ == nullptr) {
          ++it;
          continue;
        }
        const ze_kernel_timestamp_result_t *pre_ts = command->deferred_ts_;
        command->deferred_ts_ = nullptr;
        if (kids != nullptr) {
          kids->push_back(command->instance_id_);
        }
        // Same tail the drain ran inline in v3/v4: emit from the held packet
        // (no query), same TSBAD semantics. deferred_emit=true skips the
        // event-release block — BatchQktReleaseArmedEvent already ran it at
        // the drain, and the event has been re-signaled by the next replay
        // since.
        ProcessCommandSubmittedTail(submissions, command, true, pre_ts, /*deferred_emit=*/true);
        submissions.commands_free_pool_.push_back(command);
        it = submissions.commands_submitted_.erase(it);
        consumed++;
      }
    };
    if (at_drain) {
      if (global_device_submissions_ != nullptr) {
        for (auto s : *global_device_submissions_) {
          consume_list(*s);
        }
      }
    }
    else {
      consume_list(local_device_submissions_);
    }
    uni_batchqkt_in_batch_ = prev_guard;
    if (consumed > 0) {
      if (at_drain) {
        uni_batchqkt_flush_at_drain_.fetch_add(consumed, std::memory_order_relaxed);
      }
      else {
        uni_batchqkt_flush_at_poll_.fetch_add(consumed, std::memory_order_relaxed);
      }
    }
    if (meta && consumed > 0) {
      // at="poll": the healthy overlapped emit (inside a device-busy sweep).
      // at="drain": the staging drain, or the teardown backstop (which has no
      // enclosing host API slice -- an expected, reported nesting exception).
      const uint64_t mt1 = UniTimer::GetHostTimestamp();
      MetaRecord("unitrace.emit", mt0, mt1,
                 "\"n\": " + std::to_string(consumed) +
                 ", \"us\": " + std::to_string(UniTimer::GetTimeInUs(mt1 - mt0)) +
                 ", \"at\": \"" + (at_drain ? "drain" : "poll") + "\"");
    }
  }

  void PrintBatchQktDiag(void) {
    if (!BatchQktDiagEnabled()) {
      return;
    }
    std::cerr << "[BATCHQKT] summary: enabled=" << uni_batchqkt_enabled_.load(std::memory_order_relaxed)
              << " batches=" << uni_batchqkt_batch_count_.load(std::memory_order_relaxed)
              << " cmds=" << uni_batchqkt_cmd_count_.load(std::memory_order_relaxed)
              << " fallback=" << uni_batchqkt_fallback_count_.load(std::memory_order_relaxed)
              << " max_n=" << uni_batchqkt_max_n_.load(std::memory_order_relaxed)
              << " max_batch_us=" << uni_batchqkt_max_batch_us_.load(std::memory_order_relaxed)
              << " sweeps=" << uni_batchqkt_sweeps_.load(std::memory_order_relaxed)
              << " seen=" << uni_batchqkt_seen_.load(std::memory_order_relaxed)
              << " collected=" << uni_batchqkt_collected_.load(std::memory_order_relaxed)
              // E6-v4 (appended at the end so existing greps keep matching):
              // device-side reset batches, events reset through them, largest
              // one, its append+sync cost, and events that fell back to the
              // legacy per-event host reset.
              << " reset_batches=" << uni_batchqkt_reset_batch_count_.load(std::memory_order_relaxed)
              << " reset_batched=" << uni_batchqkt_reset_batched_.load(std::memory_order_relaxed)
              << " reset_max=" << uni_batchqkt_reset_max_.load(std::memory_order_relaxed)
              << " reset_max_us=" << uni_batchqkt_reset_max_us_.load(std::memory_order_relaxed)
              << " reset_fallback=" << uni_batchqkt_reset_fallback_.load(std::memory_order_relaxed)
              // E6-v4b: where the emit of a drain-read batch ran (clones, not
              // flushes). poll = the overlapped path working as designed;
              // drain = the backstop (app skipped polling between steps).
              << " flush_at_poll=" << uni_batchqkt_flush_at_poll_.load(std::memory_order_relaxed)
              << " flush_at_drain=" << uni_batchqkt_flush_at_drain_.load(std::memory_order_relaxed)
              // E6-v4c (all zero unless UNITRACE_GRAPH_QKT_AT_POLL=1): where
              // the READ of a batch ran (clones), and the cost that moved off
              // the staging drain — TOTAL us across the run of the poll-armed
              // read's append+sync, plus the two halves separately. Healthy
              // v4c: qkt_at_poll == cmds, qkt_at_drain == 0, qkt_sync_us tiny
              // (the wait resolved during the step), qkt_append_us ~= the old
              // in-drain append cost, now spent inside device-busy sweeps.
              << " qkt_at_poll=" << uni_batchqkt_qkt_at_poll_.load(std::memory_order_relaxed)
              << " qkt_at_drain=" << uni_batchqkt_qkt_at_drain_.load(std::memory_order_relaxed)
              << " qkt_batch_us=" << uni_batchqkt_qkt_batch_us_.load(std::memory_order_relaxed)
              << " qkt_append_us=" << uni_batchqkt_qkt_append_us_.load(std::memory_order_relaxed)
              << " qkt_sync_us=" << uni_batchqkt_qkt_sync_us_.load(std::memory_order_relaxed)
              // HARDEN: graceful-skip accounting -- alloc/context-dead skips
              // and batches suppressed during graph capture windows.
              << " qkt_skip_alloc_fail=" << uni_batchqkt_alloc_fail_.load(std::memory_order_relaxed)
              << " dead_ctx_skips=" << uni_batchqkt_dead_ctx_skips_.load(std::memory_order_relaxed)
              << " ctx_releases=" << uni_batchqkt_ctx_releases_.load(std::memory_order_relaxed)
              << " capture_skip=" << uni_batchqkt_capture_skip_.load(std::memory_order_relaxed)
              << " capture_depth=" << uni_batchqkt_capture_depth_.load(std::memory_order_relaxed)
              << std::endl;
  }

  // -----------------------------------------------------------------------
  // E6-v4: batched reset of the graph clones' shared events. See the v4 note
  // in the T6'/E6 design comment above for the why; the mechanism:
  // EventHistoryDeferGraphEventReset parks the handle (only for graph clones,
  // only with the batch gate on), FlushGraphEventResets drains the parked set
  // at the end of EVERY sweep, and BatchQktResetEvents resets one context's
  // set as one device-side batch on the persistent immediate list.
  // -----------------------------------------------------------------------

  // A/B knob so a gate-on run can be split into "v3 resets" / "v4 resets"
  // without a rebuild. Absent (or anything but exactly "0") keeps the batch.
  static bool BatchQktResetBatchEnvEnabled(void) {
    static const bool enabled = []() {
      const char *e = std::getenv("UNITRACE_GRAPH_BATCH_QKT_RESET");
      return (e == nullptr || !(e[0] == '0' && e[1] == '\0'));
    }();
    return enabled;
  }

  // Master gate of the deferred reset path. Inherits the batch gate's TSLOG2
  // force-off so the diagnosis modes keep their exact inline ordering, and is
  // false whenever UNITRACE_GRAPH_BATCH_QKT is unset — the resets then stay on
  // the untouched inline host-reset path.
  inline bool BatchQktResetBatchActive(void) {
    return BatchQktActive() && BatchQktResetBatchEnvEnabled();
  }

  // Resets one context's parked events: batched when the immediate list for
  // that context exists, legacy host reset for anything that cannot ride it.
  // Called outside the submission locks (every sweep flushes after unlocking),
  // with uni_batchqkt_mutex_ held only around the shared-list use, like
  // BatchQktExecute.
  void BatchQktResetEvents(ze_context_handle_t context, const std::set<ze_event_handle_t> &events) {
    // Drop events the cache no longer owns: ReleaseGraphResources destroys and
    // recreates them, and appending a dead handle is worse than the no-op the
    // legacy host reset was for it.
    std::vector<ze_event_handle_t> live;
    live.reserve(events.size());
    for (ze_event_handle_t event : events) {
      if (event == nullptr) {
        continue;  // HARDEN: a null handle never reaches an append
      }
      if (event_cache_.QueryEvent(event)) {
        live.push_back(event);
      }
    }
    if (live.empty()) {
      return;
    }
    // HARDEN: no device-side reset batch while a graph capture is open --
    // the parked set is drained through the legacy host reset instead
    // (identical semantics, just not batched for this sweep).
    if (BatchQktCaptureHold()) {
      uni_batchqkt_capture_skip_.fetch_add(1, std::memory_order_relaxed);
      uni_batchqkt_reset_fallback_.fetch_add(live.size(), std::memory_order_relaxed);
      for (ze_event_handle_t event : live) {
        event_cache_.ResetEvent(event);
      }
      return;
    }
    ze_command_list_handle_t imm = nullptr;
    if (BatchQktResetBatchActive() && context != nullptr &&
        ZE_HAVE_FUNC(zeCommandListAppendEventReset) &&
        ZE_HAVE_FUNC(zeCommandListHostSynchronize)) {
      std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
      auto it = uni_batchqkt_imm_lists_.find(context);
      imm = (it != uni_batchqkt_imm_lists_.end()) ? it->second : nullptr;
      if (imm == nullptr) {
        // E6-v4c: with the read armed at a poll sweep the drain never runs
        // BatchQktExecute, so nothing ever created the regular imm list —
        // the reset batch then silently fell back to one zeEventHostReset
        // ioctl per event (node3: reset_batched=0, reset_fallback==cmds,
        // correct but the ~63ms/step serial shape v4 removed on the big
        // workload). The poll list can host the reset batch: a parked set
        // only exists once every armed read has been COMPLETED (its clones
        // were consumed to park these events), so no wait op is pending —
        // guard it anyway and take the legacy host reset if one somehow is.
        if (!uni_batchqkt_read_pending_.load(std::memory_order_acquire)) {
          auto pit = uni_batchqkt_poll_lists_.find(context);
          imm = (pit != uni_batchqkt_poll_lists_.end()) ? pit->second : nullptr;
        }
      }
      if (imm != nullptr) {
        const bool prev_guard = uni_batchqkt_in_batch_;
        uni_batchqkt_in_batch_ = true;  // our L0 calls must not re-enter the sweeps
        // T14/A5: the batched event reset of this sweep's clones.
        const bool meta = MetaOn();
        const uint64_t mt0 = meta ? UniTimer::GetHostTimestamp() : 0;
        auto t0 = std::chrono::steady_clock::now();
        ze_result_t status = ZE_RESULT_SUCCESS;
        for (ze_event_handle_t event : live) {
          status = ZE_FUNC(zeCommandListAppendEventReset)(imm, event);
          if (status != ZE_RESULT_SUCCESS) {
            break;
          }
        }
        if (status == ZE_RESULT_SUCCESS) {
          status = ZE_FUNC(zeCommandListHostSynchronize)(imm, UINT64_MAX);
        }
        auto t1 = std::chrono::steady_clock::now();
        const uint64_t mt1 = meta ? UniTimer::GetHostTimestamp() : 0;
        uni_batchqkt_in_batch_ = prev_guard;
        if (status == ZE_RESULT_SUCCESS) {
          const uint64_t n = static_cast<uint64_t>(live.size());
          uint64_t prev = uni_batchqkt_reset_max_.load(std::memory_order_relaxed);
          while (n > prev &&
                 !uni_batchqkt_reset_max_.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {
          }
          const uint64_t batch_us = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
          prev = uni_batchqkt_reset_max_us_.load(std::memory_order_relaxed);
          while (batch_us > prev &&
                 !uni_batchqkt_reset_max_us_.compare_exchange_weak(prev, batch_us, std::memory_order_relaxed)) {
          }
          uni_batchqkt_reset_batch_count_.fetch_add(1, std::memory_order_relaxed);
          uni_batchqkt_reset_batched_.fetch_add(n, std::memory_order_relaxed);
          if (meta) {
            // T14/A5: the sweep's batched reset, as one child slice.
            MetaRecord("unitrace.reset_batch", mt0, mt1,
                       "\"n\": " + std::to_string(n) +
                       ", \"us\": " + std::to_string(batch_us) + "");
          }
          if (!uni_batchqkt_notice_resetok_.exchange(true, std::memory_order_relaxed) &&
              BatchQktDiagEnabled()) {
            std::cerr << "[BATCHQKT] reset-batch=" << n
                      << " events on the immediate list, per-clone host resets are gone (E6-v4)"
                      << std::endl;
          }
          return;
        }
        // Append/synchronize failed: which of the appends executed is unknown,
        // so put every event back through the legacy host reset (resetting an
        // already-reset event is a no-op).
        if (!uni_batchqkt_notice_resetfail_.exchange(true, std::memory_order_relaxed) &&
            BatchQktDiagEnabled()) {
          std::cerr << "[BATCHQKT] reset-batch failed (status=0x" << std::hex << status << std::dec
                    << "), this sweep falls back to the per-event host reset" << std::endl;
        }
      }
    }
    uni_batchqkt_reset_fallback_.fetch_add(live.size(), std::memory_order_relaxed);
    for (ze_event_handle_t event : live) {
      event_cache_.ResetEvent(event);
    }
  }

  // HARDEN (2026-09-30 audit + pairing review): destroy side of every
  // collector allocation made from one context, called from the
  // zeContextDestroy ENTER intercept -- the context is still ALIVE here, so
  // every free below is legal and paired:
  //   zeCommandListCreateImmediate -> zeCommandListDestroy (imm + poll lists)
  //   zeMemAllocHost               -> zeMemFree (both staging buffers)
  // Without this, L0 heap-reuses the context handle value and the collector
  // keeps serving the DEAD list/buffer to the next engine's context -- the
  // first batched read then programs a list bound to a torn-down VM into the
  // CCS stream (GPU page fault at 0, the multi-boot wedge signature).
  // The handle value is marked dead afterwards: never trusted again even if
  // recycled (a reuse only costs the counted legacy-path fallback).
  // Locking: uni_batchqkt_mutex_ is the same lock every batched append/sync
  // holds, so an in-flight batch on another thread finishes (bounded by one
  // HostSynchronize) before the frees run.
  void ReleaseBatchQktContextResources(ze_context_handle_t context) {
    if (context == nullptr) {
      return;
    }
    {
      std::lock_guard<std::shared_mutex> elk(events_mutex_);
      // Parked (not yet reset) events of this context: drop them. Their
      // context is going away; resetting them through any list is the hazard
      // the v4 reset batching must never take.
      graph_events_pending_reset_.erase(context);
    }
    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    // Retire an armed poll read of this context: bounded sync while its list
    // is still alive, then discard the slot.
    if (uni_batchqkt_read_pending_.load(std::memory_order_acquire) &&
        uni_batchqkt_poll_read_.live && uni_batchqkt_poll_read_.context == context) {
      if (uni_batchqkt_poll_read_.list != nullptr) {
        ze_result_t status = ZE_FUNC(zeCommandListHostSynchronize)(uni_batchqkt_poll_read_.list, 1000000000ULL);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to synchronize armed poll read on context release (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
      }
      uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
      uni_batchqkt_read_pending_.store(false, std::memory_order_release);
    }
    size_t freed = 0, failed = 0;
    auto imm_it = uni_batchqkt_imm_lists_.find(context);
    if (imm_it != uni_batchqkt_imm_lists_.end() && imm_it->second != nullptr) {
      ze_result_t status = ZE_FUNC(zeCommandListDestroy)(imm_it->second);
      if (status == ZE_RESULT_SUCCESS) {
        freed++;
      } else {
        failed++;
        std::cerr << "[WARNING] Failed to destroy BATCHQKT immediate list (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
      }
    }
    auto poll_it = uni_batchqkt_poll_lists_.find(context);
    if (poll_it != uni_batchqkt_poll_lists_.end() && poll_it->second != nullptr) {
      ze_result_t status = ZE_FUNC(zeCommandListDestroy)(poll_it->second);
      if (status == ZE_RESULT_SUCCESS) {
        freed++;
      } else {
        failed++;
        std::cerr << "[WARNING] Failed to destroy BATCHQKT poll list (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
      }
    }
    auto buf_it = uni_batchqkt_bufs_.find(context);
    if (buf_it != uni_batchqkt_bufs_.end() && buf_it->second != nullptr) {
      ze_result_t status = ZE_FUNC(zeMemFree)(context, buf_it->second);
      if (status == ZE_RESULT_SUCCESS) {
        freed++;
      } else {
        failed++;
        std::cerr << "[WARNING] Failed to free BATCHQKT staging buffer (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
      }
    }
    auto poll_buf_it = uni_batchqkt_poll_bufs_.find(context);
    if (poll_buf_it != uni_batchqkt_poll_bufs_.end() && poll_buf_it->second != nullptr) {
      ze_result_t status = ZE_FUNC(zeMemFree)(context, poll_buf_it->second);
      if (status == ZE_RESULT_SUCCESS) {
        freed++;
      } else {
        failed++;
        std::cerr << "[WARNING] Failed to free BATCHQKT poll staging buffer (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
      }
    }
    uni_batchqkt_imm_lists_.erase(context);
    uni_batchqkt_poll_lists_.erase(context);
    uni_batchqkt_bufs_.erase(context);
    uni_batchqkt_buf_caps_.erase(context);
    uni_batchqkt_poll_bufs_.erase(context);
    uni_batchqkt_poll_buf_caps_.erase(context);
    // Remember the handle value as dead: never create/serve resources for it
    // again, even if L0 hands a NEW context out at the same address.
    uni_batchqkt_dead_contexts_.insert(context);
    uni_batchqkt_ctx_releases_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[BATCHQKT] context 0x" << std::hex << context << std::dec
              << " released: " << freed << " list/buffer resources freed";
    if (failed != 0) {
      std::cerr << ", " << failed << " FAILED (see warnings above)";
    }
    std::cerr << std::endl;
  }

  // HARDEN exit-path safety net, called from OnExitContextDestroy: at that
  // point the driver has ALREADY destroyed the context, so the frees must
  // NOT run here (use-after-destroy) -- if the enter intercept fired, every
  // map is empty and this only re-marks the handle; if it did not fire, this
  // still drops the dead context's resources from the maps so no sweep can
  // resurrect them, and the teardown drain below falls back to the legacy
  // per-event path instead of issuing device ops on the dying context.
  void InvalidateBatchQktContext(ze_context_handle_t context) {
    if (context == nullptr) {
      return;
    }
    {
      std::lock_guard<std::shared_mutex> elk(events_mutex_);
      graph_events_pending_reset_.erase(context);
    }
    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    // Discard an armed poll read slot of this context WITHOUT device calls:
    // its list may already be dead if the enter intercept did not run.
    if (uni_batchqkt_read_pending_.load(std::memory_order_acquire) &&
        uni_batchqkt_poll_read_.live && uni_batchqkt_poll_read_.context == context) {
      uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
      uni_batchqkt_read_pending_.store(false, std::memory_order_release);
    }
    uni_batchqkt_imm_lists_.erase(context);
    uni_batchqkt_poll_lists_.erase(context);
    uni_batchqkt_bufs_.erase(context);
    uni_batchqkt_buf_caps_.erase(context);
    uni_batchqkt_poll_bufs_.erase(context);
    uni_batchqkt_poll_buf_caps_.erase(context);
    uni_batchqkt_dead_contexts_.insert(context);
  }

  // Releases the collector-owned batch resources. Called at the end of
  // Finalize, after the tracer is gone and every submission is finalized, so
  // the L0 calls below cannot re-enter a live sweep. Normally a backstop:
  // per-context resources are already freed (paired) in
  // ReleaseBatchQktContextResources when the app destroyed each context.
  void ReleaseBatchQktResources(void) {
    std::lock_guard<std::mutex> lk(uni_batchqkt_mutex_);
    size_t freed = 0, failed = 0;
    // E6-v4c: drop a still-armed poll read first — its commands are back in
    // the free pool by now and its wait op (if any) must not outlive the poll
    // lists destroyed below. Best effort, bounded: the last step completed
    // long before teardown in the healthy case, so this returns immediately;
    // a bounded timeout keeps a wedged step from hanging the shutdown (the
    // descriptor is dropped either way).
    if (uni_batchqkt_poll_read_.live && uni_batchqkt_poll_read_.list != nullptr) {
      ze_result_t status = ZE_FUNC(zeCommandListHostSynchronize)(uni_batchqkt_poll_read_.list, 5000000000ULL);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[WARNING] Failed to synchronize armed poll read at teardown (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
      }
      uni_batchqkt_poll_read_ = ZeBatchQktPollRead();
    }
    uni_batchqkt_read_pending_.store(false, std::memory_order_release);
    for (auto &kv : uni_batchqkt_bufs_) {
      if (kv.second != nullptr) {
        ze_result_t status = ZE_FUNC(zeMemFree)(kv.first, kv.second);
        (status == ZE_RESULT_SUCCESS) ? freed++ : failed++;
      }
    }
    uni_batchqkt_bufs_.clear();
    uni_batchqkt_buf_caps_.clear();
    for (auto &kv : uni_batchqkt_poll_bufs_) {
      if (kv.second != nullptr) {
        ze_result_t status = ZE_FUNC(zeMemFree)(kv.first, kv.second);
        (status == ZE_RESULT_SUCCESS) ? freed++ : failed++;
      }
    }
    uni_batchqkt_poll_bufs_.clear();
    uni_batchqkt_poll_buf_caps_.clear();
    for (auto &kv : uni_batchqkt_imm_lists_) {
      if (kv.second != nullptr) {
        ze_result_t status = ZE_FUNC(zeCommandListDestroy)(kv.second);
        (status == ZE_RESULT_SUCCESS) ? freed++ : failed++;
      }
    }
    uni_batchqkt_imm_lists_.clear();
    for (auto &kv : uni_batchqkt_poll_lists_) {
      if (kv.second != nullptr) {
        ze_result_t status = ZE_FUNC(zeCommandListDestroy)(kv.second);
        (status == ZE_RESULT_SUCCESS) ? freed++ : failed++;
      }
    }
    uni_batchqkt_poll_lists_.clear();
    // Pairing audit line: nonzero on the healthy path only if the app never
    // destroyed its contexts; failures carry their own [WARNING] lines above.
    if (freed != 0 || failed != 0) {
      std::cerr << "[BATCHQKT] teardown freed " << freed << " list/buffer resources";
      if (failed != 0) {
        std::cerr << ", " << failed << " FAILED";
      }
      std::cerr << std::endl;
    }
  }

  inline void ProcessCommandSubmitted(ZeDeviceSubmissions& submissions, ZeCommand *command, std::vector<uint64_t> *kids, bool on_event, const ZePrefetchedTs *pre_ts = nullptr) {

    if (kids) {
        kids->push_back(command->instance_id_);
    }

    ProcessCommandSubmittedTail(submissions, command, on_event);
  }

  // Fix B: the completion work that follows the synchronous kids bookkeeping —
  // timestamp read (with TSBAD fallback), timeline/chrome emit, and the event
  // release/reset that gives the event back to the app. Runs inline in the
  // original path, and on the background completer thread when deferral
  // (UNITRACE_DEFERRED_TS=1) is active. Whoever runs it owns the command
  // object and deletes it afterwards.
  // T6'/E6: pre_ts carries an already batch-read packet for graph replay
  // clones (UNITRACE_GRAPH_BATCH_QKT=1) — the query step is skipped and every
  // later stage (TSBAD check, emit, pending-clone event reset) runs unchanged.
  // E6-v4b: deferred_emit marks a packet that was read at the staging drain
  // and whose event was already released there (BatchQktReleaseArmedEvent);
  // the event has since been reset and re-signaled by the next replay, so the
  // release block below must not run again — resetting it here would erase the
  // new generation's packet and permanently strand that clone at the next
  // drain's readiness gate.
  void ProcessCommandSubmittedTail(ZeDeviceSubmissions& submissions, ZeCommand *command, bool on_event,
                                   const ze_kernel_timestamp_result_t *pre_ts = nullptr,
                                   bool deferred_emit = false) {

    ze_kernel_timestamp_result_t timestamp;
    bool bad_read = false;
    if (!on_event) {
      if (command->device_global_timestamps_) {
        timestamp.global.kernelStart = command->device_global_timestamps_[0];
        timestamp.global.kernelEnd = command->device_global_timestamps_[1];
      }
      else {
        if (command->timestamps_on_event_reset_) {
          int slot = command->index_timestamps_on_commands_completion_->at(command->timestamp_seq_);
          if (slot == -1) {
            slot = command->index_timestamps_on_event_reset_->at(command->timestamp_seq_);
            ze_kernel_timestamp_result_t *ts = command->timestamps_on_event_reset_->at(slot / number_timestamps_per_slice_);
            timestamp = ts[slot % number_timestamps_per_slice_];
          }
          else {
            timestamp = (*(command->timestamps_on_commands_completion_))[slot];
          }
        }
        else {
          std::cerr << "[ERROR] Failed to get timestamps on device" << std::endl;
          ReleaseGraphCommandEvent(command);  // v3: account the clone before dropping it
          return;
        }
      }
      if (timestamp.global.kernelStart == timestamp.global.kernelEnd) {
        std::cerr << "[WARNING] Kernel starting timestamp and ending timestamp on the device are the same (" << timestamp.global.kernelStart << ")" << std::endl;
        if (command->event_ != nullptr) {
          ze_result_t status;
          TsSnapshotV2 ts_snap = SnapshotTsQueryV2(command);
          LogTsQueryV2(command, false);
          status = ZE_FUNC(zeEventQueryStatus)(command->event_);
          if (status == ZE_RESULT_SUCCESS) {
            std::cerr << "[WARNING] Trying to query event for timestamps" << std::endl;
            status = QueryLatestTimestamp(command->event_, &timestamp);
            LogTsAnomalyV2(command, false, timestamp, ts_snap);
            bad_read = TsReadIsBad(command, timestamp);
            if (status != ZE_RESULT_SUCCESS) {
              // do not panic
              std::cerr << "[WARNING] Unable to query event for timestamps" << std::endl;
            }
          }
        }
      }
    }
    else {
      if (command->event_ != nullptr) {
        TsSnapshotV2 ts_snap;
        ze_result_t status = ZE_RESULT_SUCCESS;
        if (pre_ts != nullptr) {
          // T6'/E6: the packet was batch-read for this replay after the shared
          // completion event was proven signaled (byte-identical to the
          // per-event query this replaces, T3 e2 micro verified). Snapshot /
          // anomaly logging stays on the query path only; TsReadIsBad below
          // keeps the exact legacy TSBAD semantics for the batched packet.
          timestamp = *pre_ts;
        }
        else {
          // Merge note: v3.1 prefetch pass-through dropped here — pre_ts is not
          // available on the Fix B tail path (prefetch was proven inert for
          // counter-based events in vLLM workloads).
          ts_snap = SnapshotTsQueryV2(command);
          status = QueryLatestTimestamp(command->event_, &timestamp);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Unable to query event for timestamps" << std::endl;
            ReleaseGraphCommandEvent(command);  // v3: account the clone before dropping it
            return;
          }
          LogTsAnomalyV2(command, true, timestamp, ts_snap);
        }
        bad_read = TsReadIsBad(command, timestamp);
        if (std::getenv("UNITRACE_DEBUG_TS") != nullptr) {
          std::cerr << "[TSLOG] on_event inst=" << command->instance_id_
                    << " ev=" << static_cast<const void*>(command->event_)
                    << " graph=" << command->graph_command_
                    << " raw_start=" << timestamp.global.kernelStart
                    << " raw_end=" << timestamp.global.kernelEnd
                    << " submit_host=" << command->submit_time_
                    << " submit_dev=" << command->submit_time_device_
                    << " freq=" << command->device_timer_frequency_
                    << " mask=" << std::hex << command->device_timer_mask_ << std::dec
                    << std::endl;
        }
      }
    }

    if (bad_read) {
      // Graph replay fix: never emit a timeline record from an unusable packet
      // read (erased start / torn / stale). The record for this instance is
      // dropped; [TSLOG2] (anomaly mode) carries the full state for diagnosis.
      std::cerr << "[TSBAD] dropped record inst=" << command->instance_id_
                << " ev=" << static_cast<const void*>(command->event_)
                << " graph=" << command->graph_command_
                << " submit_dev=" << command->submit_time_device_
                << " raw_start=" << timestamp.global.kernelStart
                << " raw_end=" << timestamp.global.kernelEnd
                << std::endl;
    }
    else {
    ZeKernelProfileRecord r;

    if (options_.metric_query || options_.metric_stream) {
      r.device_ = command->device_;
      r.instance_id_ = command->instance_id_;
      r.kernel_command_id_ = command->kernel_command_id_;
      r.group_count_ = command->group_count_;
      r.mem_size_ = command->mem_size_;
    }

    if (options_.kernels_per_tile && (command->type_ == KERNEL_COMMAND_TYPE_COMPUTE)) {
      if (command->implicit_scaling_) { // Implicit Scaling
        uint32_t count = 0;
        ze_result_t status = ZE_FUNC(zeEventQueryTimestampsExp)(command->event_, command->device_, &count, nullptr);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        PTI_ASSERT(count > 0);

        std::vector<ze_kernel_timestamp_result_t> timestamps(count);
        status = ZE_FUNC(zeEventQueryTimestampsExp)(command->event_, command->device_, &count, timestamps.data());
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);

        if (options_.metric_query || options_.metric_stream) {
          for (uint32_t i = 0; i < count; i++) {
            ZeKernelProfileTimestamps ts;

            ts.subdevice_id = i;

            ts.metric_start = timestamps[i].global.kernelStart;
            ts.metric_end = timestamps[i].global.kernelEnd;

            r.timestamps_.push_back(std::move(ts));
          }

          submissions.kernel_profiles_.insert({command->instance_id_, std::move(r)});
        }

        if (count == 1) { // First tile is used only
          LogCommandCompleted(command, timestamps[0], 0);
        } else {
          for (uint32_t i = 0; i < count; ++i) {
            LogCommandCompleted(command, timestamps[i], static_cast<int>(i));
          }
        }
      } else { // Explicit Scaling
        auto it = devices_->find(command->device_);
        if (it != devices_->end()) {
          LogCommandCompleted(command, timestamp, -1);
        }

        if (options_.metric_query || options_.metric_stream) {
          ZeKernelProfileTimestamps ts;

          ts.metric_start = timestamp.global.kernelStart;
          ts.metric_end = timestamp.global.kernelEnd;

          ts.subdevice_id = -1;

          r.timestamps_.push_back(std::move(ts));

          submissions.kernel_profiles_.insert({command->instance_id_, std::move(r)});
        }
      }
    } else {
      if (options_.metric_query || options_.metric_stream) {
        ZeKernelProfileTimestamps ts;

        ts.metric_start = timestamp.global.kernelStart;
        ts.metric_end = timestamp.global.kernelEnd;

        ts.subdevice_id = -1;
        r.timestamps_.push_back(std::move(ts));

        submissions.kernel_profiles_.insert({command->instance_id_, std::move(r)});
      }

      LogCommandCompleted(command, timestamp, -1);
    }
    }  // end of !bad_read emission (Graph replay fix)

    // Graph commands are reused across executions.
    if (command->immediate_ && !command->graph_command_) {
      EventHistoryErase(command->event_);  // TSLOG v2: handle will be destroyed/recreated
      event_cache_.ReleaseEvent(command->event_);
    } else if (!deferred_emit) {  // E6-v4b: event already released at arm time
      EventHistoryMarkReset(command->event_);  // TSLOG v2: signal state zeroed before packet reset
      // Graph replay fix v3: reset the shared event as soon as the last pending
      // clone has read it. All clones consumed within this sweep see the same
      // packet (last-kernel attribution); resetting later (v2: end of sweep)
      // left the event latched across replays and the app's next host-wait
      // returned early on the previous generation's packet.
      bool reset_now = EventHistoryConsumePending(command->event_);
      if (!reset_now && !EventHistoryHasPending(command->event_)) {
        // No pending clones tracked (eager command or untracked event) —
        // original immediate reset.
        reset_now = true;
      }
      if (reset_now) {
        // E6-v4: a graph clone whose event is due for reset, with the batch
        // gate on, hands the reset to the sweep-end batch
        // (EventHistoryDeferGraphEventReset -> FlushGraphEventResets) instead
        // of paying one zeEventHostReset ioctl here — at the staging drain
        // those ~770 ioctls were ~63ms of fully-serial driver time on the
        // app's critical path. Deferral still happens inside the sweep, so the
        // event is un-signaled before this L0 call returns and before the
        // clone staging loop re-signals it; the gate off keeps the inline
        // reset below byte-identical.
        if (command->graph_command_ && BatchQktResetBatchActive()) {
          EventHistoryDeferGraphEventReset(command->context_, command->event_);
        }
        else {
          event_cache_.ResetEvent(command->event_);
        }
      }
      // else: clones of this event are still queued — they must read this
      // packet; the last one to be processed performs the reset.
    }
    command->event_ = nullptr;
    command->in_order_counter_event_ = nullptr;
  }

  void CreateCommandList( ze_command_list_handle_t command_list,
    ze_context_handle_t context,
    ze_device_handle_t device,
    uint32_t ordinal,
    uint32_t index,
    bool immediate,
    bool in_order) {

    ZeCommandList *desc;

    command_lists_mutex_.lock();
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      desc = it->second;
      command_lists_.erase(it);
    }
    else {
      desc = new ZeCommandList;
      UniMemory::ExitIfOutOfMemory((void *)(desc));
    }

    desc->num_timestamps_ = 0;
    desc->num_timestamps_on_event_reset_ = 0;
    desc->timestamps_on_commands_completion_ = nullptr;
    desc->timestamps_on_event_reset_.clear();
    desc->event_to_timestamp_seq_.clear();
    desc->index_timestamps_on_commands_completion_.clear();
    desc->index_timestamps_on_event_reset_.clear();
    desc->num_device_global_timestamps_ = 0;
    desc->device_global_timestamps_.clear();

    command_lists_mutex_.unlock();

    desc->cmdlist_ = command_list;
    desc->context_ = context;
    desc->device_ = device;
    desc->immediate_ = immediate;
    desc->in_order_ = in_order;
    desc->engine_ordinal_ = ordinal;  // valid if immediate command list
    desc->engine_index_ = index;;  // valid if immediate command list

    if (immediate == false) {
      desc->timestamp_event_to_signal_ = event_cache_.GetEvent(context);
      if (desc->timestamp_event_to_signal_ == nullptr) {
        // HARDEN: event pool creation failed (reported by the cache); leave
        // the event null (immediate command lists run without one) and keep
        // the app alive instead of signalling a null handle.
        std::cerr << "[ERROR] No timestamp event for command list, kernels on it will not be instrumented" << std::endl;
      } else {
      // set to signal state to unblock first ZE_FUNC(zeCommandQueueExecuteCommandLists)() call
      auto status = ZE_FUNC(zeEventHostSignal)(desc->timestamp_event_to_signal_);
      if (status != ZE_RESULT_SUCCESS) {
        std::cerr << "[ERROR] Failed to signal timestamp event in command list" << std::endl;
        exit(-1);
      }
      }
    }
    else {
      desc->timestamp_event_to_signal_ = nullptr;
    }

    devices_mutex_.lock_shared();
    auto it2 = devices_->find(device);
    if (it2 != devices_->end()) {
      desc->host_time_origin_ = it2->second.host_time_origin_;
      desc->device_timer_frequency_ = it2->second.device_timer_frequency_;
      desc->device_timer_mask_ = it2->second.device_timer_mask_;
      desc->device_ns_per_cycle_ = it2->second.device_ns_per_cycle_;
      desc->implicit_scaling_ = (it2->second.num_subdevices_ != 0);
    }
    devices_mutex_.unlock_shared();

    command_lists_mutex_.lock();
    command_lists_.insert({command_list, desc});
    command_lists_mutex_.unlock();
  }

  void DestroyCommandList(ze_command_list_handle_t command_list) {

    // Fix B barrier: this path releases the list's events (and frees its
    // timestamp buffers); queued completer items must be finished first.
    FlushDeferredTimestamps();

    command_lists_mutex_.lock();

    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      if (!it->second->immediate_) {
        if (it->second->timestamp_event_to_signal_) {
          auto status = ZE_FUNC(zeEventHostSynchronize)(it->second->timestamp_event_to_signal_, UINT64_MAX);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Timestamp event is not signaled" << std::endl;
            command_lists_mutex_.unlock();
            return;
          }
          ProcessAllCommandsSubmitted(nullptr);  // make sure commands submitted already are processed
        }
        for (auto& command : it->second->commands_) {
          if (command->event_) {
            event_cache_.ReleaseEvent(command->event_);
          }
        }
        it->second->commands_.clear();
        it->second->event_to_timestamp_seq_.clear();
        ze_result_t status;
        for (auto ts : it->second->timestamps_on_event_reset_) {
          if (ts != nullptr) {
            status = ZE_FUNC(zeMemFree)(it->second->context_, ts);
            if (status != ZE_RESULT_SUCCESS) {
              std::cerr << "[WARNING] Failed to free event timestamp memory (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
            }
          }
        }
        it->second->timestamps_on_event_reset_.clear();
        for (auto ts : it->second->device_global_timestamps_) {
          if (ts != nullptr) {
            status = ZE_FUNC(zeMemFree)(it->second->context_, ts);
            if (status != ZE_RESULT_SUCCESS) {
              std::cerr << "[WARNING] Failed to free global timestamp memory (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
            }
          }
        }
        it->second->device_global_timestamps_.clear();
        if (it->second->timestamps_on_commands_completion_ != nullptr) {
          status = ZE_FUNC(zeMemFree)(it->second->context_, it->second->timestamps_on_commands_completion_);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[WARNING] Failed to free command timestamp memory (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          }
          it->second->timestamps_on_commands_completion_ = nullptr;
        }
        it->second->index_timestamps_on_commands_completion_.clear();
        it->second->index_timestamps_on_event_reset_.clear();
        event_cache_.ReleaseEvent(it->second->timestamp_event_to_signal_);
        it->second->timestamp_event_to_signal_ = nullptr;
      }
      // Clean up pending graph capture to prevent memory leak
      if (it->second->pending_graph_capture_ != nullptr) {
        ReleaseGraphResources(*it->second->pending_graph_capture_);
        delete it->second->pending_graph_capture_;
        it->second->pending_graph_capture_ = nullptr;
      }
      if (it->second->graph_capturing_) {  // HARDEN: list dies mid-capture
        it->second->graph_capturing_ = false;
        uni_batchqkt_capture_depth_.fetch_sub(1, std::memory_order_acq_rel);
      }
      command_lists_.erase(it);
    }

    command_lists_mutex_.unlock();
  }

  void ResetCommandList(ze_command_list_handle_t command_list) {

    // Fix B barrier: this path releases the list's events (and frees its
    // timestamp buffers); queued completer items must be finished first.
    FlushDeferredTimestamps();

    command_lists_mutex_.lock();

    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      if (!it->second->immediate_) {
        if (it->second->timestamp_event_to_signal_) {
          auto status = ZE_FUNC(zeEventHostSynchronize)(it->second->timestamp_event_to_signal_, UINT64_MAX);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Timestamp event is not signaled" << std::endl;
            command_lists_mutex_.unlock();
            return;
          }
          ProcessAllCommandsSubmitted(nullptr);  // make sure commands submitted already are processed
        }
        for (auto& command : it->second->commands_) {
          if (command->event_) {
            event_cache_.ReleaseEvent(command->event_);
          }
        }
        it->second->commands_.clear();
        it->second->event_to_timestamp_seq_.clear();
        it->second->num_timestamps_ = 0;
        it->second->num_timestamps_on_event_reset_ = 0;
        it->second->index_timestamps_on_commands_completion_.clear();
        it->second->index_timestamps_on_event_reset_.clear();
        if (it->second->timestamps_on_commands_completion_ != nullptr) {
          ze_result_t status;
          status = ZE_FUNC(zeMemFree)(it->second->context_, it->second->timestamps_on_commands_completion_);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[WARNING] Failed to free command timestamp memory (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          }
          it->second->timestamps_on_commands_completion_ = nullptr;
        }
        it->second->device_global_timestamps_.clear();
        it->second->num_device_global_timestamps_ = 0;
      }
    }

    command_lists_mutex_.unlock();
  }

  void PrepareToExecuteCommandLists(
    ze_command_list_handle_t *cmdlists, uint32_t count, ze_command_queue_handle_t queue, ze_fence_handle_t fence) {

    command_queues_mutex_.lock_shared();
    auto qit = command_queues_.find(queue);
    if (qit != command_queues_.end()) {
      command_lists_mutex_.lock_shared();
      PrepareToExecuteCommandListsLocked(cmdlists, count, qit->second.device_, qit->second.engine_ordinal_, qit->second.engine_index_, fence);
      command_lists_mutex_.unlock_shared();
    }
    command_queues_mutex_.unlock_shared();
  }

  void PrepareToExecuteCommandListsLocked(
    ze_command_list_handle_t *cmdlists, uint32_t count, ze_device_handle_t device,
    uint32_t engine_ordinal, uint32_t engine_index, ze_fence_handle_t fence) {

    for (uint32_t i = 0; i < count; i++) {
      ze_command_list_handle_t cmdlist = cmdlists[i];

      auto it = command_lists_.find(cmdlist);

      if (it == command_lists_.end()) {
        std::cerr << "[ERROR] Command list (" << cmdlist << ") is not found to execute." << std::endl;
        continue;
      }

      if (!it->second->immediate_) {
        if (it->second->timestamp_event_to_signal_) {
          auto status = ZE_FUNC(zeEventHostSynchronize)(it->second->timestamp_event_to_signal_, UINT64_MAX);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Timestamp event is not signaled" << std::endl;
            return;
          }
          ProcessAllCommandsSubmitted(nullptr);  // make sure commands submitted last time are processed
          if (ZE_FUNC(zeEventHostReset)(it->second->timestamp_event_to_signal_) != ZE_RESULT_SUCCESS) {    // reset event
            std::cerr << "[ERROR] Failed to reset timestamp event" << std::endl;
            return;
          }
        }
      }
    }

    uint64_t host_timestamp;
    uint64_t device_timestamp;
    ze_result_t status;

    status = ZE_FUNC(zeDeviceGetGlobalTimestamps)(device, &host_timestamp, &device_timestamp);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    for (uint32_t i = 0; i < count; i++) {
      ze_command_list_handle_t cmdlist = cmdlists[i];
      auto it = command_lists_.find(cmdlist);
      if (it == command_lists_.end()) {
        std::cerr << "[ERROR] Command list (" << cmdlist << ") is not found to execute." << std::endl;
        continue;
      }

      if (!it->second->immediate_) {
        for (auto command : it->second->commands_) {
          ZeCommand *cmd = nullptr;
          ZeCommandMetricQuery *cmd_query = nullptr;

          cmd = local_device_submissions_.GetKernelCommand();

          if (command->command_metric_query_ != nullptr) {
            cmd_query = local_device_submissions_.GetCommandMetricQuery();
          }
          *cmd = *command;

          cmd->engine_ordinal_ = engine_ordinal;
          cmd->engine_index_ = engine_index;
          cmd->submit_time_ = host_timestamp;    //in ns
          cmd->submit_time_device_ = device_timestamp;  //in ticks
          cmd->tid_ = utils::GetTid();;
          cmd->fence_ = fence;
          // Exit callback will reset cmd->event_ and backfill cmd->instance_id_
          local_device_submissions_.StageKernelCommand(cmd);

          if (cmd_query) {
            *cmd_query = *(command->command_metric_query_);
            // Exit callback will reset cmd_query->metric_query_event_ and backfill cmd_query->instance_id_
            local_device_submissions_.StageCommandMetricQuery(cmd_query);
          }
          else {
            local_device_submissions_.StageCommandMetricQuery(nullptr);
          }
        }
      }
    }
  }

  void CreateImage(ze_image_handle_t image, size_t size) {
    images_mutex_.lock();
    if (images_.find(image) != images_.end()) {
      images_.erase(image);
    }
    images_.insert({image, size});
    images_mutex_.unlock();
  }

  void DestroyImage(ze_image_handle_t image) {
    images_mutex_.lock();
    images_.erase(image);
    images_mutex_.unlock();
  }

  size_t GetImageSize(ze_image_handle_t image) {
    size_t size;

    images_mutex_.lock_shared();
    auto it = images_.find(image);
    if (it != images_.end()) {
      size = it->second;
    }
    else {
      size = 0;
    }
    images_mutex_.unlock_shared();

    return size;
  }

 private: // Callbacks

  #include <tracing.gen.types> // Auto-generated types for callbacks

  static void OnEnterEventPoolCreate(ze_event_pool_create_params_t *params, void * /* global_data */, void **instance_data) {
    const ze_event_pool_desc_t* desc = *(params->pdesc);
    if (desc == nullptr) {
      return;
    }
    if (desc->flags & ZE_EVENT_POOL_FLAG_IPC) {
      return;
    }

    // Do not override flags if counter based pool
    const void *pNext = desc->pNext;
    while (pNext) {
      if (((ze_base_cb_params_t *)pNext)->stype == ZE_STRUCTURE_TYPE_COUNTER_BASED_EVENT_POOL_EXP_DESC) {
        return;
      }
      pNext = ((ze_base_cb_params_t *)pNext)->pNext;
    }

    ze_event_pool_desc_t* profiling_desc = new ze_event_pool_desc_t;
    UniMemory::ExitIfOutOfMemory((void *)(profiling_desc));
    PTI_ASSERT(profiling_desc != nullptr);
    profiling_desc->stype = desc->stype;
    profiling_desc->pNext = desc->pNext;
    profiling_desc->flags = desc->flags;
    profiling_desc->flags |= ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;
    profiling_desc->flags |= ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
    profiling_desc->count = desc->count;

    *(params->pdesc) = profiling_desc;
    *instance_data = profiling_desc;
  }

  static void OnExitEventPoolCreate(ze_event_pool_create_params_t *params,
                                    ze_result_t result,
                                    void *global_data,
                                    void **instance_data) {

    if (result == ZE_RESULT_SUCCESS && params->pphEventPool && *params->pphEventPool) {
      const ze_event_pool_desc_t* desc = *(params->pdesc);
      const void *pNext = desc ? desc->pNext : nullptr;
      while (pNext) {
        if (((ze_base_cb_params_t *)pNext)->stype == ZE_STRUCTURE_TYPE_COUNTER_BASED_EVENT_POOL_EXP_DESC) {
          ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
          collector->events_mutex_.lock();
          collector->counter_based_pools_.insert(**params->pphEventPool);
          collector->events_mutex_.unlock();
          break;
        }
        pNext = ((ze_base_cb_params_t *)pNext)->pNext;
      }
    }

    ze_event_pool_desc_t* desc = static_cast<ze_event_pool_desc_t*>(*instance_data);
    if (desc != nullptr) {
      delete desc;
    }
  }

  static void OnExitEventPoolDestroy(ze_event_pool_destroy_params_t *params,
                                ze_result_t result,
                                void* global_data,
                                void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->events_mutex_.lock();
      collector->counter_based_pools_.erase(*params->phEventPool);
      collector->events_mutex_.unlock();
    }
  }

  static void OnExitEventCreate(ze_event_create_params_t* params,
                                ze_result_t result,
                                void* global_data,
                                void** /* instance_data */) {
    if (uni_defer_on_completer_thread_) {
      return;  // Fix B: replacement events created by the completer are not app events
    }
    if (result == ZE_RESULT_SUCCESS && params->pphEvent && *params->pphEvent) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->events_mutex_.lock();
      if (collector->counter_based_pools_.find(*params->phEventPool) != collector->counter_based_pools_.end()) {
        collector->counter_based_events_.insert(**params->pphEvent);
      }
      collector->events_mutex_.unlock();
    }
  }

  static void OnExitCounterBasedEventCreate2(
      ze_x_counter_based_event_create2_params_t *params,
      ze_result_t result,
      void* global_data,
      void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS && params->pphEvent && *params->pphEvent) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->events_mutex_.lock();
      collector->counter_based_events_.insert(**params->pphEvent);
      collector->events_mutex_.unlock();
    }
  }

  static void OnEnterEventDestroy(
      ze_event_destroy_params_t *params,
      void *global_data, void ** /* instance_data */, std::vector<uint64_t> *kids) {

    // Fix B: the completer thread's own L0 calls (zeEventHostReset /
    // zeEventDestroy inside the event cache, issued under the cache lock) come
    // back through the tracing layer; re-entering the collector here would
    // deadlock on the locks the caller already holds. Same guard on the other
    // event-related callbacks below.
    if (uni_defer_on_completer_thread_) {
      return;
    }
    if (*(params->phEvent) != nullptr) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      // Fix B barrier: the app is about to destroy this event; the completer
      // thread must have finished every queued command that queries it.
      collector->FlushDeferredTimestamps();
      if (ZE_FUNC(zeEventQueryStatus)(*(params->phEvent)) == ZE_RESULT_SUCCESS) {
        collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
      }
      else {
        // Event is not signaled on destroy, removing associated commands
        global_device_submissions_mutex_.lock_shared();
        // discard event associated staged commands
        local_device_submissions_.RevertStagedKernelCommandAndMetricQueriesForEvent(*(params->phEvent));
        // discard event associated submitted commands
        auto it_cmd = local_device_submissions_.commands_submitted_.begin();
        while (it_cmd != local_device_submissions_.commands_submitted_.end()) {
          ZeCommand *command = *it_cmd;
          if (command->event_ == *(params->phEvent)) {
            std::cerr << "[INFO] Remove command from submitted commands on event destroy" << std::endl;
            local_device_submissions_.commands_free_pool_.push_back(command);
            it_cmd = local_device_submissions_.commands_submitted_.erase(it_cmd);
            continue;
          }
          ++it_cmd;
        }
        global_device_submissions_mutex_.unlock_shared();
        // Non immediate command list
        collector->command_lists_mutex_.lock();
        auto it = collector->command_lists_.begin();
        while (it != collector->command_lists_.end()) {
          auto cmd_it = it->second->commands_.begin();
          while (cmd_it != it->second->commands_.end()) {
            ZeCommand *command = *cmd_it;
            if (command->event_ == *(params->phEvent)) {
              std::cerr << "[INFO] Remove command from command list on event destroy" << std::endl;
              cmd_it = it->second->commands_.erase(cmd_it);
              continue;
            }
            ++cmd_it;
          }
          ++it;
        }
        collector->command_lists_mutex_.unlock();
      }
    }
  }

  static void OnExitEventDestroy(
      ze_event_destroy_params_t *params, ze_result_t result,
      void *global_data, void ** /* instance_data */) {
    if (uni_defer_on_completer_thread_) {
      return;  // Fix B: no collector re-entry from the completer's own L0 calls
    }
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->events_mutex_.lock();
      collector->counter_based_events_.erase(*params->phEvent);
      collector->event_history_.erase(*params->phEvent);  // TSLOG v2: drop history of destroyed event
      collector->events_mutex_.unlock();
    }
  }

  static void OnEnterEventHostReset(
      ze_event_host_reset_params_t *params,
      void *global_data, void ** /* instance_data */, std::vector<uint64_t> *kids) {
    if (*(params->phEvent) != nullptr) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      if (uni_defer_on_completer_thread_) {
        return;  // Fix B: this host reset was issued by the completer itself
      }
      // Fix B barrier: this reset zeroes the kernel-timestamp packet; queued
      // completer items must have read it before that.
      collector->FlushDeferredTimestamps();
      if (ZE_FUNC(zeEventQueryStatus)(*(params->phEvent)) == ZE_RESULT_SUCCESS) {
        collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
      }
      collector->EventHistoryMarkReset(*(params->phEvent));  // TSLOG v2: app host reset
    }
  }

  static void OnExitEventHostSynchronize(
      ze_event_host_synchronize_params_t *params,
      ze_result_t result, void *global_data, void ** /* instance_data */, std::vector<uint64_t> *kids) {
    if (uni_defer_on_completer_thread_) {
      return;  // Fix B: no collector re-entry from the completer's own L0 calls
    }
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
    }
  }

  static void OnExitCommandListHostSynchronize(
      ze_command_list_host_synchronize_params_t * /* params */,
      ze_result_t result, void *global_data, void ** /* instance_data */, std::vector<uint64_t> *kids) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessAllCommandsSubmitted(kids);
    }
  }

  static void OnExitEventQueryStatus(
      ze_event_query_status_params_t *params,
      ze_result_t result, void *global_data, void ** /* instance_data */, std::vector<uint64_t> *kids) {
    if (uni_defer_on_completer_thread_) {
      return;  // Fix B: no collector re-entry from the completer's own L0 calls
    }
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmittedOnSignaledEvent(*(params->phEvent), kids);
    }
  }

  static void OnExitFenceHostSynchronize(
      ze_fence_host_synchronize_params_t *params,
      ze_result_t result, void *global_data, void ** /* instance_data */, std::vector<uint64_t> *kids) {
    if (uni_defer_on_completer_thread_) {
      return;  // Fix B: no collector re-entry from the completer's own L0 calls
    }
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*(params->phFence) != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmittedOnFenceSynchronization(*(params->phFence), kids);
    }
  }

  static void OnExitImageCreate(
      ze_image_create_params_t *params, ze_result_t result,
      void *global_data, void ** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      ze_image_desc_t image_desc = **(params->pdesc);
      size_t image_size = image_desc.width;
      switch (image_desc.type) {
        case ZE_IMAGE_TYPE_2D:
        case ZE_IMAGE_TYPE_2DARRAY:
          image_size *= image_desc.height;
          break;
        case ZE_IMAGE_TYPE_3D:
          image_size *= image_desc.height * image_desc.depth;
          break;
        default:
          break;
      }

      switch (image_desc.format.type) {
        case ZE_IMAGE_FORMAT_TYPE_UINT:
        case ZE_IMAGE_FORMAT_TYPE_UNORM:
        case ZE_IMAGE_FORMAT_TYPE_FORCE_UINT32:
          image_size *= sizeof(unsigned int);
          break;
        case ZE_IMAGE_FORMAT_TYPE_SINT:
        case ZE_IMAGE_FORMAT_TYPE_SNORM:
          image_size *= sizeof(int);
          break;
        case ZE_IMAGE_FORMAT_TYPE_FLOAT:
          image_size *= sizeof(float);
          break;
      }

      collector->CreateImage(**(params->pphImage), image_size);
    }
  }

  static void OnExitImageDestroy(
      ze_image_destroy_params_t *params, ze_result_t result,
      void *global_data, void ** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->DestroyImage(*(params->phImage));
    }
  }

  static void PrepareToAppendKernelCommand(
    ZeCollector* collector,
    ze_event_handle_t& signal_event,
    ze_command_list_handle_t command_list,
    bool iskernel,
    ze_kernel_handle_t kernel = nullptr,
    uint32_t num_wait_events = 0,
    ze_event_handle_t* wait_events = nullptr) {

    // TAX-OPT: enter-side collector work of the eager kernel append path.
    UniPhaseTimer ph_prepk(collector->uni_ph_prepk_us_, collector->uni_ph_prepk_n_);

    ze_context_handle_t context = nullptr;
    ze_device_handle_t device = nullptr;
    bool in_order = false;

    ze_instance_data.query_ = nullptr;
    ze_instance_data.in_order_counter_event_ = nullptr;
    ze_instance_data.signal_gen_ = 0;
    ze_instance_data.instrument_ = true;

    if (iskernel) {
      if (kernel == nullptr) {
        std::cerr << "[ERROR] Kernel handle is null for kernel command." << std::endl;
        ze_instance_data.instrument_ = false;
        return;
      }
      kernel_command_properties_mutex_.lock_shared();
      auto kit = active_kernel_properties_->find(kernel);
      if (kit != active_kernel_properties_->end()) {
          if (kit->second.skip_) {
            ze_instance_data.instrument_ = false;
            kernel_command_properties_mutex_.unlock_shared();
            return;
          }
      }
      kernel_command_properties_mutex_.unlock_shared();
    }

    collector->command_lists_mutex_.lock_shared();

    auto it = collector->command_lists_.find(command_list);
    if (it != collector->command_lists_.end()) {
      context = it->second->context_;
      device = it->second->device_;
      in_order = it->second->in_order_;
    }

    collector->command_lists_mutex_.unlock_shared();

    if ((context == nullptr) || (device == nullptr)) {
      // should never get here
      std::cerr << "[ERROR] Command list (" << command_list << ") is not found for appending." << std::endl;
      ze_instance_data.instrument_ = true;
      return;
    }

    // Check if this append triggers a fork transition
    // (i.e., this cmdlist waits on an event from an active graph capture)
    if (num_wait_events > 0 && wait_events != nullptr) {
      collector->CheckAndApplyForkTransition(command_list, num_wait_events, wait_events);
    }

    if (signal_event == nullptr) {
      signal_event = collector->event_cache_.GetEvent(context);
      if (signal_event == nullptr) {
        // HARDEN: pool creation failed (reported by the cache) -- skip the
        // instrumentation of this append instead of asserting the process.
        std::cerr << "[ERROR] No event for kernel instrumentation, command will not be instrumented" << std::endl;
        ze_instance_data.instrument_ = false;
        return;
      }
    } else {
      collector->events_mutex_.lock();
      if (collector->counter_based_events_.find(signal_event) != collector->counter_based_events_.end()) {
        if (in_order) {
          ze_instance_data.in_order_counter_event_ = signal_event;
          signal_event = collector->event_cache_.GetEvent(context);
        } else {
          // This is an error that should never happen since counter based events can be
          // used only in in-order command lists.
          std::cerr << "[ERROR] Counter based events are used in non immediate command list - command will not be instrumented" << std::endl;
          ze_instance_data.instrument_ = false;
          collector->events_mutex_.unlock();
          return;
        }
      }
      collector->events_mutex_.unlock();
    }

    // TSLOG v2: register the signal this command will produce on signal_event.
    // Covers collector-owned events and app-owned signal events (e.g. events
    // the app passes during graph capture).
    ze_instance_data.signal_gen_ = collector->EventHistoryBumpSignal(signal_event, /*path=*/0, 0);

    ze_result_t status;
    if (collector->options_.metric_query && iskernel) {
      devices_mutex_.lock_shared();

      auto it2 = devices_->find(device);

      PTI_ASSERT(it2 != devices_->end());
      ze_instance_data.query_ = collector->query_pools_.GetQuery(context, device, it2->second.metric_group_);

      devices_mutex_.unlock_shared();

      status = ZE_FUNC(zetCommandListAppendMetricQueryBegin)(command_list, ze_instance_data.query_);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    }

    uint64_t host_timestamp;
    uint64_t device_timestamp;  // in ticks

    status = ZE_FUNC(zeDeviceGetGlobalTimestamps)(device, &host_timestamp, &device_timestamp);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    ze_instance_data.timestamp_host = host_timestamp;
    ze_instance_data.timestamp_device = device_timestamp;
  }

  static void PrepareToAppendKernelCommand(ZeCommandList *cl) {
    ze_result_t status;
    uint64_t host_timestamp;
    uint64_t device_timestamp;  // in ticks

    status = ZE_FUNC(zeDeviceGetGlobalTimestamps)(cl->device_, &host_timestamp, &device_timestamp);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    ze_instance_data.timestamp_host = host_timestamp;
    ze_instance_data.timestamp_device = device_timestamp;
  }

  void AppendLaunchKernel(
    ze_kernel_handle_t kernel,
    const ze_group_count_t *group_count,
    ze_event_handle_t& event_to_signal,
    zet_metric_query_handle_t& query,
    ze_command_list_handle_t command_list,
    std::vector<uint64_t> *kids) {

    // TAX-OPT: exit-side collector work of the eager kernel append path.
    UniPhaseTimer ph_appendk(uni_ph_appendk_us_, uni_ph_appendk_n_);

    uint64_t kernel_id;
    ZeKernelGroupSize group_size;
    bool found = false;

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_kernel_properties_->find(kernel);
    if (kit != active_kernel_properties_->end()) {
      kernel_id = kit->second.id_;
      group_size = kit->second.group_size_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (!found) {
      std::cerr << "[ERROR] Kernel (" << kernel << ") is not found." << std::endl;
      return;
    }

    command_lists_mutex_.lock_shared();

    auto it = command_lists_.find(command_list);

    if (it != command_lists_.end()) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;

      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_COMPUTE;
      desc->kernel_command_id_ = kernel_id;
      desc->group_size_ = group_size;
      desc->group_count_ = *group_count;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;  // in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->device_ns_per_cycle_ = it->second->device_ns_per_cycle_;
      desc->implicit_scaling_ = it->second->implicit_scaling_;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;

      desc->mem_size_ = 0;
      desc->event_ = event_to_signal;
      desc->in_order_counter_event_ = ze_instance_data.in_order_counter_event_;
      desc->signal_gen_ = ze_instance_data.signal_gen_;
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();

      desc->device_global_timestamps_ = nullptr;
      desc->timestamps_on_event_reset_ = nullptr;
      desc->timestamps_on_commands_completion_ = nullptr;
      desc->timestamp_event_ = nullptr;
      desc->timestamp_seq_ = -1;
      desc->index_timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_event_reset_ = nullptr;

      // Read graph capture state before releasing the lock
      bool graph_capturing = it->second->graph_capturing_;
      ZeGraph* graph_capture_target = it->second->graph_capture_target_;

      command_lists_mutex_.unlock_shared();

      if (ze_instance_data.in_order_counter_event_ && it->second->in_order_) {
        ze_result_t status = ZE_FUNC(zeCommandListAppendSignalEvent)(command_list, ze_instance_data.in_order_counter_event_);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = event_cache_.GetEvent(context);
        if (metric_query_event == nullptr) {
          // HARDEN: pool creation failed (reported by the cache) -- skip the
          // metric query instrumentation instead of appending a null event.
          std::cerr << "[ERROR] No event for metric query, query will not be instrumented" << std::endl;
        } else {
        ze_result_t status = ZE_FUNC(zetCommandListAppendMetricQueryEnd)(command_list, query, metric_query_event, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to append metric query end (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
        }
        desc_query->metric_query_event_ = metric_query_event;
        desc_query->metric_query_ = query;
        desc_query->device_ = it->second->device_;
      }

      uint64_t host_timestamp = ze_instance_data.timestamp_host;

      // Check if command list is in graph capture mode
      ZeGraph* capture_graph = graph_capturing ? graph_capture_target : nullptr;
      if (capture_graph) {
        StoreCommandInGraph(capture_graph, desc, desc_query, host_timestamp, event_to_signal, command_list);
      } else if (it->second->immediate_) {
        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = ze_instance_data.timestamp_device;  // append time and submit time are the same
        desc->command_metric_query_ = nullptr;    // don't care metric query in case of immediate command list
        local_device_submissions_.SubmitKernelCommand(desc);
        kids->push_back(desc->instance_id_);

        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;  // need metric query upon submission

        command_lists_mutex_.lock();

        if (reset_event_on_device_) {
          int seq = it->second->num_timestamps_++;
          it->second->index_timestamps_on_commands_completion_.push_back(-1);
          it->second->index_timestamps_on_event_reset_.push_back(-1);
          it->second->event_to_timestamp_seq_.insert({event_to_signal, seq});

          desc->timestamp_seq_ = seq;
          desc->timestamps_on_event_reset_ = &(it->second->timestamps_on_event_reset_);
          desc->timestamps_on_commands_completion_ = &(it->second->timestamps_on_commands_completion_);
          desc->index_timestamps_on_commands_completion_ = &(it->second->index_timestamps_on_commands_completion_);
          desc->index_timestamps_on_event_reset_ = &(it->second->index_timestamps_on_event_reset_);
        }

        desc->timestamp_event_ = it->second->timestamp_event_to_signal_;

        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }

        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendMemoryCommand(
    ZeDeviceCommandHandle handle,
    size_t size,
    const void* src,
    const void* dst,
    ze_event_handle_t& event_to_signal,
    zet_metric_query_handle_t& query,
    ze_command_list_handle_t command_list,
    std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    ze_context_handle_t context = nullptr;
    command_lists_mutex_.lock_shared();
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      context = it->second->context_;
    }

    int mtype = GetMemoryTransferType(context, src, context, dst);
    if (mtype != -1) {
      handle = ZeDeviceCommandHandle(int(handle) + mtype);
    }

    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;

      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_MEMORY;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;  // in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->device_ns_per_cycle_ = it->second->device_ns_per_cycle_;
      desc->event_ = event_to_signal;
      desc->in_order_counter_event_ = ze_instance_data.in_order_counter_event_;
      desc->signal_gen_ = ze_instance_data.signal_gen_;  // TSLOG v2: memory commands snapshot too
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;

      // Read graph capture state before releasing the lock
      bool graph_capturing = it->second->graph_capturing_;
      ZeGraph* graph_capture_target = it->second->graph_capture_target_;

      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->mem_size_ = size;
      desc->tid_ = utils::GetTid();

      desc->device_global_timestamps_ = nullptr;
      desc->timestamps_on_event_reset_ = nullptr;
      desc->timestamps_on_commands_completion_ = nullptr;
      desc->timestamp_event_ = nullptr;
      desc->timestamp_seq_ = -1;
      desc->index_timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_event_reset_ = nullptr;

      if (ze_instance_data.in_order_counter_event_ && it->second->in_order_) {
        ze_result_t status = ZE_FUNC(zeCommandListAppendSignalEvent)(command_list, ze_instance_data.in_order_counter_event_);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = event_cache_.GetEvent(context);
        if (metric_query_event == nullptr) {
          // HARDEN: pool creation failed (reported by the cache) -- skip the
          // metric query instrumentation instead of appending a null event.
          std::cerr << "[ERROR] No event for metric query, query will not be instrumented" << std::endl;
        } else {
        ze_result_t status = ZE_FUNC(zetCommandListAppendMetricQueryEnd)(command_list, query, metric_query_event, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to append metric query end (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
        }
        desc_query->metric_query_event_ = metric_query_event;
        desc_query->metric_query_ = query;
        desc_query->device_ = it->second->device_;
      }

      uint64_t host_timestamp = ze_instance_data.timestamp_host;

      // Check if command list is in graph capture mode
      ZeGraph* capture_graph = graph_capturing ? graph_capture_target : nullptr;
      if (capture_graph) {
        StoreCommandInGraph(capture_graph, desc, desc_query, host_timestamp, event_to_signal, command_list);
      } else if (it->second->immediate_) {
        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = ze_instance_data.timestamp_device;  // append time and submit time are the same
        desc->command_metric_query_ = nullptr;  // do not care metric query in case of immediate command list

        local_device_submissions_.SubmitKernelCommand(desc);

        kids->push_back(desc->instance_id_);

        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;

        command_lists_mutex_.lock();

        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }

        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendMemoryCommandContext(
      ZeDeviceCommandHandle handle,
      size_t size,
      ze_context_handle_t src_context,
      const void* src,
      ze_context_handle_t dst_context,
      const void* dst,
      ze_event_handle_t& event_to_signal,
      zet_metric_query_handle_t& query,
      ze_command_list_handle_t command_list,
      std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    command_lists_mutex_.lock_shared();
    auto it = command_lists_.find(command_list);

    int mtype = GetMemoryTransferType(src_context, src, dst_context, dst);
    if (mtype != -1) {
      handle = ZeDeviceCommandHandle(int(handle) + mtype);
    }

    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;

      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_MEMORY;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;  // in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->device_ns_per_cycle_ = it->second->device_ns_per_cycle_;
      desc->event_ = event_to_signal;
      desc->in_order_counter_event_ = ze_instance_data.in_order_counter_event_;
      desc->signal_gen_ = ze_instance_data.signal_gen_;  // TSLOG v2: memory commands snapshot too
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;

      // Read graph capture state before releasing the lock
      bool graph_capturing = it->second->graph_capturing_;
      ZeGraph* graph_capture_target = it->second->graph_capture_target_;

      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->mem_size_ = size;
      desc->tid_ = utils::GetTid();

      desc->device_global_timestamps_ = nullptr;
      desc->timestamps_on_event_reset_ = nullptr;
      desc->timestamps_on_commands_completion_ = nullptr;
      desc->timestamp_event_ = nullptr;
      desc->timestamp_seq_ = -1;
      desc->index_timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_event_reset_ = nullptr;

      if (ze_instance_data.in_order_counter_event_ && it->second->in_order_) {
        ze_result_t status = ZE_FUNC(zeCommandListAppendSignalEvent)(command_list, ze_instance_data.in_order_counter_event_);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = event_cache_.GetEvent(context);
        if (metric_query_event == nullptr) {
          // HARDEN: pool creation failed (reported by the cache) -- skip the
          // metric query instrumentation instead of appending a null event.
          std::cerr << "[ERROR] No event for metric query, query will not be instrumented" << std::endl;
        } else {
        ze_result_t status = ZE_FUNC(zetCommandListAppendMetricQueryEnd)(command_list, query, metric_query_event, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to append metric query end (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
        }
        desc_query->metric_query_ = query;
        desc_query->metric_query_event_ = metric_query_event;
      }

      uint64_t host_timestamp = ze_instance_data.timestamp_host;

      // Check if command list is in graph capture mode
      ZeGraph* capture_graph = graph_capturing ? graph_capture_target : nullptr;
      if (capture_graph) {
        StoreCommandInGraph(capture_graph, desc, desc_query, host_timestamp, event_to_signal, command_list);
      } else if (it->second->immediate_) {
        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = ze_instance_data.timestamp_device;  // append time and submit time are the same
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);

        kids->push_back(desc->instance_id_);
        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;

        command_lists_mutex_.lock();

        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }

        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendImageMemoryCopyCommand(
    ZeDeviceCommandHandle handle,
    ze_image_handle_t image,
    const void* src,
    const void* dst,
    ze_event_handle_t& event_to_signal,
    zet_metric_query_handle_t& query,
    ze_command_list_handle_t command_list,
    std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    ze_context_handle_t context = nullptr;
    command_lists_mutex_.lock_shared();
    auto it = command_lists_.find(command_list);
    if (it != command_lists_.end()) {
      context = it->second->context_;
    }

    int mtype = GetMemoryTransferType(context, src, context, dst);
    if (mtype != -1) {
      handle = ZeDeviceCommandHandle(int(handle) + mtype);
    }

    size_t size = GetImageSize(image);

    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;

      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_MEMORY;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;  // in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->device_ns_per_cycle_ = it->second->device_ns_per_cycle_;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;

      // Read graph capture state before releasing the lock
      bool graph_capturing = it->second->graph_capturing_;
      ZeGraph* graph_capture_target = it->second->graph_capture_target_;

      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->event_ = event_to_signal;
      desc->in_order_counter_event_ = ze_instance_data.in_order_counter_event_;
      desc->signal_gen_ = ze_instance_data.signal_gen_;  // TSLOG v2: memory commands snapshot too
      desc->command_list_ = command_list;
      desc->mem_size_ = size;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();

      desc->device_global_timestamps_ = nullptr;
      desc->timestamps_on_event_reset_ = nullptr;
      desc->timestamps_on_commands_completion_ = nullptr;
      desc->timestamp_event_ = nullptr;
      desc->timestamp_seq_ = -1;
      desc->index_timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_event_reset_ = nullptr;

      if (ze_instance_data.in_order_counter_event_ && it->second->in_order_) {
        ze_result_t status = ZE_FUNC(zeCommandListAppendSignalEvent)(command_list, ze_instance_data.in_order_counter_event_);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = event_cache_.GetEvent(context);
        if (metric_query_event == nullptr) {
          // HARDEN: pool creation failed (reported by the cache) -- skip the
          // metric query instrumentation instead of appending a null event.
          std::cerr << "[ERROR] No event for metric query, query will not be instrumented" << std::endl;
        } else {
        ze_result_t status = ZE_FUNC(zetCommandListAppendMetricQueryEnd)(command_list, query, metric_query_event, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to append metric query end (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
        }
        desc_query->metric_query_ = query;
        desc_query->metric_query_event_ = metric_query_event;
      }

      uint64_t host_timestamp = ze_instance_data.timestamp_host;

      // Check if command list is in graph capture mode
      ZeGraph* capture_graph = graph_capturing ? graph_capture_target : nullptr;
      if (capture_graph) {
        StoreCommandInGraph(capture_graph, desc, desc_query, host_timestamp, event_to_signal, command_list);
      } else if (it->second->immediate_) {
        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = ze_instance_data.timestamp_device;  // append time and submit time are the same
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);

        kids->push_back(desc->instance_id_);
        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;

        command_lists_mutex_.lock();

        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }

        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendCommand(
      ZeDeviceCommandHandle handle,
      ze_event_handle_t& event_to_signal,
      zet_metric_query_handle_t& query,
      ze_command_list_handle_t command_list,
      std::vector<uint64_t> *kids) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    command_lists_mutex_.lock_shared();

    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    auto it = command_lists_.find(command_list);
    if (found && (it != command_lists_.end())) {
      ZeCommand *desc = nullptr;
      ZeCommandMetricQuery *desc_query = nullptr;

      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_COMMAND;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = it->second->engine_ordinal_;
      desc->engine_index_ = it->second->engine_index_;
      desc->host_time_origin_ = it->second->host_time_origin_;  // in ns
      desc->device_timer_frequency_ = it->second->device_timer_frequency_;
      desc->device_timer_mask_ = it->second->device_timer_mask_;
      desc->device_ns_per_cycle_ = it->second->device_ns_per_cycle_;
      desc->device_ = it->second->device_;
      ze_context_handle_t context = it->second->context_;

      // Read graph capture state before releasing the lock
      bool graph_capturing = it->second->graph_capturing_;
      ZeGraph* graph_capture_target = it->second->graph_capture_target_;

      command_lists_mutex_.unlock_shared();

      desc->group_count_ = {0, 0, 0};
      desc->mem_size_ = 0;
      desc->event_ = event_to_signal;
      desc->in_order_counter_event_ = ze_instance_data.in_order_counter_event_;
      desc->signal_gen_ = ze_instance_data.signal_gen_;
      desc->command_list_ = command_list;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();

      desc->device_global_timestamps_ = nullptr;
      desc->timestamp_seq_ = -1;
      desc->timestamps_on_event_reset_ = nullptr;
      desc->timestamps_on_commands_completion_ = nullptr;
      desc->timestamp_event_ = nullptr;
      desc->index_timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_event_reset_ = nullptr;

      if (ze_instance_data.in_order_counter_event_ && it->second->in_order_) {
        ze_result_t status = ZE_FUNC(zeCommandListAppendSignalEvent)(command_list, ze_instance_data.in_order_counter_event_);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      if (query != nullptr) {
        desc_query = local_device_submissions_.GetCommandMetricQuery();

        ze_event_handle_t metric_query_event = event_cache_.GetEvent(context);
        desc_query->metric_query_ = query;
        desc_query->metric_query_event_ = metric_query_event;
        if (metric_query_event == nullptr) {
          // HARDEN: pool creation failed (reported by the cache) -- skip the
          // metric query instrumentation instead of appending a null event.
          std::cerr << "[ERROR] No event for metric query, query will not be instrumented" << std::endl;
        } else {
        ze_result_t status = ZE_FUNC(zetCommandListAppendMetricQueryEnd)(command_list, query, metric_query_event, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to append metric query end (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
        }
      }

      uint64_t host_timestamp = ze_instance_data.timestamp_host;

      // Check if command list is in graph capture mode
      ZeGraph* capture_graph = graph_capturing ? graph_capture_target : nullptr;
      if (capture_graph) {
        StoreCommandInGraph(capture_graph, desc, desc_query, host_timestamp, event_to_signal, command_list);
      } else if (it->second->immediate_) {
        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = ze_instance_data.timestamp_device;  // append time and submit time are the same
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);
        kids->push_back(desc->instance_id_);
        if (query != nullptr) {
          desc_query->instance_id_ = desc->instance_id_;
          desc_query->immediate_ = true;
          local_device_submissions_.SubmitCommandMetricQuery(desc_query);
        }
      }
      else {
        // TODO: what happens if an event associated with a barrier gets reset?
        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = desc_query;

        command_lists_mutex_.lock();

        it->second->commands_.push_back(desc);
        if (query != nullptr) {
          desc_query->immediate_ = false;
          it->second->metric_queries_.push_back(desc_query);
        }

        command_lists_mutex_.unlock();
      }
    }
    else {
      command_lists_mutex_.unlock_shared();
    }
  }

  void AppendCommand(ZeDeviceCommandHandle handle, ZeCommandList *cl, std::vector<uint64_t> *kids, uint64_t *dts) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    if (dts == nullptr) {
      std::cerr << "[WARNING] Invalid timestamp slot" << std::endl;
      return;  // ignore this command
    }

    uint64_t command_id;
    bool found = false;

    kernel_command_properties_mutex_.lock_shared();
    auto kit = active_command_properties_->find(handle);
    if (kit != active_command_properties_->end()) {
      command_id = kit->second.id_;
      found = true;
    }
    kernel_command_properties_mutex_.unlock_shared();

    if (found) {
      ZeCommand *desc = nullptr;

      desc = local_device_submissions_.GetKernelCommand();

      desc->type_ = KERNEL_COMMAND_TYPE_COMMAND;
      desc->kernel_command_id_ = command_id;;
      desc->engine_ordinal_ = cl->engine_ordinal_;
      desc->engine_index_ = cl->engine_index_;
      desc->host_time_origin_ = cl->host_time_origin_;  // in ns
      desc->device_timer_frequency_ = cl->device_timer_frequency_;
      desc->device_timer_mask_ = cl->device_timer_mask_;
      desc->device_ns_per_cycle_ = cl->device_ns_per_cycle_;
      desc->device_ = cl->device_;

      desc->group_count_ = {0, 0, 0};
      desc->mem_size_ = 0;
      desc->event_ = nullptr;
      desc->in_order_counter_event_ = ze_instance_data.in_order_counter_event_;
      desc->command_list_ = cl->cmdlist_;
      desc->queue_ = nullptr;
      desc->tid_ = utils::GetTid();

      desc->timestamp_seq_ = -1;
      desc->timestamps_on_event_reset_ = nullptr;
      desc->timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_commands_completion_ = nullptr;
      desc->index_timestamps_on_event_reset_ = nullptr;

      // dts points to end timestamp but we need start timestamp too which is immediately followed by end timestamp
      // hence dts - 1
      desc->device_global_timestamps_ = dts - 1;  // dts points to end timestamp but start timestamp is needed also which immediately
      desc->timestamp_event_ = cl->timestamp_event_to_signal_;

      uint64_t host_timestamp = ze_instance_data.timestamp_host;
      if (cl->immediate_) {
        desc->immediate_ = true;
        desc->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
        desc->append_time_ = host_timestamp;
        desc->submit_time_ = host_timestamp;
        desc->submit_time_device_ = ze_instance_data.timestamp_device;  // append time and submit time are the same
        desc->command_metric_query_ = nullptr;

        local_device_submissions_.SubmitKernelCommand(desc);
        kids->push_back(desc->instance_id_);
      }
      else {
        desc->append_time_ = host_timestamp;
        desc->immediate_ = false;
        desc->command_metric_query_ = nullptr;
        cl->commands_.push_back(desc);
      }
    }
  }

  // Stores a command into a graph during capture mode and tracks the signal event.
  void StoreCommandInGraph(ZeGraph* graph, ZeCommand* desc,
                           ZeCommandMetricQuery* desc_query, uint64_t host_timestamp,
                           ze_event_handle_t signal_event,
                           ze_command_list_handle_t command_list) {
    desc->immediate_ = true;
    desc->graph_command_ = true;
    desc->append_time_ = host_timestamp;
    desc->command_metric_query_ = desc_query;

    graph->commands_.push_back(desc);
    if (desc_query != nullptr) {
      desc_query->immediate_ = false;
      graph->metric_queries_.push_back(desc_query);
    }

    // Track signal event for fork detection and join handling
    if (signal_event != nullptr) {
      graph->event_to_cmdlist_[signal_event] = command_list;
    }
  }

  // Detects fork and join transitions during graph capture.
  //
  // Fork: when a non-capturing cmdlist waits on an event signaled during an
  //   active graph capture, it auto-transitions into capture mode.
  // Join: when the primary cmdlist waits on an event signaled by a forked
  //   cmdlist, capture ends on that fork. Per spec each fork must join back
  //   to the primary cmdlist (no nested forks).
  void CheckAndApplyForkTransition(
      ze_command_list_handle_t command_list,
      uint32_t num_wait_events, ze_event_handle_t* wait_events) {
    if (wait_events == nullptr || num_wait_events == 0) return;

    bool already_capturing = false;
    ZeGraph* existing_target = nullptr;
    {
      std::shared_lock<std::shared_mutex> lock(command_lists_mutex_);
      auto clit = command_lists_.find(command_list);
      if (clit == command_lists_.end()) {
        return;
      }
      already_capturing = clit->second->graph_capturing_;
      existing_target = clit->second->graph_capture_target_;
    }

    // Find the graph whose captured signal events match a wait event
    ZeGraph* target = nullptr;
    {
      std::shared_lock<std::shared_mutex> lock(graphs_mutex_);
      for (auto& [gh, ginfo] : graphs_) {
        for (uint32_t i = 0; i < num_wait_events; ++i) {
          if (ginfo->event_to_cmdlist_.count(wait_events[i])) {
            target = ginfo;
            break;
          }
        }
        if (target) break;
      }
    }

    if (target == nullptr) return;

    // --- Join: primary waits on fork events → end capture on those forks ---
    if (already_capturing && command_list == target->primary_command_list_) {
      std::lock_guard<std::shared_mutex> cmd_lock(command_lists_mutex_);
      std::lock_guard<std::shared_mutex> graph_lock(graphs_mutex_);
      for (uint32_t i = 0; i < num_wait_events; ++i) {
        auto evt_it = target->event_to_cmdlist_.find(wait_events[i]);
        if (evt_it == target->event_to_cmdlist_.end()) continue;
        ze_command_list_handle_t fork_cl = evt_it->second;
        if (fork_cl == target->primary_command_list_) continue;
        auto fit = command_lists_.find(fork_cl);
        if (fit != command_lists_.end() && fit->second->graph_capturing_) {
          fit->second->graph_capturing_ = false;
          uni_batchqkt_capture_depth_.fetch_sub(1, std::memory_order_acq_rel);  // HARDEN
          fit->second->graph_capture_target_ = nullptr;
        }
        target->forked_command_lists_.erase(fork_cl);
      }
      return;
    }

    // Already capturing into the same graph — nothing to do
    if (already_capturing && existing_target == target) return;

    // Cross-graph merge prevention
    if (already_capturing && existing_target != nullptr && existing_target != target) {
      std::cerr << "[ERROR] Cross-graph merge detected: command list " << command_list
                << " is already capturing into a different graph." << std::endl;
      return;
    }

    // --- Fork: auto-transition this cmdlist into capture mode ---
    {
      std::lock_guard<std::shared_mutex> lock(command_lists_mutex_);
      auto it = command_lists_.find(command_list);
      if (it != command_lists_.end() && !it->second->graph_capturing_) {
        it->second->graph_capturing_ = true;
        uni_batchqkt_capture_depth_.fetch_add(1, std::memory_order_acq_rel);  // HARDEN
        it->second->graph_capture_target_ = target;
      }
    }
    {
      std::lock_guard<std::shared_mutex> lock(graphs_mutex_);
      target->forked_command_lists_.insert(command_list);
    }
  }

  // Called when zeGraphDestroyExp succeeds - cleanup graph tracking
  void OnGraphDestroy(ze_graph_handle_t graph) {
    std::lock_guard<std::shared_mutex> cmd_lock(command_lists_mutex_);
    std::lock_guard<std::shared_mutex> graph_lock(graphs_mutex_);
    auto git = graphs_.find(graph);
    if (git != graphs_.end()) {
      ZeGraph* g = git->second;
      if (g != nullptr) {
        // Reset any command lists still referencing this graph
        for (auto& [cl_handle, cl] : command_lists_) {
          if (cl->graph_capture_target_ == g) {
            if (cl->graph_capturing_) {  // HARDEN
              cl->graph_capturing_ = false;
              uni_batchqkt_capture_depth_.fetch_sub(1, std::memory_order_acq_rel);
            }
            cl->graph_capture_target_ = nullptr;
            cl->pending_graph_capture_ = nullptr;
          }
        }
        ReleaseGraphResources(*g);
        delete g;
      }
      graphs_.erase(git);
    }
  }

  // True if the cmdlist is currently inside a graph capture. Used so the
  // append callbacks keep capturing into the graph even when collection is
  // paused.
  bool IsCommandListGraphCapturing(ze_command_list_handle_t command_list) {
    std::shared_lock<std::shared_mutex> lock(command_lists_mutex_);
    auto it = command_lists_.find(command_list);
    return (it != command_lists_.end() && it->second->graph_capturing_);
  }

  // True if any wait event is a fork-signal of an active graph capture.
  // Lets the very first append on a fork cmdlist through while collection
  // is paused, before its graph_capturing_ flag has been flipped.
  bool WouldTriggerGraphCapture(uint32_t num_wait_events,
                                ze_event_handle_t* wait_events) {
    if (wait_events == nullptr || num_wait_events == 0) return false;
    std::shared_lock<std::shared_mutex> lock(graphs_mutex_);
    for (auto& [gh, ginfo] : graphs_) {
      for (uint32_t i = 0; i < num_wait_events; ++i) {
        if (ginfo->event_to_cmdlist_.count(wait_events[i])) {
          return true;
        }
      }
    }
    return false;
  }

  // Whether an append on this command list should be instrumented. Always
  // true when collection is enabled; while paused, still true if the list is
  // mid graph-capture, so the captured graph is populated for later replay.
  bool ShouldCaptureAppend(ze_command_list_handle_t command_list,
                           uint32_t num_wait_events,
                           ze_event_handle_t* wait_events) {
    return UniController::IsCollectionEnabled() ||
           IsCommandListGraphCapturing(command_list) ||
           WouldTriggerGraphCapture(num_wait_events, wait_events);
  }

  // Release all resources owned by a ZeGraph (events, metric queries, commands)
  void ReleaseGraphResources(ZeGraph& graph_info) {
    // Fix B barrier: the graph's shared events are released/recreated below;
    // queued clones must have read their packets first.
    FlushDeferredTimestamps();
    // T6'/E6 (v3): accumulate mode can leave this replay's clones queued
    // (no staging drain ran since its execution). Their events are
    // released/recreated right below, so flush the local thread's pending
    // clones now — this is exactly the "graph destroyed while clones are
    // pending" path (engine teardown, re-capture). Unready clones (none, if
    // the app synchronized before destroying) stay queued and their dead
    // event handles fail the readiness status check at every later drain, so
    // they can never reach the batch append; worst case is a few leaked
    // ZeCommand structs, never a dangling handle in a driver call.
    // Lock order: callers already hold command_lists_mutex_/graphs_mutex_ and
    // this sweep only takes global_device_submissions_mutex_ (shared), the
    // same order DestroyCommandList -> ProcessAllCommandsSubmitted uses.
    if (BatchQktActive() && !uni_batchqkt_in_batch_ &&
        !local_device_submissions_.IsFinalized()) {
      ProcessCommandsSubmitted(nullptr);
    }
    for (auto* cmd : graph_info.commands_) {
      if (cmd != nullptr) {
        if (cmd->event_ != nullptr) {
          event_cache_.ReleaseEvent(cmd->event_);
          cmd->event_ = nullptr;
        }
        delete cmd;
      }
    }
    for (auto* mq : graph_info.metric_queries_) {
      if (mq != nullptr) {
        if (mq->metric_query_event_ != nullptr) {
          event_cache_.ReleaseEvent(mq->metric_query_event_);
          mq->metric_query_event_ = nullptr;
        }
        if (mq->metric_query_ != nullptr) {
          query_pools_.PutQuery(mq->metric_query_);
          mq->metric_query_ = nullptr;
        }
        delete mq;
      }
    }
    graph_info.commands_.clear();
    graph_info.metric_queries_.clear();
  }

  // Called when zeExecutableGraphDestroyExp succeeds - cleanup executable graph mapping
  void OnExecutableGraphDestroy(ze_executable_graph_handle_t exec_graph) {
    std::lock_guard<std::shared_mutex> lock(executable_graphs_mutex_);
    executable_to_source_graph_.erase(exec_graph);
  }

  // Called when zeCommandListBeginGraphCaptureExp succeeds (no graph handle yet).
  // Allocates a temporary ZeGraph so that Append* methods can capture commands
  // into it.  OnEndGraphCapture will adopt this into graphs_ under the real handle.
  void OnBeginGraphCaptureNoGraph(ze_command_list_handle_t command_list) {
    std::lock_guard<std::shared_mutex> lock(command_lists_mutex_);
    auto it = command_lists_.find(command_list);
    if (it == command_lists_.end()) {
      return;
    }

    // Allocate a temporary graph info to accumulate captured commands
    ZeGraph* pending = new ZeGraph();
    pending->primary_command_list_ = command_list;

    it->second->graph_capturing_ = true;
    uni_batchqkt_capture_depth_.fetch_add(1, std::memory_order_acq_rel);  // HARDEN
    it->second->graph_capture_target_ = pending;
    it->second->pending_graph_capture_ = pending;  // Ownership tracked here
  }

  // Called when zeCommandListBeginCaptureIntoGraphExp succeeds - start tracking captures
  void OnBeginGraphCapture(ze_command_list_handle_t command_list, ze_graph_handle_t graph) {
    {
      std::shared_lock<std::shared_mutex> lock(command_lists_mutex_);
      if (command_lists_.find(command_list) == command_lists_.end()) {
        return;
      }
    }

    // Create or update the graph entry and reset capture state
    ZeGraph* graph_info = nullptr;
    {
      std::lock_guard<std::shared_mutex> lock(graphs_mutex_);
      auto git = graphs_.try_emplace(graph, nullptr).first;
      if (git->second == nullptr) {
        git->second = new ZeGraph();
      }
      graph_info = git->second;
      graph_info->commands_.clear();
      graph_info->metric_queries_.clear();
      graph_info->primary_command_list_ = command_list;
      graph_info->forked_command_lists_.clear();
      graph_info->event_to_cmdlist_.clear();
    }

    // Mark command list as in graph capture mode with pointer to graph in graphs_ map
    {
      std::lock_guard<std::shared_mutex> lock(command_lists_mutex_);
      auto it = command_lists_.find(command_list);
      if (it != command_lists_.end()) {
        if (!it->second->graph_capturing_) {  // HARDEN: count each window once
          it->second->graph_capturing_ = true;
          uni_batchqkt_capture_depth_.fetch_add(1, std::memory_order_acq_rel);
        }
        it->second->graph_capture_target_ = graph_info;
      }
    }
  }

  // Called when zeCommandListEndGraphCaptureExp succeeds - stop tracking captures
  void OnEndGraphCapture(ze_command_list_handle_t command_list, ze_graph_handle_t graph) {
    // Get pending capture (if any) and reset capture state on command list
    ZeGraph* pending = nullptr;

    // Reset capture state on primary and all forked command lists
    {
      std::lock_guard<std::shared_mutex> lock(command_lists_mutex_);
      auto it = command_lists_.find(command_list);
      if (it != command_lists_.end()) {
        pending = it->second->pending_graph_capture_;
        ZeGraph* target = it->second->graph_capture_target_;
        if (it->second->graph_capturing_) {  // HARDEN
          it->second->graph_capturing_ = false;
          uni_batchqkt_capture_depth_.fetch_sub(1, std::memory_order_acq_rel);
        }
        it->second->graph_capture_target_ = nullptr;
        it->second->pending_graph_capture_ = nullptr;

        // End capture on all forked command lists
        if (target != nullptr) {
          for (auto forked_cl : target->forked_command_lists_) {
            auto fit = command_lists_.find(forked_cl);
            if (fit != command_lists_.end()) {
              if (fit->second->graph_capturing_) {  // HARDEN
                fit->second->graph_capturing_ = false;
                uni_batchqkt_capture_depth_.fetch_sub(1, std::memory_order_acq_rel);
              }
              fit->second->graph_capture_target_ = nullptr;
            }
          }
          // Clear event tracking data now that capture is complete.
          // Prevents stale events from triggering false fork matches
          // in CheckAndApplyForkTransition during a later capture.
          target->event_to_cmdlist_.clear();
          target->forked_command_lists_.clear();
        }
      }
    }

    {
      std::lock_guard<std::shared_mutex> lock(graphs_mutex_);
      auto git = graphs_.find(graph);
      if (git == graphs_.end()) {
        // BeginGraphCaptureExp path: adopt the pending capture into graphs_
        if (pending != nullptr) {
          graphs_.insert({graph, pending});
        }
      } else {
        // BeginCaptureIntoGraphExp path: graph already exists with captured commands.
        // If there was somehow a pending capture, clean it up.
        if (pending != nullptr) {
          ReleaseGraphResources(*pending);
          delete pending;
        }
      }
    }
  }

  // Called when zeCommandListInstantiateGraphExp succeeds - map executable to source graph
  void OnInstantiateGraph(ze_graph_handle_t graph, ze_executable_graph_handle_t exec_graph) {
    // Map executable graph directly to source graph's ZeGraph
    ZeGraph* graph_info = nullptr;
    {
      std::shared_lock<std::shared_mutex> lock(graphs_mutex_);
      auto git = graphs_.find(graph);
      if (git != graphs_.end()) {
        graph_info = git->second;
      }
    }

    if (graph_info != nullptr) {
      std::lock_guard<std::shared_mutex> lock(executable_graphs_mutex_);
      executable_to_source_graph_.insert({exec_graph, graph_info});
    }
  }

  // Called from Enter callback for zeCommandListAppendGraphExp.
  // Prepares timing entries for all captured kernels in the graph.
  // Mirrors the pattern in OnEnterCommandListImmediateAppendCommandListsExp.
  void PrepareGraphExecution(
      ze_command_list_handle_t command_list,
      ze_executable_graph_handle_t exec_graph) {

    if (local_device_submissions_.IsFinalized()) {
      return;
    }

    // Look up source graph info directly from executable graph
    ZeGraph* graph_info = nullptr;
    executable_graphs_mutex_.lock_shared();
    auto eit = executable_to_source_graph_.find(exec_graph);
    if (eit != executable_to_source_graph_.end()) {
      graph_info = eit->second;
    }
    executable_graphs_mutex_.unlock_shared();

    if (graph_info == nullptr) {
      return;  // Graph not tracked
    }

    // T14/A5: the step landmark. An instant at every graph replay dispatch --
    // the navigation band humans and scripts use to split the trace into
    // steps -- plus the per-step debt counter sample (meta microseconds
    // accumulated since the previous landmark, rendered as a Perfetto counter
    // track). Runs inside the urEnqueueGraphExp host API slice, before this
    // step's staging drain.
    if (MetaOn()) {
      const uint64_t step = uni_meta_step_.fetch_add(1, std::memory_order_relaxed) + 1;
      const uint64_t mt0 = UniTimer::GetHostTimestamp();
      const double debt_ms = static_cast<double>(uni_meta_debt_us_.exchange(0, std::memory_order_relaxed)) / 1000.0;
      MetaRecord(("unitrace.step " + std::to_string(step)).c_str(), mt0, mt0,
                 "\"step\": " + std::to_string(step), debt_ms);
    }

    // Get command list info for execution context
    ze_device_handle_t device = nullptr;
    ze_context_handle_t context = nullptr;
    uint32_t engine_ordinal = 0;
    uint32_t engine_index = 0;

    command_lists_mutex_.lock_shared();
    auto clit = command_lists_.find(command_list);
    if (clit != command_lists_.end()) {
      device = clit->second->device_;
      context = clit->second->context_;
      engine_ordinal = clit->second->engine_ordinal_;
      engine_index = clit->second->engine_index_;
    }
    command_lists_mutex_.unlock_shared();

    if (device == nullptr) {
      return;
    }

    // The graph's captured commands reuse the same physical events on every
    // replay. If a previous replay's clones are still in flight, this replay
    // would reset those events before their timestamps are read, dropping the
    // prior replay's records. A previous replay may have been appended to a
    // different command list, so wait on every command list that currently
    // has an in-flight clone carrying one of this graph's events,
    // then process the drained commands before staging this replay.
    {
      std::set<ze_event_handle_t> graph_events;
      std::set<ze_command_list_handle_t> lists_to_wait;
      {
        // TAX-OPT: collection phase (lock_shared + per-command iteration +
        // set inserts). The sync loop below is a blocking device wait and is
        // deliberately NOT inside this timer.
        UniPhaseTimer ph_evcollect(uni_ph_evcollect_us_, uni_ph_evcollect_n_);
        graphs_mutex_.lock_shared();
        for (auto cmd : graph_info->commands_) {
          if (cmd->event_ != nullptr) {
            graph_events.insert(cmd->event_);
          }
        }
        graphs_mutex_.unlock_shared();

        global_device_submissions_mutex_.lock_shared();
        for (auto cmd : local_device_submissions_.commands_submitted_) {
          if (cmd->command_list_ != nullptr && graph_events.count(cmd->event_)) {
            lists_to_wait.insert(cmd->command_list_);
          }
        }
        global_device_submissions_mutex_.unlock_shared();
      }

      for (auto list : lists_to_wait) {
        ZE_FUNC(zeCommandListHostSynchronize)(list, UINT64_MAX);
      }
      // Graph replay fix v3: the host-synchronize above proves the in-flight
      // clones of this graph completed. First clear a stale latch left on an
      // event with no pending clones (e.g. a warmup-era signal whose commands
      // were already drained), then run the NORMAL status-gated sweep — the
      // per-event pending counter guarantees the shared event is only reset
      // after its last clone has read it, so the status gate can no longer
      // strand same-generation clones into the next replay.
      // T6'/E6 (v3): this drain is also THE batch flush point — every clone
      // accumulated since the last replay is complete here, so the sweep's
      // collect+execute reads the whole step's packets in one batch (~500us)
      // and the consuming loop resets the shared events before the clone
      // loop below re-bumps and re-signals them for this replay.
      for (auto event : graph_events) {
        if (!EventHistoryHasPending(event) &&
            ZE_FUNC(zeEventQueryStatus)(event) == ZE_RESULT_SUCCESS) {
          event_cache_.ResetEvent(event);
        }
      }
      {
        // TAX-OPT: the staging drain (collect + execute + reset batch live
        // here; qkt_batch/reset_batch already carry their own meta records).
        UniPhaseTimer ph_drain(uni_ph_drain_us_, uni_ph_drain_n_);
        ProcessAllCommandsSubmitted(nullptr, /*at_gexp=*/true);
      }
    }

    // Get timestamp for submit time
    uint64_t host_timestamp = 0;
    uint64_t device_timestamp = 0;

    ze_result_t status = ZE_FUNC(zeDeviceGetGlobalTimestamps)(device, &host_timestamp, &device_timestamp);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    // Get graph captured commands and clone them for this execution
    graphs_mutex_.lock_shared();
    if (graph_info->commands_.empty()) {
      graphs_mutex_.unlock_shared();
      return;
    }

    // Clone each captured command for this execution (similar to PrepareToExecuteCommandListsLocked)
    UniPhaseTimer ph_clone(uni_ph_clone_us_, uni_ph_clone_n_);
    for (auto command : graph_info->commands_) {
      ZeCommand* cmd = local_device_submissions_.GetKernelCommand();
      ZeCommandMetricQuery* cmd_query = nullptr;

      if (command->command_metric_query_ != nullptr) {
        cmd_query = local_device_submissions_.GetCommandMetricQuery();
      }
      *cmd = *command;

      // T6'/E6: owning context of this replay, recorded lock-free on the clone
      // so the sweep can group the batched timestamp read per context.
      cmd->context_ = context;
      cmd->instance_id_ = UniKernelInstanceId::GetKernelInstanceId();
      // TSLOG v2: this replay re-signals the SAME physical event. Bump its
      // generation and snapshot it as the generation this clone expects to read.
      cmd->signal_gen_ = EventHistoryBumpSignal(cmd->event_, /*path=*/1, cmd->instance_id_);
      // Graph replay fix v3: register this clone as a pending consumer so the
      // shared event is reset exactly when the last clone of this replay has
      // read its packet — never while a same-generation clone still needs it,
      // and never left latched into the next replay's wait.
      EventHistoryAddPending(cmd->event_);
      cmd->engine_ordinal_ = engine_ordinal;
      cmd->engine_index_ = engine_index;
      cmd->submit_time_ = host_timestamp;		//in ns
      cmd->submit_time_device_ = device_timestamp;	//in ticks
      cmd->tid_ = utils::GetTid();
      // Record the command list this replay runs on, so a later replay can
      // host-synchronize on it before reusing the shared captured events.
      cmd->command_list_ = command_list;

      local_device_submissions_.StageKernelCommand(cmd);

      if (cmd_query) {
        *cmd_query = *(command->command_metric_query_);
        cmd_query->instance_id_ = cmd->instance_id_;
        local_device_submissions_.StageCommandMetricQuery(cmd_query);
      }
      else {
        local_device_submissions_.StageCommandMetricQuery(nullptr);
      }
    }

    graphs_mutex_.unlock_shared();

    // TAX-OPT: per-replay enqueue-phase attribution. The snapshot-and-reset
    // here covers this gexp call's collector phases plus the eager kernel
    // append work accumulated since the previous replay (steady state:
    // one decode iteration's worth).
    if (MetaOn()) {
      const uint64_t mtq = UniTimer::GetHostTimestamp();
      MetaRecord("unitrace.enqueue", mtq, mtq,
                 "\"evcollect_us\": " + std::to_string(uni_ph_evcollect_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"drain_us\": " + std::to_string(uni_ph_drain_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"clone_us\": " + std::to_string(uni_ph_clone_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"appendk_us\": " + std::to_string(uni_ph_appendk_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"prepk_us\": " + std::to_string(uni_ph_prepk_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"appendk_n\": " + std::to_string(uni_ph_appendk_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"prepk_n\": " + std::to_string(uni_ph_prepk_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_flush1_us\": " + std::to_string(uni_pd_flush1_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_lock_us\": " + std::to_string(uni_pd_lock_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_consume_us\": " + std::to_string(uni_pd_consume_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_pollread_us\": " + std::to_string(uni_pd_pollread_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_loop_us\": " + std::to_string(uni_pd_loop_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_resets_us\": " + std::to_string(uni_pd_resets_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_flush2_us\": " + std::to_string(uni_pd_flush2_us_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_loop_n\": " + std::to_string(uni_pd_loop_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_outer_n\": " + std::to_string(uni_pd_outer_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_iter_n\": " + std::to_string(uni_pd_iter_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_take_n\": " + std::to_string(uni_pd_take_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_acc_n\": " + std::to_string(uni_pd_acc_n_.exchange(0, std::memory_order_relaxed)) +
                 ", \"d_lq_n\": " + std::to_string(uni_pd_lq_n_.exchange(0, std::memory_order_relaxed)));
    }
  }

  static void OnEnterCommandListAppendLaunchKernel(
      ze_command_list_append_launch_kernel_params_t* params,
      void* global_data, void** instance_data) {
    // Snapshot the collection-enabled decision here, at enter, into the
    // thread-local instance state so the matching OnExit acts on this snapshot.
    // If OnExit instead re-checked UniController::IsCollectionEnabled() and a
    // session was stopped between enter and exit, it would destroy the
    // profiling event already appended into the command list and corrupt the
    // in-flight GPU submission (seen as UR_RESULT_ERROR_UNKNOWN on queue wait).
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true, *(params->phKernel),
          *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendLaunchKernel(
    ze_command_list_append_launch_kernel_params_t* params,
    ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {

    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    // Act on the enter-time snapshot (ze_instance_data.instrument_), not a
    // fresh IsCollectionEnabled() check: if the session was stopped between
    // enter and exit, re-checking here would release (zeEventDestroy) the
    // profiling event already queued on the GPU and crash the app's wait.
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendLaunchKernel(
        *(params->phKernel),
        *(params->ppLaunchFuncArgs),
        *(params->phSignalEvent),
        ze_instance_data.query_,
        *(params->phCommandList),
        kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendLaunchKernelWithArguments(
      ze_command_list_append_launch_kernel_with_arguments_params_t* params,
      void* global_data, void** instance_data) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true, *(params->phKernel),
          *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendLaunchKernelWithArguments(
      ze_command_list_append_launch_kernel_with_arguments_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
        collector->AppendLaunchKernel(
          *(params->phKernel),
          params->pgroupCounts,
          *(params->phSignalEvent),
          ze_instance_data.query_,
          *(params->phCommandList),
          kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendLaunchKernelWithParameters(
      ze_command_list_append_launch_kernel_with_parameters_params_t* params,
      void* global_data, void** instance_data) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true, *(params->phKernel),
          *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
    else {
      *instance_data = nullptr;
    }
  }

  static void OnExitCommandListAppendLaunchKernelWithParameters(
      ze_command_list_append_launch_kernel_with_parameters_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
        collector->AppendLaunchKernel(
          *(params->phKernel),
          *(params->ppGroupCounts),
          *(params->phSignalEvent),
          ze_instance_data.query_,
          *(params->phCommandList),
          kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendLaunchCooperativeKernel(
      ze_command_list_append_launch_cooperative_kernel_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true, *(params->phKernel),
          *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendLaunchCooperativeKernel(
      ze_command_list_append_launch_cooperative_kernel_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
        collector->AppendLaunchKernel(
          *(params->phKernel),
          *(params->ppLaunchFuncArgs),
          *(params->phSignalEvent),
          ze_instance_data.query_,
          *(params->phCommandList),
          kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendLaunchKernelIndirect(
      ze_command_list_append_launch_kernel_indirect_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), true, *(params->phKernel),
          *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendLaunchKernelIndirect(
      ze_command_list_append_launch_kernel_indirect_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
        collector->AppendLaunchKernel(
          *(params->phKernel),
          *(params->ppLaunchArgumentsBuffer),
          *(params->phSignalEvent),
          ze_instance_data.query_,
          *(params->phCommandList),
          kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  int GetMemoryTransferType(ze_context_handle_t src_context, const void *src) {

    int stype = -1;

    if (src_context != nullptr && src != nullptr) {
      ze_memory_allocation_properties_t props{ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,};
      ze_result_t status = ZE_FUNC(zeMemGetAllocProperties)(src_context, src, &props, nullptr);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      switch (props.type) {
        case ZE_MEMORY_TYPE_HOST:
          stype = 0;
          break;
        case ZE_MEMORY_TYPE_DEVICE:
          stype = 1;
          break;
        case ZE_MEMORY_TYPE_UNKNOWN:
          stype = 2;
          break;
        case ZE_MEMORY_TYPE_SHARED:
          stype = 3;
          break;
        case ZE_MEMORY_TYPE_HOST_IMPORTED:
          stype = 0;
          break;
        default:
          break;
      }
    }
    return stype;
  }

  int GetMemoryTransferType(ze_context_handle_t src_context, const void *src, ze_context_handle_t dst_context, const void *dst) {

    int stype = -1;
    int dtype = -1;

    if (src_context != nullptr && src != nullptr) {
      ze_memory_allocation_properties_t props{ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,};
      ze_result_t status = ZE_FUNC(zeMemGetAllocProperties)(src_context, src, &props, nullptr);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      switch (props.type) {
        case ZE_MEMORY_TYPE_HOST:
          stype = 0;
          break;
        case ZE_MEMORY_TYPE_DEVICE:
          stype = 1;
          break;
        case ZE_MEMORY_TYPE_UNKNOWN:
          stype = 2;
          break;
        case ZE_MEMORY_TYPE_SHARED:
          stype = 3;
          break;
        case ZE_MEMORY_TYPE_HOST_IMPORTED:
          stype = 0;
          break;
        default:
          break;
      }
    }

    if (dst_context != nullptr && dst != nullptr) {
      ze_memory_allocation_properties_t props{ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,};
      ze_result_t status = ZE_FUNC(zeMemGetAllocProperties)(dst_context, dst, &props, nullptr);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      switch (props.type) {
        case ZE_MEMORY_TYPE_HOST:
          dtype = 0;
          break;
        case ZE_MEMORY_TYPE_DEVICE:
          dtype = 1;
          break;
        case ZE_MEMORY_TYPE_UNKNOWN:
          dtype = 2;
          break;
        case ZE_MEMORY_TYPE_SHARED:
          dtype = 3;
          break;
        case ZE_MEMORY_TYPE_HOST_IMPORTED:
          dtype = 0;
          break;
        default:
          break;
      }
    }

    if ((stype != -1) && (dtype != -1)) {
      return (stype << 2 | dtype);
    }
    else {
      return stype;
    }
  }

  static void OnEnterCommandListAppendMemoryCopy(
      ze_command_list_append_memory_copy_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendMemoryCopy(
      ze_command_list_append_memory_copy_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendMemoryCommand(MemoryCopy, *(params->psize),
          *(params->psrcptr), *(params->pdstptr), *(params->phSignalEvent), ze_instance_data.query_,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryFill(
      ze_command_list_append_memory_fill_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendMemoryFill(
      ze_command_list_append_memory_fill_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendMemoryCommand(MemoryFill, *(params->psize),
          *(params->pptr), nullptr, *(params->phSignalEvent), ze_instance_data.query_,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendBarrier(
      ze_command_list_append_barrier_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendBarrier(
      ze_command_list_append_barrier_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendCommand(Barrier, *(params->phSignalEvent), ze_instance_data.query_,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryRangesBarrier(
      ze_command_list_append_memory_ranges_barrier_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendMemoryRangesBarrier(
      ze_command_list_append_memory_ranges_barrier_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendCommand(MemoryRangesBarrier, *(params->phSignalEvent), ze_instance_data.query_,
          *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryCopyRegion(
      ze_command_list_append_memory_copy_region_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendMemoryCopyRegion(
      ze_command_list_append_memory_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      size_t bytes_transferred = 0;
      const ze_copy_region_t* region = *(params->psrcRegion);

      if (region != nullptr) {
        if (region->depth != 0) {
          bytes_transferred *= region->depth;
        }
      }

      collector->AppendMemoryCommand(MemoryCopyRegion, bytes_transferred,
        *(params->psrcptr), *(params->pdstptr), *(params->phSignalEvent), ze_instance_data.query_,
         *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendMemoryCopyFromContext(
      ze_command_list_append_memory_copy_from_context_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendMemoryCopyFromContext(
      ze_command_list_append_memory_copy_from_context_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      ze_context_handle_t src_context = *(params->phContextSrc);
      collector->AppendMemoryCommandContext(MemoryCopyFromContext, *(params->psize),
        src_context, *(params->psrcptr), nullptr, *(params->pdstptr), *(params->phSignalEvent), ze_instance_data.query_,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopy(
      ze_command_list_append_image_copy_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendImageCopy(
      ze_command_list_append_image_copy_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendImageMemoryCopyCommand(ImageCopy, *(params->phSrcImage),
        nullptr, nullptr, *(params->phSignalEvent), ze_instance_data.query_,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopyRegion(
      ze_command_list_append_image_copy_region_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendImageCopyRegion(
      ze_command_list_append_image_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendImageMemoryCopyCommand(ImageCopyRegion, *(params->phSrcImage),
        nullptr, nullptr, *(params->phSignalEvent), ze_instance_data.query_,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopyToMemory(
      ze_command_list_append_image_copy_to_memory_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendImageCopyToMemory(
      ze_command_list_append_image_copy_to_memory_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      collector->AppendImageMemoryCopyCommand(ImageCopyToMemory, *(params->phSrcImage),
        nullptr, *(params->pdstptr), *(params->phSignalEvent), ze_instance_data.query_,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendImageCopyFromMemory(
      ze_command_list_append_image_copy_from_memory_params_t* params,
      void* global_data, void** /* instance_data */) {
    ze_instance_data.instrument_ = false;
    ze_instance_data.query_ = nullptr;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumWaitEvents),
                                       *(params->pphWaitEvents))) {
      PrepareToAppendKernelCommand(collector, *(params->phSignalEvent), *(params->phCommandList), false,
          nullptr, *(params->pnumWaitEvents), *(params->pphWaitEvents));
    }
  }

  static void OnExitCommandListAppendImageCopyFromMemory(
      ze_command_list_append_image_copy_from_memory_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if ((result == ZE_RESULT_SUCCESS) && ze_instance_data.instrument_) {
      size_t bytes_transferred = 0;
      const ze_image_region_t* region = *(params->ppDstRegion);

      if (region != nullptr) {
        bytes_transferred = region->width * region->height;
        if (region->depth != 0) {
          bytes_transferred *= region->depth;
        }
      }

      collector->AppendMemoryCommand(ImageCopyFromMemory, bytes_transferred,
        *(params->psrcptr), nullptr, *(params->phSignalEvent), ze_instance_data.query_,
        *(params->phCommandList), kids);
    }
    else {
      collector->query_pools_.PutQuery(ze_instance_data.query_);
      collector->event_cache_.ReleaseEvent(*(params->phSignalEvent));
    }
  }

  static void OnEnterCommandListAppendEventReset(ze_command_list_append_event_reset_params_t* params, void* global_data, void** instance_data) {

    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

    if (uni_batchqkt_in_batch_) {
      // E6-v4: the batched reset flush (FlushGraphEventResets ->
      // BatchQktResetEvents) appending the parked graph events to the
      // collector's own immediate list. The flag is thread-local and only set
      // inside a batch window, so this can only be our own append: skipping
      // the work below avoids an out-of-order-immediate-list warning plus a
      // submission walk under command_lists_mutex_ per event — the per-event
      // tax this batch exists to remove. Application calls (any other thread,
      // or the same thread outside a batch window, or the gate off) are
      // unaffected.
      return;
    }

    if (!(collector->reset_event_on_device_)) {
      return;
    }

    collector->command_lists_mutex_.lock();
    auto it = collector->command_lists_.find(*(params->phCommandList));
    if (it != collector->command_lists_.end()) {
      if (it->second->immediate_) {
        if (!it->second->in_order_) {
          std::cerr << "[WARNING] Event reset in out-of-order immediate command list" << std::endl;
        }
        // handle immediate command list
        global_device_submissions_mutex_.lock_shared();
        auto it_cmd = local_device_submissions_.commands_submitted_.begin();
        bool process_commands = false;
        while (it_cmd != local_device_submissions_.commands_submitted_.end()) {
          ZeCommand *command = *it_cmd;
          if (command->command_list_ == *(params->phCommandList)) {
            // check if reset event is associated with any command
            if (command->event_ == *(params->phEvent)) {
                // found a command associated with this event
                // wait for the event to be signaled before processing commands
                auto status = ZE_FUNC(zeEventHostSynchronize)(command->event_, UINT64_MAX);
                if (status != ZE_RESULT_SUCCESS) {
                  process_commands = false;
                  std::cerr << "[ERROR] Failed to synchronize event when tracing event reset on device" << std::endl;
                  break;
                }
                process_commands = true;
            }
          }
          ++it_cmd;
        }
        if (process_commands) {
          // Associated commands found for immediate command list, process them
          collector->ProcessCommandsSubmitted(nullptr);
        }
        global_device_submissions_mutex_.unlock_shared();
      }
    }

    if ((it != collector->command_lists_.end()) && !(it->second->immediate_)) {
      auto it2 = it->second->event_to_timestamp_seq_.find(*(params->phEvent));
      if (it2 != it->second->event_to_timestamp_seq_.end()) {
        int slot = it->second->num_timestamps_on_event_reset_++;
        it->second->index_timestamps_on_event_reset_[it2->second] = slot;
        ze_kernel_timestamp_result_t *ts = nullptr;
        size_t slice = slot / number_timestamps_per_slice_;
        if (it->second->timestamps_on_event_reset_.size() <= slice) {
          ze_host_mem_alloc_desc_t host_alloc_desc = {ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC, nullptr, 0};
          auto status = ZE_FUNC(zeMemAllocHost)(it->second->context_, &host_alloc_desc, number_timestamps_per_slice_ * sizeof(ze_kernel_timestamp_result_t), cache_line_size_, (void **)&ts);
          UniMemory::ExitIfOutOfMemory((void *)(ts));
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Failed to allocate host memory for timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
            exit(-1);
          }
          it->second->timestamps_on_event_reset_.push_back(ts);
        }
        else {
          ts = it->second->timestamps_on_event_reset_[slice];
        }
        int idx = slot % number_timestamps_per_slice_;
        auto status = ZE_FUNC(zeCommandListAppendQueryKernelTimestamps)(*(params->phCommandList), 1, (ze_event_handle_t *)(params->phEvent), (void *)&(ts[idx]), nullptr, nullptr, 1, (ze_event_handle_t *)(params->phEvent));
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[ERROR] Failed to get kernel timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          exit(-1);
        }
        it->second->event_to_timestamp_seq_.erase(it2);
      }

      if (UniController::IsCollectionEnabled()) {
        // each command or kernel needs two slots: one for start and one for end
        uint64_t *dts = nullptr;
        size_t slice = it->second->num_device_global_timestamps_ / (2 * number_timestamps_per_slice_);
        if (it->second->device_global_timestamps_.size() <= slice) {
          ze_host_mem_alloc_desc_t host_alloc_desc = {ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC, nullptr, 0};
          auto status = ZE_FUNC(zeMemAllocHost)(it->second->context_, &host_alloc_desc, number_timestamps_per_slice_ * sizeof(uint64_t) * 2, cache_line_size_, (void **)&dts);
          UniMemory::ExitIfOutOfMemory((void *)(dts));
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Failed to allocate host memory for timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
            exit(-1);
          }
          it->second->device_global_timestamps_.push_back(dts);
        }
        else {
          dts = it->second->device_global_timestamps_.at(slice);
        }
        int idx = it->second->num_device_global_timestamps_ % (2 * number_timestamps_per_slice_);
        auto status = ZE_FUNC(zeCommandListAppendWriteGlobalTimestamp)(*(params->phCommandList), (uint64_t *)&(dts[idx]), nullptr, 0, nullptr);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[ERROR] Failed to get device global timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
          exit(-1);
        }

        collector->PrepareToAppendKernelCommand(it->second);

        *instance_data = reinterpret_cast<void *>(&(dts[idx]) + 1);
        it->second->num_device_global_timestamps_ += 2; // start timestamp and end timestamp
      }
      else {
        *instance_data = nullptr;
      }
    }
    collector->command_lists_mutex_.unlock();
  }

  static void OnExitCommandListAppendEventReset(
      ze_command_list_append_event_reset_params_t* params, ze_result_t result, void* global_data,
      void** instance_data, std::vector<uint64_t> *kids) {

    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      if (uni_batchqkt_in_batch_) {
        return;  // E6-v4: our own batched reset append — nothing to capture (see OnEnter)
      }

      if (!(collector->reset_event_on_device_)) {
        return;
      }

      collector->command_lists_mutex_.lock();
      auto it = collector->command_lists_.find(*(params->phCommandList));
      if ((it != collector->command_lists_.end()) && !(it->second->immediate_)) {
        // TODO: handle immediate command list?
        uint64_t *dts = (*((uint64_t **)instance_data));
        if (dts != nullptr) {
          auto status = ZE_FUNC(zeCommandListAppendWriteGlobalTimestamp)(*(params->phCommandList), (uint64_t *)(dts), nullptr, 0, nullptr);
          if (status != ZE_RESULT_SUCCESS) {
            std::cerr << "[ERROR] Failed to get device global timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
            exit(-1);
          }
          collector->AppendCommand(EventReset, it->second, kids, dts);
        }
      }
      collector->command_lists_mutex_.unlock();
    }
  }

  static void OnExitCommandListAppendSignalEvent(
      ze_command_list_append_signal_event_params_t* params,
      ze_result_t /* result */, void* global_data, void** /* instance_data */) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (UniController::IsCollectionEnabled() ||
        collector->IsCommandListGraphCapturing(*(params->phCommandList))) {
      // Track this as a potential fork signal event during graph capture
      std::shared_lock<std::shared_mutex> lock(collector->command_lists_mutex_);
      auto it = collector->command_lists_.find(*(params->phCommandList));
      if (it != collector->command_lists_.end() && it->second->graph_capturing_) {
        ZeGraph* graph = it->second->graph_capture_target_;
        if (graph != nullptr) {
          graph->event_to_cmdlist_[*(params->phEvent)] = *(params->phCommandList);
        }
      }
    }
  }

  static void OnEnterCommandListAppendWaitOnEvents(
      ze_command_list_append_wait_on_events_params_t* params,
      void* global_data, void** /* instance_data */) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    if (collector->ShouldCaptureAppend(*(params->phCommandList),
                                       *(params->pnumEvents),
                                       *(params->pphEvents))) {
      // Check for fork/join transitions based on wait events
      collector->CheckAndApplyForkTransition(
          *(params->phCommandList),
          *(params->pnumEvents),
          *(params->pphEvents));
    }
  }

  static void OnExitCommandListCreate(
      ze_command_list_create_params_t* params,
      ze_result_t result,
      void* global_data,
      void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      // dummy engine ordinal and index
      bool in_order = ((*(params->pdesc))->flags & ZE_COMMAND_LIST_FLAG_IN_ORDER) != 0;
      collector->CreateCommandList( **(params->pphCommandList), *(params->phContext), *(params->phDevice), -1, -1, false, in_order);
    }
  }

  static void OnExitCommandListCreateImmediate(
      ze_command_list_create_immediate_params_t* params,
      ze_result_t result,
      void* global_data,
      void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(**params->pphCommandList != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      ze_device_handle_t* hDevice = params->phDevice;
      if (hDevice == nullptr) {
        return;
      }

      const ze_command_queue_desc_t* clq_desc = *params->paltdesc;
      if (clq_desc == nullptr) {
        return;
      }

      ze_command_list_handle_t*  command_list = *params->pphCommandList;
      if (command_list == nullptr) {
        return;
      }

      bool in_order = ((*(params->paltdesc))->flags & ZE_COMMAND_QUEUE_FLAG_IN_ORDER) != 0;
      collector->CreateCommandList(**(params->pphCommandList), *(params->phContext), *(params->phDevice), clq_desc->ordinal, clq_desc->index, true, in_order);
    }
  }

  static void OnExitCommandListDestroy(
    ze_command_list_destroy_params_t* params,
    ze_result_t result, void* global_data, void** /* instance_data */) {

    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*params->phCommandList != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmitted(nullptr);
      collector->DestroyCommandList(*params->phCommandList);
    }
  }

  static void OnEnterCommandListClose(ze_command_list_close_params_t* params, void* global_data, void** /* instance_data */) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

    collector->command_lists_mutex_.lock();
    auto it = collector->command_lists_.find(*(params->phCommandList));
    if (it != collector->command_lists_.end()) {
      int num_events = it->second->event_to_timestamp_seq_.size();
      if (num_events) {
        // HARDEN: a failed event-pool creation at list creation time (reported
        // there) leaves the timestamp event null -- close this list
        // uninstrumented instead of appending null signal/query events.
        if (it->second->timestamp_event_to_signal_ == nullptr) {
          std::cerr << "[ERROR] No timestamp event for command list, closing uninstrumented" << std::endl;
        }
        else {
        std::vector<ze_event_handle_t> events(num_events);

        int i = 0;
        for (auto it2 = it->second->event_to_timestamp_seq_.begin(); it2 != it->second->event_to_timestamp_seq_.end(); it2++, i++) {
          events[i] = it2->first;
          it->second->index_timestamps_on_commands_completion_[it2->second] = i;
        }

        ze_kernel_timestamp_result_t *ts = nullptr;

        ze_host_mem_alloc_desc_t host_alloc_desc = {ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC, nullptr, 0};
        auto status = ZE_FUNC(zeMemAllocHost)(it->second->context_, &host_alloc_desc, i * sizeof(ze_kernel_timestamp_result_t), cache_line_size_, (void **)&ts);
        UniMemory::ExitIfOutOfMemory((void *)(ts));
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[ERROR] Failed to allocate host memory for timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
        }
        it->second->timestamps_on_commands_completion_ = ts;

        if (it->second->in_order_) {
          // WA for driver bug. If command list is in order avoid signaling event
          // in zeCommandListAppendQueryKernelTimestamps.
          status = ZE_FUNC(zeCommandListAppendQueryKernelTimestamps)(*(params->phCommandList), num_events, events.data(), (void *)it->second->timestamps_on_commands_completion_, nullptr, nullptr, num_events, events.data());
          if (status == ZE_RESULT_SUCCESS)
            status = ZE_FUNC(zeCommandListAppendSignalEvent)(*(params->phCommandList), it->second->timestamp_event_to_signal_);
        } else {
          status = ZE_FUNC(zeCommandListAppendQueryKernelTimestamps)(*(params->phCommandList), num_events, events.data(), (void *)it->second->timestamps_on_commands_completion_, nullptr, it->second->timestamp_event_to_signal_, num_events, events.data());
        }

        if (status != ZE_RESULT_SUCCESS){
          std::cerr << "[ERROR] Failed to get kernel timestamps (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
        }
        }
      }
      else if (it->second->timestamp_event_to_signal_ != nullptr) {
        // signal event if events were reset earlier
        auto status = ZE_FUNC(zeCommandListAppendSignalEvent)(*(params->phCommandList), it->second->timestamp_event_to_signal_);
        if (status != ZE_RESULT_SUCCESS){
          std::cerr << "[ERROR] Failed to signal command list timestamps event (status = 0x" << std::hex << status << std::dec << ")" << std::endl;
        }
      }

      if (!it->second->event_to_timestamp_seq_.empty()) {
        it->second->event_to_timestamp_seq_.clear();
      }
    }
    collector->command_lists_mutex_.unlock();
  }

  static void OnExitCommandListReset(ze_command_list_reset_params_t* params, ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*params->phCommandList != nullptr);
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessCommandsSubmitted(nullptr);
      collector->ResetCommandList(*params->phCommandList);
    }
  }

  static void OnEnterCommandQueueExecuteCommandLists(
      ze_command_queue_execute_command_lists_params_t* params,
      void* global_data, void** /* instance_data */) {

    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

    if (UniController::IsCollectionEnabled()) {
      uint32_t count = *params->pnumCommandLists;
      if (count == 0) {
        return;
      }

      ze_command_list_handle_t* cmdlists = *params->pphCommandLists;
      if (cmdlists == nullptr) {
        return;
      }

      if (local_device_submissions_.IsFinalized()) {
        return;
      }

      ze_command_queue_handle_t queue = *(params->phCommandQueue);
      collector->PrepareToExecuteCommandLists(cmdlists, count, queue, *(params->phFence));
    }
  }

  static void OnExitCommandQueueExecuteCommandLists(
      ze_command_queue_execute_command_lists_params_t* /* params */,
      ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {

    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

      if (UniController::IsCollectionEnabled()) {
        local_device_submissions_.SubmitStagedKernelCommandAndMetricQueries(collector->event_cache_, kids);
      }
    }
    else {
      local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
    }
  }

  static void OnExitCommandQueueSynchronize(
    ze_command_queue_synchronize_params_t* /* params */,
    ze_result_t result, void* global_data, void** /* instance_data */, std::vector<uint64_t> *kids) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessAllCommandsSubmitted(kids);
    }
  }

  static void OnExitCommandQueueCreate(ze_command_queue_create_params_t* params, ze_result_t /* result */, void* global_data, void** /* instance_data */) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    ze_device_handle_t* device = params->phDevice;
    if (device == nullptr) {
      return;
    }
    const ze_command_queue_desc_t* queue_desc = *params->pdesc;
    if (queue_desc == nullptr) {
      return;
    }
    ze_command_queue_handle_t*  command_queue = *params->pphCommandQueue;
    if (command_queue == nullptr) {
      return;
    }

    ZeCommandQueue desc;
    desc.queue_ = *command_queue;
    desc.context_ = *(params->phContext);
    desc.device_ = *device;
    desc.engine_ordinal_ = queue_desc->ordinal;
    desc.engine_index_ = queue_desc->index;;

    collector->command_queues_mutex_.lock();
    collector->command_queues_.erase(*command_queue);
    collector->command_queues_.insert({*command_queue, std::move(desc)});
    collector->command_queues_mutex_.unlock();
  }

  static void OnExitCommandQueueDestroy(ze_command_queue_destroy_params_t* params, ze_result_t result,
    void* global_data, void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      collector->ProcessAllCommandsSubmitted(nullptr);
      collector->command_queues_mutex_.lock();
      collector->command_queues_.erase(*params->phCommandQueue);
      collector->command_queues_mutex_.unlock();
    }
  }

  static void OnExitModuleCreate(ze_module_create_params_t* params, ze_result_t result, void* /* global_data */, void** /* instance_user_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ze_module_handle_t mod = **(params->pphModule);
      ze_device_handle_t device = *(params->phDevice);
      size_t binary_size;
      if (ZE_FUNC(zeModuleGetNativeBinary)(mod, &binary_size, nullptr) != ZE_RESULT_SUCCESS) {
        binary_size = (size_t)(-1);
      }

      ZeModule m;

      m.device_ = device;
      m.size_ = binary_size;
      m.aot_ = (*(params->pdesc))->format;

      modules_on_devices_mutex_.lock();
      modules_on_devices_.insert({mod, std::move(m)});
      modules_on_devices_mutex_.unlock();
    }
  }

  static void OnEnterModuleDestroy(ze_module_destroy_params_t* params, void* /* global_data */, void** /* instance_user_data */) {
    ze_module_handle_t mod = *(params->phModule);
    modules_on_devices_mutex_.lock();
    modules_on_devices_.erase(mod);
    modules_on_devices_mutex_.unlock();
  }

  static void OnEnterCommandListImmediateAppendCommandListsExp(
      ze_command_list_immediate_append_command_lists_exp_params_t* params,
      void* global_data, void** /* instance_data */) {

    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

    if (UniController::IsCollectionEnabled()) {
      collector->command_lists_mutex_.lock_shared();

      auto it = collector->command_lists_.find(*(params->phCommandListImmediate));
      if (it != collector->command_lists_.end()) {
        collector->PrepareToExecuteCommandListsLocked(*(params->pphCommandLists), *(params->pnumCommandLists),
                                                it->second->device_, it->second->engine_ordinal_, it->second->engine_index_, nullptr);
      }
      collector->command_lists_mutex_.unlock_shared();
    }
  }

  static void OnExitCommandListImmediateAppendCommandListsExp(
    ze_command_list_immediate_append_command_lists_exp_params_t* /* params */,
    ze_result_t result,
    void* global_data,
    void** /* instance_data */, std::vector<uint64_t> *kids) {
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);

    if (UniController::IsCollectionEnabled()) {
        if (result == ZE_RESULT_SUCCESS) {
           local_device_submissions_.SubmitStagedKernelCommandAndMetricQueries(collector->event_cache_, kids);
        }
        else {
           local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
        }
    }
  }

#if !defined(ZEX_STRUCTURE_KERNEL_REGISTER_FILE_SIZE_EXP)

#define ZEX_STRUCTURE_KERNEL_REGISTER_FILE_SIZE_EXP (ze_structure_type_t)0x00030012
typedef struct _zex_kernel_register_file_size_exp_t {
    ze_structure_type_t stype = ZEX_STRUCTURE_KERNEL_REGISTER_FILE_SIZE_EXP; ///< [in] type of this structure
    const void *pNext = nullptr;                                             ///< [in, out][optional] pointer to extension-specific structure
    uint32_t registerFileSize;                                               ///< [out] Register file size used in kernel
} zex_kernel_register_file_size_exp_t;

#endif /* !defined(ZEX_STRUCTURE_KERNEL_REGISTER_FILE_SIZE_EXP) */

  static void OnExitKernelCreate(ze_kernel_create_params_t *params, ze_result_t result, void* global_data, void** /* instance_user_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      ze_kernel_handle_t kernel = **(params->pphKernel);

      ze_module_handle_t mod = *(params->phModule);
      ze_device_handle_t device = nullptr;
      size_t module_binary_size = (size_t)(-1);
      bool aot = false;
      modules_on_devices_mutex_.lock_shared();
      auto mit = modules_on_devices_.find(mod);
      if (mit != modules_on_devices_.end()) {
        device = mit->second.device_;
        module_binary_size = mit->second.size_;
        aot = mit->second.aot_;
      }
      modules_on_devices_mutex_.unlock_shared();

      int did = -1;
      if (device != nullptr) {
        devices_mutex_.lock_shared();
        auto dit = devices_->find(device);
        if (dit != devices_->end()) {
          did = dit->second.id_;
        }
        devices_mutex_.unlock_shared();
      }
      kernel_command_properties_mutex_.lock();

      auto it = active_kernel_properties_->find(kernel);
      if (it != active_kernel_properties_->end()) {
        active_kernel_properties_->erase(it);
      }

      ZeKernelCommandProperties desc;

      desc.type_ = KERNEL_COMMAND_TYPE_COMPUTE;
      desc.aot_ = aot;

      ze_result_t status;

      desc.id_ = UniKernelId::GetKernelId();

      if ((*(params->pdesc) != nullptr) && ((*(params->pdesc))->pKernelName != nullptr)) {
        desc.name_ = std::string((*(params->pdesc))->pKernelName);
      }
      else {
        // try one more time
        size_t kname_size = 0;
        status = ZE_FUNC(zeKernelGetName)(kernel, &kname_size, nullptr);
        if ((status == ZE_RESULT_SUCCESS) && (kname_size > 0)) {
          char* kname = (char*) malloc(kname_size);
          if (kname != nullptr) {
            status = ZE_FUNC(zeKernelGetName)(kernel, &kname_size, kname);
            PTI_ASSERT(status == ZE_RESULT_SUCCESS);
            desc.name_ = std::string(kname);
            free(kname);
          }
          else {
            desc.name_ = "UnknownKernel";
          }
        }
        else {
          desc.name_ = "UnknownKernel";
        }
      }

      desc.device_id_ = did;
      desc.device_ = device;

      ze_kernel_properties_t kprops{};

      zex_kernel_register_file_size_exp_t regsize{ZEX_STRUCTURE_KERNEL_REGISTER_FILE_SIZE_EXP, nullptr, 0};
      kprops.pNext = (void *)&regsize;

      status = ZE_FUNC(zeKernelGetProperties)(kernel, &kprops);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      desc.simd_width_ = kprops.maxSubgroupSize;
      desc.nargs_ = kprops.numKernelArgs;
      desc.nsubgrps_ = kprops.maxNumSubgroups;
      desc.slmsize_ = kprops.localMemSize;
      desc.private_mem_size_ = kprops.privateMemSize;
      desc.spill_mem_size_ = kprops.spillMemSize;
      ZeKernelGroupSize group_size{kprops.requiredGroupSizeX, kprops.requiredGroupSizeY, kprops.requiredGroupSizeZ};
      desc.group_size_ = group_size;
      desc.regsize_ = regsize.registerFileSize;

      // for stall sampling
      uint64_t base_addr = 0;
      uint64_t binary_size = 0;
      if (collector->options_.stall_sampling && (ZexKernelGetBaseAddress != nullptr) && (ZexKernelGetBaseAddress(kernel, &base_addr) == ZE_RESULT_SUCCESS)) {
        base_addr &= 0xFFFFFFFF;
        binary_size = module_binary_size;  // store module binary size. only an upper bound is needed
      }

      desc.base_addr_ = base_addr;
      desc.size_ = binary_size;

      // First, check include_kernels_ to see if the kernel should be included (if any match, skip=false).
      // Then, check exclude_kernels_ to see if the kernel should be excluded (if any match, skip=true).
      desc.skip_ = false;
      if (!collector->include_kernels_.empty() || !collector->exclude_kernels_.empty()) {
        std::string demangled_name = utils::Demangle(desc.name_.c_str());
        if (!collector->include_kernels_.empty()) {
          desc.skip_ = true;
          for (const auto& filter : collector->include_kernels_) {
            if (!filter.empty() && demangled_name.find(filter) != std::string::npos) {
              desc.skip_ = false;
              break;
            }
          }
        }

        if (!collector->exclude_kernels_.empty() && desc.skip_ == false) {
          for (const auto& filter : collector->exclude_kernels_) {
            if (!filter.empty() && demangled_name.find(filter) != std::string::npos) {
              desc.skip_ = true;
              break;
            }
          }
        }
      }


      ZeKernelCommandProperties desc2 = desc;
      active_kernel_properties_->insert({kernel, std::move(desc)});
      kernel_command_properties_->insert({desc2.id_, std::move(desc2)});

      kernel_command_properties_mutex_.unlock();
    }
  }

  static void OnExitKernelSetGroupSize(ze_kernel_set_group_size_params_t* params, ze_result_t result,
    void* /* global_data */, void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      if (UniController::IsCollectionEnabled()) {
        ZeKernelGroupSize group_size{*(params->pgroupSizeX), *(params->pgroupSizeY), *(params->pgroupSizeZ)};
        kernel_command_properties_mutex_.lock();

        auto it = active_kernel_properties_->find(*(params->phKernel));
        PTI_ASSERT(it != active_kernel_properties_->end());
        if ((it->second.group_size_.x != group_size.x) || (it->second.group_size_.y != group_size.y) ||
          (it->second.group_size_.z != group_size.z)) {
          // new group size
          it->second.group_size_ = group_size;
          auto it2 = kernel_command_properties_->find(it->second.id_);
          if ((it2 != kernel_command_properties_->end()) &&
            (it2->second.group_size_.x == group_size.x) &&
            (it2->second.group_size_.y == group_size.y) &&
            (it2->second.group_size_.z == group_size.z)) {
            // group size was used before
            it->second.id_ = it2->second.id_;
          }
          else {
            // first time use the group size
            it->second.id_ = UniKernelId::GetKernelId();
            ZeKernelCommandProperties desc2 = it->second;
            kernel_command_properties_->insert({desc2.id_, std::move(desc2)});
          }
        }
        else {
          // do nothing
        }

        kernel_command_properties_mutex_.unlock();
      }
    }
  }

  static void OnExitKernelDestroy(ze_kernel_destroy_params_t* params, ze_result_t result, void* /* global_data */, void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS) {
      kernel_command_properties_mutex_.lock();
      active_kernel_properties_->erase(*(params->phKernel));
      kernel_command_properties_mutex_.unlock();
    }
  }

  // HARDEN (pairing review): runs BEFORE the driver destroys the context --
  // the only point where the collector's per-context allocations can be
  // legally freed (lists/buffers/events/pools). Mirrors the pattern of
  // OnEnterModuleDestroy.
  static void OnEnterContextDestroy(ze_context_destroy_params_t* params, void* global_data, void** /* instance_data */) {
    if (global_data != nullptr && params->phContext != nullptr) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      ze_context_handle_t context = *(params->phContext);
      // Paired teardown of the BATCHQKT lists/buffers (marks the handle
      // dead), then of the event cache pools/events for this context.
      collector->ReleaseBatchQktContextResources(context);
      collector->event_cache_.ReleaseContext(context);
    }
  }

  static void OnExitContextDestroy(ze_context_destroy_params_t* params, ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result == ZE_RESULT_SUCCESS && params->phContext != nullptr) {
      ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
      // HARDEN: the driver has ALREADY destroyed the context when this exit
      // callback runs. If the enter intercept above fired, every BATCHQKT
      // map entry is already freed and this only re-marks the handle as
      // dead; if it did not fire, this drops the resources from the maps
      // (unfreeable at this point) so the teardown drain below cannot
      // program device work on the dead context and the handle value can
      // never be served to a recycled context.
      collector->InvalidateBatchQktContext(*(params->phContext));
      collector->ProcessAllCommandsSubmitted(nullptr);
      collector->event_cache_.ReleaseContext(*(params->phContext));
    }
  }

  static void OnExitGraphCreateExp(
      ze_graph_create_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    // Graph tracking entry is created lazily in OnBeginGraphCapture.
  }

  static void OnExitGraphDestroyExp(
      ze_graph_destroy_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnGraphDestroy(*(params->phGraph));
  }

  static void OnExitExecutableGraphDestroyExp(
      ze_executable_graph_destroy_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnExecutableGraphDestroy(*(params->phGraph));
  }

  static void OnExitCommandListBeginGraphCaptureExp(
      ze_command_list_begin_graph_capture_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnBeginGraphCaptureNoGraph(*(params->phCommandList));
  }

  static void OnExitCommandListBeginCaptureIntoGraphExp(
      ze_command_list_begin_capture_into_graph_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnBeginGraphCapture(*(params->phCommandList), *(params->phGraph));
  }

  static void OnExitCommandListEndGraphCaptureExp(
      ze_command_list_end_graph_capture_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnEndGraphCapture(*(params->phCommandList), **(params->pphGraph));
  }

  static void OnExitCommandListInstantiateGraphExp(
      ze_command_list_instantiate_graph_exp_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnInstantiateGraph(*(params->phGraph), **(params->pphExecutableGraph));
  }

  // Bodies shared by the Exp and Ext variants of zeCommandListAppendGraph.
  static void StageGraphExecution(void* global_data, ze_command_list_handle_t command_list,
                                  ze_executable_graph_handle_t graph) {
    if (!UniController::IsCollectionEnabled()) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->PrepareGraphExecution(command_list, graph);
  }

  static void SubmitGraphExecution(ze_result_t result, void* global_data,
                                   std::vector<uint64_t> *kids) {
    if (result != ZE_RESULT_SUCCESS || !UniController::IsCollectionEnabled()) {
      local_device_submissions_.RevertStagedKernelCommandAndMetricQueries();
      return;
    }
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    // kids let the host-side flow callback emit FLOW_H2D to each replayed kernel.
    local_device_submissions_.SubmitStagedKernelCommandAndMetricQueries(collector->event_cache_, kids);
  }

  static void OnEnterCommandListAppendGraphExp(
      ze_command_list_append_graph_exp_params_t* params,
      void* global_data, void** /* instance_data */) {
    StageGraphExecution(global_data, *(params->phCommandList), *(params->phGraph));
  }

  static void OnExitCommandListAppendGraphExp(
      ze_command_list_append_graph_exp_params_t* /* params */,
      ze_result_t result, void* global_data, void** /* instance_data */,
      std::vector<uint64_t> *kids) {
    SubmitGraphExecution(result, global_data, kids);
  }

  // Stable (Ext) graph APIs, which SYCL uses when the driver reports them. They
  // carry the same handles as the Exp ones, so they feed the same bookkeeping.

  static void OnExitCommandListBeginGraphCaptureExt(
      ze_command_list_begin_graph_capture_ext_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnBeginGraphCaptureNoGraph(*(params->phCommandList));
  }

  static void OnExitCommandListBeginCaptureIntoGraphExt(
      ze_command_list_begin_capture_into_graph_ext_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnBeginGraphCapture(*(params->phCommandList), *(params->phGraph));
  }

  static void OnExitCommandListEndGraphCaptureExt(
      ze_command_list_end_graph_capture_ext_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnEndGraphCapture(*(params->phCommandList), **(params->pphGraph));
  }

  static void OnExitGraphInstantiateExt(
      ze_graph_instantiate_ext_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnInstantiateGraph(*(params->phGraph), **(params->pphExecutableGraph));
  }

  static void OnExitGraphDestroyExt(
      ze_graph_destroy_ext_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnGraphDestroy(*(params->phGraph));
  }

  static void OnExitExecutableGraphDestroyExt(
      ze_executable_graph_destroy_ext_params_t* params,
      ze_result_t result, void* global_data, void** /* instance_data */) {
    if (result != ZE_RESULT_SUCCESS) return;
    ZeCollector* collector = reinterpret_cast<ZeCollector*>(global_data);
    collector->OnExecutableGraphDestroy(*(params->phGraph));
  }

  static void OnEnterCommandListAppendGraphExt(
      ze_command_list_append_graph_ext_params_t* params,
      void* global_data, void** /* instance_data */) {
    StageGraphExecution(global_data, *(params->phCommandList), *(params->phGraph));
  }

  static void OnExitCommandListAppendGraphExt(
      ze_command_list_append_graph_ext_params_t* /* params */,
      ze_result_t result, void* global_data, void** /* instance_data */,
      std::vector<uint64_t> *kids) {
    SubmitGraphExecution(result, global_data, kids);
  }

  #include <tracing.gen> // Auto-generated callbacks

  void CollectHostFunctionTimeStats(uint32_t id, uint64_t time) {
    local_device_submissions_.CollectHostFunctionTimeStats(id, time);
  }

  void AggregateDeviceTimeStats() const {
    // do not acquire global_device_time_stats_mutex_. caller does it.
    for (auto it = global_device_time_stats_->begin(); it != global_device_time_stats_->end(); it++) {
      std::string kname;
      if (it->first.tile_ >= 0) {
        kname = "Tile #" + std::to_string(it->first.tile_) + ": " + GetZeKernelCommandName(it->first.kernel_command_id_, it->first.group_count_, it->first.mem_size_, options_.verbose);
      }
      else {
        kname = GetZeKernelCommandName(it->first.kernel_command_id_, it->first.group_count_, it->first.mem_size_, options_.verbose);
      }

      auto it2 = it;
      it2++;

      for (; it2 != global_device_time_stats_->end();) {
        std::string kname2;
        if (it2->first.tile_ >= 0) {
          kname2 = "Tile #" + std::to_string(it2->first.tile_) + ": " + GetZeKernelCommandName(it2->first.kernel_command_id_, it2->first.group_count_, it2->first.mem_size_, options_.verbose);
        }
        else {
          kname2 = GetZeKernelCommandName(it2->first.kernel_command_id_, it2->first.group_count_, it2->first.mem_size_, options_.verbose);
        }

        if (kname2 == kname) {
          it->second.append_time_ += it2->second.append_time_;
          it->second.submit_time_ += it2->second.submit_time_;
          it->second.execute_time_ += it2->second.execute_time_;
          if (it->second.min_time_ > it2->second.min_time_) {
            it->second.min_time_ = it2->second.min_time_;
          }
          if (it->second.max_time_ < it2->second.max_time_) {
            it->second.max_time_ = it2->second.max_time_;
          }
          it->second.call_count_ += it2->second.call_count_;
          it2 = global_device_time_stats_->erase(it2);
        }
        else {
          it2++;
        }
      }
    }
  }

  static std::vector<std::string> ParseFilterList(const std::string& file, const std::string& filter) {
    std::vector<std::string> result;

    // Helper lambda to split a line by comma, supporting double quotes
    auto split_by_comma_with_quotes = [](const std::string& line, std::vector<std::string>& out) {
      size_t i = 0;
      while (i < line.size()) {
        // Skip leading whitespace
        while (i < line.size() && isspace(line[i])) ++i;
        if (i >= line.size()) break;
        std::string item;
        if (line[i] == '"') {
          // Quoted item
          ++i;
          while (i < line.size()) {
            if (line[i] == '"') {
              ++i;
              break;
            }
            // Support escaped quotes
            if (line[i] == '\\' && i + 1 < line.size() && line[i+1] == '"') {
              item += '"';
              i += 2;
              continue;
            }
            item += line[i++];
          }
        } else {
          // Unquoted item
          while (i < line.size() && line[i] != ',') {
            item += line[i++];
          }
        }
        // Skip trailing whitespace
        size_t start = 0, end = item.size();
        while (start < end && isspace(item[start])) ++start;
        while (end > start && isspace(item[end-1])) --end;
        if (start < end) out.push_back(item.substr(start, end-start));
        // Skip comma
        if (i < line.size() && line[i] == ',') ++i;
      }
    };

    // First, gather from file if provided
    if (!file.empty()) {
      std::ifstream fin(file);
      if (fin.good()) {
        std::string line;
        while (std::getline(fin, line)) {
          split_by_comma_with_quotes(line, result);
        }
      }
    }

    // Then, gather from string if provided
    if (!filter.empty()) {
      split_by_comma_with_quotes(filter, result);
    }

    return result;
  }

 private: // Data
  LoggerFactory* logger_factory_;
  std::shared_ptr<Logger> logger_;
  std::shared_ptr<Logger> logger_device_timeline_;
  CollectorOptions options_;
  OnZeKernelFinishCallback kcallback_ = nullptr;
  OnZeFunctionFinishCallback fcallback_ = nullptr;
  OnZeMetaRecordCallback mcallback_ = nullptr;  // T14/A5 self-annotation
  bool reset_event_on_device_; // support event reset on device
  ZeEventCache event_cache_;

  zel_tracer_handle_t tracer_ = nullptr;

  mutable std::shared_mutex images_mutex_;
  std::map<ze_image_handle_t, size_t> images_;


  mutable std::shared_mutex command_queues_mutex_;
  std::map<ze_command_queue_handle_t, ZeCommandQueue> command_queues_;

  mutable std::shared_mutex command_lists_mutex_;
  std::map<ze_command_list_handle_t, ZeCommandList *> command_lists_;

  std::set<std::pair<ze_context_handle_t, ze_device_handle_t>> metric_activations_;

  ZeMetricQueryPools query_pools_;

  std::vector<ze_context_handle_t> metric_contexts_;

  mutable std::shared_mutex events_mutex_;
  std::set<ze_event_pool_handle_t> counter_based_pools_;
  std::set<ze_event_handle_t> counter_based_events_;
  std::map<ze_event_handle_t, ZeEventHistory> event_history_;  // TSLOG v2 per-event signal/reset history (guarded by events_mutex_)

  // Graph replay fix: events whose pending clones have been processed but whose
  // packet reset is deferred to the end of the sweep, so that all clones sharing
  // the event can read the packet before it is erased (guarded by events_mutex_).
  // E6-v4: the sweeps park the shared events here per owning context
  // (EventHistoryDeferGraphEventReset) and FlushGraphEventResets resets each
  // context's set as ONE device-side batch (BatchQktResetEvents). With the
  // batch gate off nothing is ever inserted, so the flush keeps the legacy
  // per-event host reset.
  std::map<ze_context_handle_t, std::set<ze_event_handle_t>> graph_events_pending_reset_;

  constexpr static size_t kCallsLength = 12;
  constexpr static size_t kTimeLength = 20;

  std::string data_dir_name_;
  std::vector<std::string> include_kernels_;
  std::vector<std::string> exclude_kernels_;

  // ---- Fix B: background completer (UNITRACE_DEFERRED_TS=1) ----
  std::mutex uni_defer_q_mutex_;
  std::condition_variable uni_defer_q_cv_;
  std::deque<DeferredTsItem> uni_defer_queue_;
  std::thread uni_defer_worker_;
  std::atomic<bool> uni_defer_ts_enabled_{false};
  std::atomic<bool> uni_defer_worker_started_{false};
  std::atomic<bool> uni_defer_worker_stop_{false};
  std::atomic<bool> uni_defer_worker_joined_{false};
  bool uni_defer_worker_busy_ = false;  // guarded by uni_defer_q_mutex_
  // Diagnostics (relaxed atomics; diagnostics only, env UNITRACE_DEBUG_DEFER)
  std::atomic<uint64_t> uni_defer_handoff_count_{0};
  std::atomic<uint64_t> uni_defer_processed_count_{0};
  std::atomic<uint64_t> uni_defer_inline_fallback_count_{0};
  std::atomic<uint64_t> uni_defer_flush_count_{0};
  std::atomic<uint64_t> uni_defer_flush_max_wait_us_{0};
  std::atomic<uint64_t> uni_defer_batch_count_{0};
  std::atomic<uint64_t> uni_defer_max_batch_{0};

  // ---- T6'/E6: per-replay batched graph timestamp read (UNITRACE_GRAPH_BATCH_QKT=1) ----
  std::atomic<bool> uni_batchqkt_enabled_{false};
  std::atomic<bool> uni_batchqkt_ts2_notice_{false};  // one-shot TS2/DEBUG_TS2 forced-off notice
  // Serializes the shared immediate command list + staging buffer (batch steps
  // are once-per-replay, so contention is a non-issue).
  std::mutex uni_batchqkt_mutex_;
  std::map<ze_context_handle_t, ze_command_list_handle_t> uni_batchqkt_imm_lists_;  // nullptr = creation failed (sticky fallback)
  std::map<ze_context_handle_t, void *> uni_batchqkt_bufs_;      // per-context host staging buffer
  std::map<ze_context_handle_t, size_t> uni_batchqkt_buf_caps_;  // capacities in bytes
  std::map<ze_device_handle_t, std::pair<uint32_t, bool>> uni_batchqkt_ordinals_;  // device -> {ordinal, has compute}
  // HARDEN: context handle values whose context the app already destroyed.
  // A value here is NEVER trusted again -- L0 heap-reuses handles, so a new
  // context can land on the same address; refusing it only costs a graceful
  // fallback, trusting it puts device work on a dead context (CCS fault).
  std::set<ze_context_handle_t> uni_batchqkt_dead_contexts_;
  // HARDEN: pairing/retry accounting -- per-context resource releases (the
  // paired frees at zeContextDestroy enter) and requests refused because the
  // context handle was already destroyed.
  std::atomic<uint64_t> uni_batchqkt_ctx_releases_{0};
  std::atomic<uint64_t> uni_batchqkt_dead_ctx_skips_{0};
  // HARDEN: number of command lists currently inside a graph capture window
  // (graph_capturing_ == true). Device-side BATCHQKT work is suppressed
  // while > 0: capture appends create no clones, so mid-capture device work
  // can only be leftovers of an earlier replay, and the sweeps that would
  // issue it fire exactly from the list/queue/context teardown intercepts
  // that run between capture pieces.
  std::atomic<int> uni_batchqkt_capture_depth_{0};
  // Diagnostics (relaxed atomics; env UNITRACE_DEBUG_BATCHQKT)
  std::atomic<uint64_t> uni_batchqkt_batch_count_{0};
  std::atomic<uint64_t> uni_batchqkt_cmd_count_{0};
  std::atomic<uint64_t> uni_batchqkt_fallback_count_{0};
  std::atomic<uint64_t> uni_batchqkt_max_n_{0};
  std::atomic<uint64_t> uni_batchqkt_max_batch_us_{0};
  // v2 funnel counters: batch-enabled sweeps that saw graph clones, graph
  // clones seen in them, and clones that joined a batch. Localizes a
  // zero-batch result to the failing stage (see BatchQktAccountSweep).
  std::atomic<uint64_t> uni_batchqkt_sweeps_{0};
  std::atomic<uint64_t> uni_batchqkt_seen_{0};
  std::atomic<uint64_t> uni_batchqkt_collected_{0};
  // One-shot per-reason bail notices (guarded by UNITRACE_DEBUG_BATCHQKT)
  std::atomic<bool> uni_batchqkt_notice_min_{false};
  std::atomic<bool> uni_batchqkt_notice_syms_{false};
  std::atomic<bool> uni_batchqkt_notice_ctx_{false};
  std::atomic<bool> uni_batchqkt_notice_mixed_{false};
  std::atomic<bool> uni_batchqkt_notice_notsig_{false};
  // HARDEN (2026-09-30 audit): graceful-skip accounting. alloc_fail counts
  // every batch/batch-read skipped because a collector allocation (host
  // staging buffer, immediate command list) failed or its context handle is
  // known destroyed; capture_skip counts batches suppressed while a graph
  // capture window is open. Both are visible in the [BATCHQKT] summary line.
  std::atomic<bool> uni_batchqkt_notice_allocfail_{false};
  std::atomic<bool> uni_batchqkt_notice_deadctx_{false};
  std::atomic<bool> uni_batchqkt_notice_invariant_{false};
  std::atomic<uint64_t> uni_batchqkt_alloc_fail_{0};
  std::atomic<uint64_t> uni_batchqkt_capture_skip_{0};

  // ---- E6-v4: batched reset of the graph clones' shared events ----
  std::atomic<uint64_t> uni_batchqkt_reset_batch_count_{0};  // device-side reset batches run
  std::atomic<uint64_t> uni_batchqkt_reset_batched_{0};      // events reset through them
  std::atomic<uint64_t> uni_batchqkt_reset_max_{0};          // largest batch (distinct events)
  std::atomic<uint64_t> uni_batchqkt_reset_max_us_{0};       // slowest batch (append+sync)
  std::atomic<uint64_t> uni_batchqkt_reset_fallback_{0};     // events that took the legacy host reset
  std::atomic<bool> uni_batchqkt_notice_resetok_{false};
  std::atomic<bool> uni_batchqkt_notice_resetfail_{false};

  // ---- E6-v4b: split flush — read at the drain, emit at a later sweep ----
  // Fast-path hint that deferred_ts_ clones exist (set by the drain's arm,
  // taken by the sweep that runs the emit pass; the drain's backstop pass
  // ignores it and always scans).
  std::atomic<bool> uni_batchqkt_flush_pending_{false};
  // Packets of the live arm. Only the drain writes it (after its backstop pass
  // consumed the previous arm, so no command can still point into it), and the
  // submission lock serializes that against any emit pass reading it.
  std::vector<ze_kernel_timestamp_result_t> uni_batchqkt_emit_buf_;
  // Where the emit of a clone ran (clones, not flushes): poll sweep = the
  // overlapped path working as designed, drain = the backstop (app skipped
  // polling, or the clones belonged to another thread's list).
  std::atomic<uint64_t> uni_batchqkt_flush_at_poll_{0};
  std::atomic<uint64_t> uni_batchqkt_flush_at_drain_{0};

  // ---- E6-v4c: poll-armed batch read (UNITRACE_GRAPH_QKT_AT_POLL=1) ----
  // Published by BatchQktArmPollRead (release) and consumed by
  // BatchQktCompletePollRead (acquire): the single slot of an armed read.
  // Writers hold a submission lock (shared for the arm, exclusive for the
  // completion), so the descriptor itself needs no lock of its own.
  std::atomic<bool> uni_batchqkt_read_pending_{false};
  ZeBatchQktPollRead uni_batchqkt_poll_read_;
  // Dedicated immediate list + packet buffer for the armed read, per context.
  // Kept separate from the drain's list/buffer so nothing else the collector
  // appends (reset batches, a fallback execute) can ever queue behind a
  // device-side wait, and so a fallback drain execute cannot clobber an armed
  // read's packets.
  std::map<ze_context_handle_t, ze_command_list_handle_t> uni_batchqkt_poll_lists_;
  std::map<ze_context_handle_t, void *> uni_batchqkt_poll_bufs_;
  std::map<ze_context_handle_t, size_t> uni_batchqkt_poll_buf_caps_;
  // Counters (see the [BATCHQKT] summary line). qkt_batch_us is the TOTAL us
  // of the poll-armed read's append+sync across the run, i.e. the cost that
  // left the staging drain; the two halves are counted separately.
  std::atomic<uint64_t> uni_batchqkt_qkt_at_poll_{0};
  std::atomic<uint64_t> uni_batchqkt_qkt_at_drain_{0};
  std::atomic<uint64_t> uni_batchqkt_qkt_batch_us_{0};
  std::atomic<uint64_t> uni_batchqkt_qkt_append_us_{0};
  std::atomic<uint64_t> uni_batchqkt_qkt_sync_us_{0};
  // One-shot per-reason bail notices (guarded by UNITRACE_DEBUG_BATCHQKT)
  std::atomic<bool> uni_batchqkt_notice_polllist_{false};
  std::atomic<bool> uni_batchqkt_notice_pollmixed_{false};
  std::atomic<bool> uni_batchqkt_notice_pollappend_{false};

};

#endif // PTI_TOOLS_UNITRACE_LEVEL_ZERO_COLLECTOR_H_
