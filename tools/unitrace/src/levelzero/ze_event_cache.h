//==============================================================
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#ifndef PTI_TOOLS_ZE_TRACER_ZE_EVENT_CACHE_H_
#define PTI_TOOLS_ZE_TRACER_ZE_EVENT_CACHE_H_

#include <map>
#include <mutex>
#include <vector>

#include "ze_loader.h"

#define EVENT_POOL_SIZE  1024
#define EVENT_INDEX_UNKNOWN  0xFFFFFFFFu

struct ZeEventInfo {
  ze_event_pool_handle_t pool;
  ze_context_handle_t context;
};

class ZeEventCache {
 public:
  ZeEventCache(ze_event_pool_flags_t flags) : flags_(flags) {}

  ZeEventCache(const ZeEventCache& that) = delete;

  ZeEventCache& operator=(const ZeEventCache& that) = delete;

  ~ZeEventCache() {
#ifdef _WIN32
    // on Windows, it is very possible that L0 has been unloaded or is being unloaded at this point and L0 calls may have undefined behavior
    // hence skipping all destroy calls and returning early.
    return;
#else /* _WIN32 */
    bool destroyed = true;
    const std::lock_guard<std::shared_mutex> lock(lock_);

    for (auto& value : event_map_) {
      for (auto event : value.second) {
        ze_result_t status = ZE_RESULT_SUCCESS;
        status = ZE_FUNC(zeEventDestroy)(event);
        if (status != ZE_RESULT_SUCCESS) {
          destroyed = false;
        }
      }
    }
    if (!destroyed) {
      std::cerr << "[WARNING] Event in event cache is not destroyed" << std::endl;
    }
    destroyed = true;
    for (auto& value : event_pools_) {
      for (auto pool : value.second) {
        ze_result_t status = ZE_FUNC(zeEventPoolDestroy)(pool);
        if (status != ZE_RESULT_SUCCESS) {
          destroyed = false;
        }
      }
    }
    if (!destroyed) {
      std::cerr << "[WARNING] Event pool in event cache is not destroyed" << std::endl;
    }
#endif /* _WIN32 */
  }

  bool QueryEvent(ze_event_handle_t event) {
    if (event == nullptr) {
      return false;
    }

    lock_.lock_shared();
    auto info = event_info_map_.find(event);
    bool found = (info != event_info_map_.end());
    lock_.unlock_shared();

    return found;
  }

  // Returns the pool index of a cached event, or EVENT_INDEX_UNKNOWN if the
  // event is not owned by this cache (e.g. an app-created event).
  uint32_t GetEventIndex(ze_event_handle_t event) {
    if (event == nullptr) {
      return EVENT_INDEX_UNKNOWN;
    }

    lock_.lock_shared();
    auto info = event_info_map_.find(event);
    uint32_t index = (info != event_info_map_.end()) ? info->second.second : EVENT_INDEX_UNKNOWN;
    lock_.unlock_shared();

    return index;
  }


