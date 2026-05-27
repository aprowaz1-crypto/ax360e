/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/thread_state.h"

#include <cstdlib>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"

#include "xenia/xbox.h"
namespace xe {
namespace cpu {

thread_local ThreadState* thread_state_ = nullptr;

    struct PackedContext {
        XE_MAYBE_UNUSED uint64_t backend_data[32];//分配256字节給CPU后端使用
        ppc::PPCContext ctx;
    };

    // Size auto-updates with net GQR placement (+32B uint32 near f[32] after
    // consolidating late stub). R1 ps research infrastructure complete.
    // See ppc_context.h (multiple citations to ppc_emit_fpu.cc plan block).
    static_assert(sizeof(PackedContext) == sizeof(ppc::PPCContext)+256);
static void* AllocateContext() {
    PackedContext* ptr=memory::AlignedAlloc<PackedContext>(64);
    return &ptr->ctx;
}

static void FreeContext(void* ctx) {
    void* ptr=reinterpret_cast<uint8_t*>( ctx)-256;
    memory::AlignedFree(ptr);
}

ThreadState::ThreadState(Processor* processor, uint32_t thread_id,
                         uint32_t stack_base, uint32_t pcr_address)
    : processor_(processor),
      memory_(processor->memory()),
      thread_id_(thread_id) {
  if (thread_id_ == UINT_MAX) {
    // System thread. Assign the system thread ID with a high bit
    // set so people know what's up.
    uint32_t system_thread_handle = xe::threading::current_thread_system_id();
    thread_id_ = 0x80000000 | system_thread_handle;
  }
  backend_data_ = processor->backend()->AllocThreadData();

  // Allocate with 64b alignment.

  context_ = reinterpret_cast<ppc::PPCContext*>(AllocateContext());
  processor->backend()->InitializeBackendContext(context_);
  assert_true(((uint64_t)context_ & 0x3F) == 0);
  std::memset(context_, 0, sizeof(ppc::PPCContext));
  // GQR[8] (uint32 at 0x220 after f[32]) zeroed here automatically. Per R1 ps
  // research report (GQR state + basic infrastructure task). get_gqr/set_gqr +
  // inline quantization helpers (no tables) now available. See ppc_context.h
  // (R1 citations + plan block ref in ppc_emit_fpu.cc).

  // Stash pointers to common structures that callbacks may need.
  context_->global_mutex = &xe::global_critical_region::mutex();
  context_->virtual_membase = memory_->virtual_membase();
  context_->physical_membase = memory_->physical_membase();
  context_->processor = processor_;
  context_->thread_state = this;
  context_->thread_id = thread_id_;

  // Set initial registers.
  context_->r[1] = stack_base;
  context_->r[13] = pcr_address;
}

ThreadState::~ThreadState() {
  if (backend_data_) {
    processor_->backend()->FreeThreadData(backend_data_);
  }
  if (thread_state_ == this) {
    thread_state_ = nullptr;
  }
  if (context_) {
    processor_->backend()->DeinitializeBackendContext(context_);
    FreeContext(reinterpret_cast<void*>(context_));
  }
}

void ThreadState::Bind(ThreadState* thread_state) {
  thread_state_ = thread_state;
}

ThreadState* ThreadState::Get() { return thread_state_; }

uint32_t ThreadState::GetThreadID() {
  return thread_state_ ? thread_state_->thread_id_ : 0xFFFFFFFF;
}

}  // namespace cpu
}  // namespace xe
