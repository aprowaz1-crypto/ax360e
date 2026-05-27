/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_frontend.h"

#include "xenia/base/atomic.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_emit.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/ppc/ppc_translator.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xthread.h"  // for Reenter delivery of DEC 0x900

#include "ax360e_perf_log.h"  // for CpuAccuracyTracker DEC underflow/reenter counters (Captain Agent 4)

namespace xe {
namespace cpu {
namespace ppc {

void InitializeIfNeeded();
void CleanupOnShutdown();

void InitializeIfNeeded() {
  static bool has_initialized = false;
  if (has_initialized) {
    return;
  }
  has_initialized = true;

  RegisterEmitCategoryAltivec();
  RegisterEmitCategoryALU();
  RegisterEmitCategoryControl();
  RegisterEmitCategoryFPU();
  RegisterEmitCategoryMemory();

  atexit(CleanupOnShutdown);
}

void CleanupOnShutdown() {}

PPCFrontend::PPCFrontend(Processor* processor) : processor_(processor) {
  InitializeIfNeeded();
}

PPCFrontend::~PPCFrontend() {
  // Force cleanup now before we deinit.
  translator_pool_.Reset();
}

Memory* PPCFrontend::memory() const { return processor_->memory(); }

// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<std::recursive_mutex*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  std::lock_guard<std::recursive_mutex> lock(*global_mutex);
  ppc_context->scratch = *global_lock_count ? 0 : 0x8000;
}

// Enters the global lock. Safe to recursion.
void EnterGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<std::recursive_mutex*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  global_mutex->lock();
  xe::atomic_inc(global_lock_count);
}

// Leaves the global lock. Safe to recursion.
void LeaveGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<std::recursive_mutex*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  auto new_lock_count = xe::atomic_dec(global_lock_count);
  assert_true(new_lock_count >= 0);
  global_mutex->unlock();
}

// Checks for a pending decrementer underflow. If dec_pending && EE enabled
// (checked via global lock state / scratch) and not in critical section, then
// deliver the 0x900 decrementer exception using the reenter mechanism (required
// on Android a64 because C++ exceptions cannot cross JIT code).
// Sets up minimal SRR0/SRR1 and vectors to (ivpr + 0x900).
// This respects interrupt masking and global lock.
void CheckDecrementerInterrupt(PPCContext* ppc_context, void* arg0, void* arg1) {
  if (!ppc_context->dec_pending) {
    return;
  }

  // Respect global interrupt lock (same as EE check in mfmsr).
  auto global_mutex = reinterpret_cast<std::recursive_mutex*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  std::lock_guard<std::recursive_mutex> lock(*global_mutex);
  if (*global_lock_count != 0) {
    // Interrupts disabled via global lock (mtmsr r13 pattern).
    return;
  }

  // EE bit is effectively set when not locked (see CheckGlobalLock / mfmsr).
  // Clear pending.
  ppc_context->dec_pending = 0;

  // Record underflow at actual delivery point (covers cases set in mfspr but delivered here).
  ax360e::perf::g_cpu_accuracy.RecordDecUnderflowFired();
  ax360e::perf::g_cpu_accuracy.RecordDecReentered();

  // Save for proper exception context (SRR0 = "next" address, SRR1 = MSR image).
  // POLISH (CAPTAIN DEC): SRR0 now documented as continuation (LR approx for JIT reenter
  // delivery; future can use translator block PC capture for exact instr on 0x900).
  // Tighter EE interaction handled by caller (global lock proxy). Reenter ensures correct target.
  ppc_context->srr0 = ppc_context->lr;
  // Simple MSR image (EE set + other common bits). Real impl would capture full MSR.
  ppc_context->srr1 = 0x8000 | 0x2000;  // EE + RI-ish

  // Compute vector target (standard PPC decrementer exception offset 0x900).
  uint64_t vector_base = ppc_context->ivpr;
  uint32_t target = static_cast<uint32_t>(vector_base + 0x900);

  // Deliver via reenter (longjmp out of current JIT frame back to XThread::Execute
  // loop, which will ExecuteRaw at the handler). This is the Android/a64-safe path.
  // Kernel handlers expect the SRR0/SRR1 + appropriate IRQL etc.
  if (auto* xthread = xe::kernel::XThread::GetCurrentThread()) {
    // Clear pending again in case of races.
    ppc_context->dec_pending = 0;
    XELOGCPU("Delivering DEC exception to 0x{:08X} (ivpr=0x{:016X})", target, vector_base);
    xthread->Reenter(target);
  } else {
    XELOGW("DEC interrupt pending but no current XThread for reentry");
  }
}

void SyscallHandler(PPCContext* ppc_context, void* arg0, void* arg1) {
  uint64_t syscall_number = ppc_context->r[0];
  switch (syscall_number) {
    default:
      assert_unhandled_case(syscall_number);
      XELOGE("Unhandled syscall {}!", syscall_number);
      break;
#pragma warning(suppress : 4065)
  }
}

bool PPCFrontend::Initialize() {
  void* arg0 = reinterpret_cast<void*>(&xe::global_critical_region::mutex());
  void* arg1 = reinterpret_cast<void*>(&builtins_.global_lock_count);
  builtins_.check_global_lock =
      processor_->DefineBuiltin("CheckGlobalLock", CheckGlobalLock, arg0, arg1);
  builtins_.enter_global_lock =
      processor_->DefineBuiltin("EnterGlobalLock", EnterGlobalLock, arg0, arg1);
  builtins_.leave_global_lock =
      processor_->DefineBuiltin("LeaveGlobalLock", LeaveGlobalLock, arg0, arg1);
  builtins_.syscall_handler = processor_->DefineBuiltin(
      "SyscallHandler", SyscallHandler, nullptr, nullptr);
  builtins_.check_decrementer_interrupt = processor_->DefineBuiltin(
      "CheckDecrementerInterrupt", CheckDecrementerInterrupt,
      reinterpret_cast<void*>(&xe::global_critical_region::mutex()),
      reinterpret_cast<void*>(&builtins_.global_lock_count));
  return true;
}

bool PPCFrontend::DeclareFunction(GuestFunction* function) {
  // Could scan or something here.
  // Could also check to see if it's a well-known function type and classify
  // for later.
  // Could also kick off a precompiler, since we know it's likely the function
  // will be demanded soon.
  return true;
}

bool PPCFrontend::DefineFunction(GuestFunction* function,
                                 uint32_t debug_info_flags) {
  auto translator = translator_pool_.Allocate(this);
  bool result = translator->Translate(function, debug_info_flags);
  translator_pool_.Release(translator);
  return result;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