  ze_event_handle_t GetEvent(ze_context_handle_t context) {
    if (context == nullptr) {
      return nullptr;
    }

    ze_event_handle_t event = nullptr;

    const std::lock_guard<std::shared_mutex> lock(lock_);

    auto result = event_map_.find(context);
    if (result == event_map_.end()) {
      result = event_map_.emplace(
          std::make_pair(context, std::vector<ze_event_handle_t>())).first;
    }

    if (result->second.empty()) {
      ze_result_t status = ZE_RESULT_SUCCESS;

      ze_event_pool_flags_t flags = ZE_EVENT_POOL_FLAG_HOST_VISIBLE | flags_;
      ze_event_pool_desc_t pool_desc = {
          ZE_STRUCTURE_TYPE_EVENT_POOL_DESC,
          nullptr, flags, EVENT_POOL_SIZE};
      ze_event_pool_handle_t pool = nullptr;
      status = ZE_FUNC(zeEventPoolCreate)(context, &pool_desc, 0, nullptr, &pool);
      if (status != ZE_RESULT_SUCCESS || pool == nullptr) {
        // Runtime allocation failure (device under pressure / device-lost
        // class): never abort the instrumented app -- report and let the
        // caller degrade (a nullptr event skips instrumentation of one op).
        std::cerr << "[ERROR] Failed to create event pool (status = 0x" << std::hex
                  << status << std::dec << ")" << std::endl;
        return nullptr;
      }

      auto pool_iter = event_pools_.find(context);
      if (pool_iter == event_pools_.end()) {
        pool_iter = event_pools_.emplace(std::make_pair(context, std::vector<ze_event_pool_handle_t>())).first;
      }
      pool_iter->second.push_back(pool);

      for (uint32_t i = 0; i < EVENT_POOL_SIZE; i++) {
        ze_event_desc_t event_desc = {
            ZE_STRUCTURE_TYPE_EVENT_DESC,
            nullptr,
            i,
            ZE_EVENT_SCOPE_FLAG_HOST,
            ZE_EVENT_SCOPE_FLAG_HOST};
        status = ZE_FUNC(zeEventCreate)(pool, &event_desc, &event);
        if (status != ZE_RESULT_SUCCESS || event == nullptr) {
          // Partial pool: keep what was created (they are usable), report the
          // gap; the caller sees a shorter free list, never a bad handle.
          std::cerr << "[WARNING] Event creation failed at index " << i << " of "
                    << EVENT_POOL_SIZE << " (status = 0x" << std::hex << status
                    << std::dec << ")" << std::endl;
          break;
        }

        PTI_ASSERT(event_info_map_.count(event) == 0);
        event_info_map_.insert({event, std::make_pair(context, i)});
        result->second.push_back(event);
      }

      if (result->second.empty()) {
        // Not a single event came back: destroy the pool (alloc/destroy
        // pairing) and report failure to the caller.
        std::cerr << "[WARNING] No event created from new pool, destroying it" << std::endl;
        status = ZE_FUNC(zeEventPoolDestroy)(pool);
        if (status != ZE_RESULT_SUCCESS) {
          std::cerr << "[WARNING] Failed to destroy empty event pool (status = 0x"
                    << std::hex << status << std::dec << ")" << std::endl;
        }
        pool_iter->second.pop_back();
        return nullptr;
      }
    }

    event = result->second.back();
    result->second.pop_back();

    //PTI_ASSERT(ZE_FUNC(zeEventQueryStatus)(event) == ZE_RESULT_NOT_READY);

    return event;
  }

  void ResetEvent(ze_event_handle_t event) {
    if (event == nullptr) {
      return;
    }

    const std::lock_guard<std::shared_mutex> lock(lock_);

    auto info = event_info_map_.find(event);
    if (info != event_info_map_.end()) {
      ze_result_t status = ZE_FUNC(zeEventHostReset)(event);
      if (status != ZE_RESULT_SUCCESS) {
        // Device-lost class of errors must not abort the instrumented app:
        // a lost reset only delays the event's next signal, it is reported
        // and the waiters behave as for a slow (not a wrong) timestamp.
        std::cerr << "[WARNING] Failed to reset event (status = 0x" << std::hex
                  << status << std::dec << ")" << std::endl;
      }
    }
  }

  void ReleaseEvent(ze_event_handle_t event) {
    if (event == nullptr) {
      return;
    }

    const std::lock_guard<std::shared_mutex> lock(lock_);

    auto info = event_info_map_.find(event);
    if (info == event_info_map_.end()) {
      return;
    }

    auto result = event_map_.find(info->second.first);
    PTI_ASSERT(result != event_map_.end());
    if (result != event_map_.end()) {
      // Workaround to cover L0 bug related to V2 adaptor. Tracking ID: https://github.com/intel-innersource/applications.analyzers.profilingtoolsinterfaces.sdk/issues/700
      // Idea is to create new event from same event pool at same index. Destroy the old event and update the map.

      // Find event pool and index where old event was create
      uint32_t event_pool_index = info->second.second;
      auto context = info->second.first;

      ze_event_pool_handle_t pool = nullptr;
      auto status = ZE_FUNC(zeEventGetEventPool)(event, &pool);
      if (status != ZE_RESULT_SUCCESS || pool == nullptr) {
        // The event stays tracked in the maps and will be retried on the
        // next release of the same handle -- nothing is corrupted, the old
        // event object is still owned by this cache.
        std::cerr << "[WARNING] Failed to query event pool for release (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
        return;
      }

      // destroy old event
      status = ZE_FUNC(zeEventDestroy)(event);
      if (status != ZE_RESULT_SUCCESS) {
        // Keep the old event tracked and retry on the next release; dropping
        // the map entry here would leak the object while still vouching for a
        // dead handle.
        std::cerr << "[WARNING] Failed to destroy event on release (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
        return;
      }

      // create new event
      ze_event_handle_t new_event = nullptr;
      ze_event_desc_t event_desc = {
          ZE_STRUCTURE_TYPE_EVENT_DESC,
          nullptr,
          event_pool_index,
          ZE_EVENT_SCOPE_FLAG_HOST,
          ZE_EVENT_SCOPE_FLAG_HOST};
      status = ZE_FUNC(zeEventCreate)(pool, &event_desc, &new_event);
      if (status != ZE_RESULT_SUCCESS || new_event == nullptr) {
        // The old event is destroyed, its slot is temporarily unavailable:
        // account the loss, keep the maps consistent (no entry for a handle
        // that does not exist) and shrink the free list by one.
        event_info_map_.erase(event);
        std::cerr << "[WARNING] Failed to recreate event on release (status = 0x"
                  << std::hex << status << std::dec << ")" << std::endl;
        return;
      }

      // Update event info map
      event_info_map_.erase(event);
      event_info_map_.insert({new_event,std::make_pair(context, event_pool_index)});
      result->second.push_back(new_event);
    }
  }

