/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/stack_walker.h"

#include "xenia/base/logging.h"

#include <unwind.h>
#include <cstdint>
#include <vector>

namespace xe {
namespace cpu {

// Basic POSIX/AArch64 stack walker using _Unwind_Backtrace (available on Android NDK).
// Provides host PCs for debugging/crash dumps. Guest symbol resolution and
// full host-guest mapping is limited (see enable_host_guest_stack_synchronization
// and A64 code cache EH frames). This greatly improves diagnostics vs. nullptr.

class PosixStackWalker : public StackWalker {
 public:
  explicit PosixStackWalker(backend::CodeCache* code_cache)
      : code_cache_(code_cache) {}

  size_t CaptureStackTrace(uint64_t* frame_host_pcs,
                           size_t frame_offset, size_t frame_count,
                           uint64_t* out_stack_hash = nullptr) override {
    struct CaptureState {
      uint64_t* pcs;
      size_t offset;
      size_t max;
      size_t count;
    } state = { frame_host_pcs, frame_offset, frame_count, 0 };

    _Unwind_Backtrace([](struct _Unwind_Context* ctx, void* arg) -> _Unwind_Reason_Code {
      auto* s = reinterpret_cast<CaptureState*>(arg);
      if (s->count >= s->max) return _URC_END_OF_STACK;
      uint64_t ip = _Unwind_GetIP(ctx);
      if (s->count >= s->offset) {
        s->pcs[s->count - s->offset] = ip;
      }
      s->count++;
      return _URC_NO_REASON;
    }, &state);

    size_t captured = (state.count > state.offset) ? (state.count - state.offset) : 0;
    if (captured > frame_count) captured = frame_count;
    if (out_stack_hash) {
      uint64_t h = 0;
      for (size_t i = 0; i < captured; ++i) h ^= frame_host_pcs[i] + 0x9e3779b97f4a7c15ULL;
      *out_stack_hash = h;
    }
    return captured;
  }

  size_t CaptureStackTrace(void* thread_handle,
                           uint64_t* frame_host_pcs,
                           size_t frame_offset, size_t frame_count,
                           const HostThreadContext* in_host_context,
                           HostThreadContext* out_host_context,
                           uint64_t* out_stack_hash = nullptr) override {
    // For other threads, full capture is complex without ptrace; fallback to current + log.
    XELOGW("PosixStackWalker: cross-thread capture limited on POSIX/Android; capturing current thread context only");
    return CaptureStackTrace(frame_host_pcs, frame_offset, frame_count, out_stack_hash);
  }

  bool ResolveStack(uint64_t* frame_host_pcs, StackFrame* frames,
                    size_t frame_count) override {
    for (size_t i = 0; i < frame_count; ++i) {
      frames[i].type = StackFrame::Type::kHost;
      frames[i].host_pc = frame_host_pcs[i];
      frames[i].guest_pc = 0;
      frames[i].host_symbol.address = frame_host_pcs[i];
      frames[i].host_symbol.name[0] = '\0';  // TODO: dladdr for symbols if needed
      frames[i].guest_symbol.function = nullptr;
    }
    // Note: full guest PC resolution requires A64 stackpoint sync + code cache lookup.
    // On Android with incomplete unwind info, many frames will be host-only.
    return true;
  }

  ~PosixStackWalker() override = default;

 private:
  backend::CodeCache* code_cache_;
};

std::unique_ptr<StackWalker> StackWalker::Create(
    backend::CodeCache* code_cache) {
  XELOGI("Creating basic POSIX stack walker (Android/A64 diagnostics enabled; guest frames partial)");
  return std::make_unique<PosixStackWalker>(code_cache);
}

}  // namespace cpu
}  // namespace xe