  // Releases every pool/event this cache owns for one context. Called from
  // the zeContextDestroy ENTER intercept, while the context is still alive:
  // zeEventDestroy/zeEventPoolDestroy below are exactly the destroy side of
  // the zeEventPoolCreate/zeEventCreate allocations GetEvent made -- and they
  // must run BEFORE the driver tears the context down (afterwards they are
  // use-after-destroy). Also safe as a no-op if nothing is tracked (exit-path
  // backstop).
  void ReleaseContext(ze_context_handle_t context) {
    if (context == nullptr) {
      return;
    }

    const std::lock_guard<std::shared_mutex> lock(lock_);

    auto result = event_map_.find(context);
    if (result == event_map_.end()) {
      return;  // nothing tracked for this context
    }

    size_t destroyed_events = 0, failed_events = 0;
    for (auto event : result->second) {
      ze_result_t status = ZE_FUNC(zeEventDestroy)(event);
      if (status == ZE_RESULT_SUCCESS) {
        event_info_map_.erase(event);
        destroyed_events++;
      } else {
        // Do not vouch for the handle once its context is going away even if
        // the destroy failed: report and drop the tracking either way.
        event_info_map_.erase(event);
        failed_events++;
      }
    }

    // Events still checked out (created but not yet released by the app
    // path): their context is being destroyed, so they can no longer be
    // returned through ReleaseEvent -- destroy them here, best effort.
    size_t destroyed_inflight = 0, failed_inflight = 0;
    for (auto info = event_info_map_.begin(); info != event_info_map_.end();) {
      if (info->second.first != context) {
        ++info;
        continue;
      }
      ze_result_t status = ZE_FUNC(zeEventDestroy)(info->first);
      if (status == ZE_RESULT_SUCCESS) {
        destroyed_inflight++;
      } else {
        failed_inflight++;
      }
      info = event_info_map_.erase(info);
    }

    event_map_.erase(result);

    auto iter = event_pools_.find(context);
    if (iter != event_pools_.end()) {
      size_t destroyed_pools = 0, failed_pools = 0;
      for (auto pool : iter->second) {
        ze_result_t status = ZE_FUNC(zeEventPoolDestroy)(pool);
        if (status == ZE_RESULT_SUCCESS) {
          destroyed_pools++;
        } else {
          failed_pools++;
        }
      }
      event_pools_.erase(iter);
      if (failed_events != 0 || failed_inflight != 0 || failed_pools != 0) {
        std::cerr << "[WARNING] Event cache release for context: " << destroyed_events
                  << " returned + " << destroyed_inflight << " in-flight events, "
                  << destroyed_pools << " pools destroyed; failures (status in prior lines): "
                  << failed_events << "/" << failed_inflight << "/" << failed_pools
                  << std::endl;
      }
    }
  }


 private:
  ze_event_pool_flags_t flags_ = 0;
  std::map<ze_context_handle_t, std::vector<ze_event_handle_t> > event_map_;
  std::map<ze_event_handle_t, std::pair<ze_context_handle_t, uint32_t>> event_info_map_;
  std::map<ze_context_handle_t, std::vector<ze_event_pool_handle_t> > event_pools_;
  std::shared_mutex lock_;
};

#endif // PTI_TOOLS_ZE_TRACER_ZE_EVENT_CACHE_H_
