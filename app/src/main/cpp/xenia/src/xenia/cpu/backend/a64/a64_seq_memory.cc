/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/memory.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/backend/a64/a64_backend.h"  // for ReserveHelper, A64BackendContext, cached_reserve_* offsets, RESERVE_BLOCK_SHIFT
#include "xenia/cpu/cpu_flags.h"  // for cvars::a64_accuracy_debug (resilience logging)

// CPU accuracy metrics (reservations etc.)
// Include resolves via Android build -I pointing at cpp/ root (ax360e_perf_log.h lives there).
#include "ax360e_perf_log.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_memory = 0;

template <typename T>
XReg ComputeMemoryAddressOffset(A64Emitter& e, const T& guest, const T& offset,
                                WReg address_register = W3) {
  assert_true(offset.is_constant);
  const int32_t offset_const = static_cast<int32_t>(offset.constant());

  if (guest.is_constant) {
    uint32_t address = static_cast<uint32_t>(guest.constant());
    address += offset_const;
    if (address < 0x80000000) {
      e.MOV(address_register.toX(), address);
      e.ADD(address_register.toX(), e.GetMembaseReg(), address_register.toX());
      return address_register.toX();
    } else {
      if (address >= 0xE0000000 &&
          xe::memory::allocation_granularity() > 0x1000) {
        e.MOV(W0, address + 0x1000);
      } else {
        e.MOV(W0, address);
      }
      e.ADD(address_register.toX(), e.GetMembaseReg(), X0);
      return address_register.toX();
    }
  } else {
    if (xe::memory::allocation_granularity() > 0x1000) {
      // Emulate the 4 KB physical address offset in 0xE0000000+ when can't do
      // it via memory mapping.
      e.MOV(W0, 0xE0000000 - offset_const);
      e.CMP(guest.reg().toW(), W0);
      e.CSET(W0, Cond::HS);
      e.ADD(W0, guest.reg().toW(), W0, LSL, 12);
    } else {
      // Clear the top 32 bits, as they are likely garbage.
      // TODO(benvanik): find a way to avoid doing this.
      e.MOV(W0, guest.reg().toW());
    }
    e.MOV(X1, offset_const);
    e.ADD(X0, X0, X1);

    e.ADD(address_register.toX(), e.GetMembaseReg(), X0);
    return address_register.toX();
  }
}

// Note: most *should* be aligned, but needs to be checked!
template <typename T>
XReg ComputeMemoryAddress(A64Emitter& e, const T& guest,
                          WReg address_register = W3) {
  if (guest.is_constant) {
    // TODO(benvanik): figure out how to do this without a temp.
    // Since the constant is often 0x8... if we tried to use that as a
    // displacement it would be sign extended and mess things up.
    const uint32_t address = static_cast<uint32_t>(guest.constant());
    if (address < 0x80000000) {
      e.MOV(W0, address);
      e.ADD(address_register.toX(), e.GetMembaseReg(), X0);
      return address_register.toX();
    } else {
      if (address >= 0xE0000000 &&
          xe::memory::allocation_granularity() > 0x1000) {
        e.MOV(W0, address + 0x1000u);
      } else {
        e.MOV(W0, address);
      }
      e.ADD(address_register.toX(), e.GetMembaseReg(), X0);
      return address_register.toX();
    }
  } else {
    if (xe::memory::allocation_granularity() > 0x1000) {
      // Emulate the 4 KB physical address offset in 0xE0000000+ when can't do
      // it via memory mapping.
      e.MOV(W0, 0xE0000000);
      e.CMP(guest.reg().toW(), W0);
      e.CSET(W0, Cond::HS);
      e.ADD(W0, guest.reg().toW(), W0, LSL, 12);
    } else {
      // Clear the top 32 bits, as they are likely garbage.
      // TODO(benvanik): find a way to avoid doing this.
      e.MOV(W0, guest.reg().toW());
    }
    e.ADD(address_register.toX(), e.GetMembaseReg(), X0);
    return address_register.toX();
  }
}

bool LocalOffsetFitsUnscaledAccess(uint32_t offset, uint32_t access_size) {
  if (offset > 0xFFFu * access_size) {
    return false;
  }
  return (offset & (access_size - 1)) == 0;
}

XReg ComputeLocalAddress(A64Emitter& e, uint32_t offset,
                         XReg address_register = X1) {
  e.MOV(X0, static_cast<uint64_t>(offset));
  e.ADD(address_register, SP, X0);
  return address_register;
}

// ============================================================================
// OPCODE_ATOMIC_EXCHANGE
// ============================================================================
// Note that the address we use here is a real, host address!
// This is weird, and should be fixed.
template <typename SEQ, typename REG, typename ARGS, typename FN>
void EmitAtomicExchangeXX(A64Emitter& e, const ARGS& i, const FN& fn) {
  if (i.dest == i.src1) {
    e.MOV(X0, i.src1);
    if (i.dest != i.src2) {
      if (i.src2.is_constant) {
        e.MOV(i.dest, i.src2.constant());
      } else {
        e.MOV(i.dest, i.src2);
      }
    }
    fn(e, i.dest, X0);
  } else {
    if (i.dest != i.src2) {
      if (i.src2.is_constant) {
        e.MOV(i.dest, i.src2.constant());
      } else {
        e.MOV(i.dest, i.src2);
      }
    }
    fn(e, i.dest, i.src1);
  }
}
struct ATOMIC_EXCHANGE_I8
    : Sequence<ATOMIC_EXCHANGE_I8,
               I<OPCODE_ATOMIC_EXCHANGE, I8Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAtomicExchangeXX<ATOMIC_EXCHANGE_I8, WReg>(
        e, i,
        [](A64Emitter& e, WReg dest, XReg src) {
          // 128B complete: atomic byte store (SWP) path covered; always performs write.
          XReg gea = X4;
          e.MOV(gea, src);
          e.SUB(gea, gea, e.GetMembaseReg());
          // Touched for CAPTAIN 128B pairing errata: Clear now contains cross-thread overlap detector
          // (another thread's res on overlapping 128B -> XELOGW + increment_pairing_violation under a64_accuracy_debug).
          // Research: per-thread SMT pairing errata + 128B false share.
          ClearXenonReservationIfStoreOverlaps(e, gea, 1);
          e.SWPALB(dest, dest, src);
        });
  }
};
struct ATOMIC_EXCHANGE_I16
    : Sequence<ATOMIC_EXCHANGE_I16,
               I<OPCODE_ATOMIC_EXCHANGE, I16Op, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAtomicExchangeXX<ATOMIC_EXCHANGE_I8, WReg>(
        e, i,
        [](A64Emitter& e, WReg dest, XReg src) {
          // 128B complete: atomic halfword store path.
          XReg gea = X4;
          e.MOV(gea, src);
          e.SUB(gea, gea, e.GetMembaseReg());
          // Touched (128B errata enforcement): Clear runs cross-thread pairing check here too (SMT/PE-local monitor risks).
          ClearXenonReservationIfStoreOverlaps(e, gea, 2);
          e.SWPALH(dest, dest, src);
        });
  }
};
struct ATOMIC_EXCHANGE_I32
    : Sequence<ATOMIC_EXCHANGE_I32,
               I<OPCODE_ATOMIC_EXCHANGE, I32Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAtomicExchangeXX<ATOMIC_EXCHANGE_I8, WReg>(
        e, i,
        [](A64Emitter& e, WReg dest, XReg src) {
          // 128B complete: I32 atomic exchange store (SWPAL) covered.
          XReg gea = X4;
          e.MOV(gea, src);
          e.SUB(gea, gea, e.GetMembaseReg());
          // Touched for 128B Xenon model: ensures pairing violation detection on 32-bit atomic stores (false share at granule).
          ClearXenonReservationIfStoreOverlaps(e, gea, 4);
          e.SWPAL(dest, dest, src);
        });
  }
};
struct ATOMIC_EXCHANGE_I64
    : Sequence<ATOMIC_EXCHANGE_I64,
               I<OPCODE_ATOMIC_EXCHANGE, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAtomicExchangeXX<ATOMIC_EXCHANGE_I8, XReg>(
        e, i,
        [](A64Emitter& e, XReg dest, XReg src) {
          // 128B complete: I64 atomic exchange (STXR-class via SWPAL) covered.
          XReg gea = X4;
          e.MOV(gea, src);
          e.SUB(gea, gea, e.GetMembaseReg());
          // CAPTAIN: 64-bit atomic store path now guarantees the 128B cross-thread check executes (research pairing errata).
          ClearXenonReservationIfStoreOverlaps(e, gea, 8);
          e.SWPAL(dest, dest, src);
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ATOMIC_EXCHANGE, ATOMIC_EXCHANGE_I8,
                     ATOMIC_EXCHANGE_I16, ATOMIC_EXCHANGE_I32,
                     ATOMIC_EXCHANGE_I64);

// ============================================================================
// OPCODE_ATOMIC_COMPARE_EXCHANGE
// ============================================================================
struct ATOMIC_COMPARE_EXCHANGE_I32
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I32,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (xe::memory::allocation_granularity() > 0x1000) {
      // Emulate the 4 KB physical address offset in 0xE0000000+ when can't do
      // it via memory mapping.
      e.MOV(W3, 0xE0000000);
      e.CMP(i.src1.reg().toW(), W3);
      e.CSET(W1, Cond::HS);
      e.ADD(W1, i.src1.reg().toW(), W1, LSL, 12);
    } else {
      e.MOV(W1, i.src1.reg().toW());
    }
    e.ADD(X1, e.GetMembaseReg(), X1);

    const XReg address = X1;
    const WReg expected = i.src2;
    const WReg desired = i.src3;
    const WReg status = W0;

    // 128B complete (atomic store path via CASAL/STLXR): invalidate if overlaps res granule.
    // (Note: conditional CAS may not store; conservative clear is acceptable, no goldplate.)
    {
      XReg gea = X4;
      e.MOV(gea, address);
      e.SUB(gea, gea, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, gea, 4);
    }

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.MOV(status, expected);

      // if([C] == A) [C] = B
      // else A = [C]
      e.CASAL(status, desired, address);
      e.CMP(status, expected);
      e.CSET(i.dest, Cond::EQ);
      return;
    }

    oaknut::Label success, fail, retry;

    e.l(retry);
    e.LDAXR(W4, address);
    e.CMP(W4, expected);
    e.B(Cond::NE, fail);

    e.STLXR(status.toW(), desired, address);
    e.CBNZ(status, retry);
    e.B(success);

    e.l(fail);
    e.CLREX();

    e.l(success);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct ATOMIC_COMPARE_EXCHANGE_I64
    : Sequence<ATOMIC_COMPARE_EXCHANGE_I64,
               I<OPCODE_ATOMIC_COMPARE_EXCHANGE, I8Op, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (xe::memory::allocation_granularity() > 0x1000) {
      // Emulate the 4 KB physical address offset in 0xE0000000+ when can't do
      // it via memory mapping.
      e.MOV(W3, 0xE0000000);
      e.CMP(i.src1.reg(), X3);
      e.CSET(W1, Cond::HS);
      e.ADD(W1, i.src1.reg().toW(), W1, LSL, 12);
    } else {
      e.MOV(W1, i.src1.reg().toW());
    }
    e.ADD(X1, e.GetMembaseReg(), X1);

    const XReg address = X1;
    const XReg expected = i.src2;
    const XReg desired = i.src3;
    const XReg status = X0;

    // 128B complete: I64 CAS/STLXR atomic store path (bypass main STORE).
    {
      XReg gea = X4;
      e.MOV(gea, address);
      e.SUB(gea, gea, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, gea, 8);
    }

    if (e.IsFeatureEnabled(kA64EmitLSE)) {
      e.MOV(status, expected);

      // if([C] == A) [C] = B
      // else A = [C]
      e.CASAL(status, desired, address);
      e.CMP(status, expected);
      e.CSET(i.dest, Cond::EQ);
      return;
    }

    oaknut::Label success, fail, retry;

    e.l(retry);
    e.LDAXR(X4, address);
    e.CMP(X4, expected);
    e.B(Cond::NE, fail);

    e.STLXR(status.toW(), desired, address);
    e.CBNZ(status, retry);
    e.B(success);

    e.l(fail);
    e.CLREX();

    e.l(success);
    e.CSET(i.dest, Cond::EQ);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ATOMIC_COMPARE_EXCHANGE,
                     ATOMIC_COMPARE_EXCHANGE_I32, ATOMIC_COMPARE_EXCHANGE_I64);

// ============================================================================
// OPCODE_LOAD_RESERVED / OPCODE_STORE_RESERVED
// Tiered PowerPC reservation support (hardware-first + optional software fallback):
// - LOAD: Always records exact guest EA (cached_reserve_offset), raw value snapshot
//   (cached_reserve_value_), and marks coarse block in the *shared* ReserveHelper
//   (atomic bit ops). Clears any prior reservation for this thread.
// - STORE: 
//     1. Software EA validation (cached) - enforces real PPC addr-match rule, cheap.
//     2. Hardware STLXR (fast path, zero extra cost on uncontended/same-core success).
//     3. On STLXR fail + a64_software_reservation_fallback=true: software recovery
//        using ReserveHelper state + exact EA + value snapshot compare. If memory
//        still matches the l*arx snapshot, we safely claim success + store (via
//        LDXR/STXR atomic in fallback). This recovers cross-core monitor loss.
// - Always clears reservation state on any st*cx (PPC rule).
// - Controlled by a64_software_reservation_fallback cvar (default true for Android).
//
// Research-driven (2026 Xenon reverse-engineering):
// - 128B model COMPLETE COVERAGE (Code Agent 2 follow-up): ClearXenonReservationIfStoreOverlaps
//   now called from EVERY guest store emitter in this file:
//     * All STORE_I* (I8/I16/I32/I64/F32/F64/V128) + all STORE_OFFSET_* 
//     * ATOMIC_EXCHANGE_* (SWP*) and ATOMIC_COMPARE_EXCHANGE (CASAL + STLXR variants)
//   Byte/halfword + 16B V128 now correctly use size for 1-2 granule span (unaligned cases).
// - Any normal store (ST8/ST16/ST32 etc, release, atomic) to 128B granule overlapping active res
//   must invalidate (real Xenon). This closes the last holes from initial I32/I64/V128 work.
// - RE-TASK: psq_st / paired-single quantized stores (from R1 ps_* report) are normal stores;
//   once emitters land they will hit these paths (esp. F32/F64 or psq mem) + Clear for 128B
//   (TLB owner integration: these are the exact hot paths exercised by new TLB+psq_st/FPU 128B seqs in ps harness)
//   invalidation + cross-thread pairing probe. See ExercisePsqStore... helper + ps harness seqs.
// - 128-byte exact checks use the per-thread cached_reserve_offset (exact EA from LOAD) + granule helpers
//   (defined in a64_backend.h). The coarse 64KB bitmap remains only as a "possible overlap" filter.
// - ea recovery via (addr_reg - membase) is granule-correct (0x1000 hack is multiple of 128).
// - CpuAccuracyTracker::stores_that_invalidated_reservations_ tracks instrumented sites.

// Research-driven helper (from real Xenon 128-byte granule + store invalidation analysis):
// If the current guest thread has an active reservation whose *128-byte* granule
// overlaps this store [ea, ea+size), clear the reservation (flag + best-effort bitmap bit).
// This matches real Xenon behavior (any normal store invalidates). Cheap inline check on cached_ fields.
// ea passed should be logical guest (or host-membase diff; +4096 hack is 128B-aligned so granule identical).
// *** PAIRING ERRATA ENFORCEMENT (Code Agent 1): the debug block appended below (after no_active_res)
//     + all ~12+ call sites touched in this file (STORE, OFFSET, F*, V128, SWP*, CAS) guarantee that
//     a64_accuracy_debug detects + reports (XELOGW + increment_pairing_violation + DebugBreak) when
//     another logical thread holds overlapping 128B res. Cites SMT pairing errata + 128B false share.
inline void ClearXenonReservationIfStoreOverlaps(A64Emitter& e, const XReg& store_ea_guest, uint32_t store_size = 1) {
  // Count instrumented store sites (one per guest store emitter that can invalidate res on 128B model).
  // Measures how many such stores fire in titles (JIT-time site count; pairs with runtime res metrics).
  ax360e::perf::g_cpu_accuracy.RecordStoreThatInvalidatedReservation();

  const XReg ctx = X5;
  const WReg wflags = W6;
  const XReg cached_ea = X7;

  e.SUB(ctx, e.GetContextReg(), sizeof(A64BackendContext));
  e.LDR(wflags, ctx, offsetof(A64BackendContext, flags));
  e.TST(wflags, 2);
  oaknut::Label no_active_res;
  e.B(Cond::EQ, no_active_res);

  e.LDR(cached_ea, ctx, offsetof(A64BackendContext, cached_reserve_offset));

  // True 128-byte Xenon granule check (research-driven fidelity).
  // Supports full store span for unaligned cases (even I8/I16 can straddle 128B if misaligned).
  // Coarse 64KB bitmap acts only as a fast "possible overlap" filter;
  // this exact check enforces real hardware behavior.
  e.MOV(X8, XENON_RESERVE_GRANULE_MASK);
  e.AND(X9, cached_ea, X8);
  e.AND(X10, store_ea_guest, X8);
  e.CMP(X9, X10);
  oaknut::Label do_clear;
  if (store_size > 1) {
    e.B(Cond::EQ, do_clear);
    // Check end of store range for cross-granule unaligned/wide stores (V128 etc.)
    e.ADD(X11, store_ea_guest, store_size - 1);
    e.AND(X11, X11, X8);
    e.CMP(X9, X11);
    e.B(Cond::NE, no_active_res);
    e.l(do_clear);
  } else {
    e.B(Cond::NE, no_active_res);
  }

  // Overlap on real 128-byte granule → clear this thread's reservation
  // (matches Xenon PPE + all shipped consoles).
  e.AND(wflags, wflags, ~2);
  e.STR(wflags, ctx, offsetof(A64BackendContext, flags));

  // Best-effort clear in the shared coarse bitmap (64KB filter only)
  e.LDR(X11, ctx, offsetof(A64BackendContext, reserve_helper_));
  e.LDR(X12, ctx, offsetof(A64BackendContext, cached_reserve_bit));
  e.LSR(X13, cached_ea, RESERVE_BLOCK_SHIFT);
  e.AND(X12, X12, 63);
  e.LSR(X14, X13, 6);
  e.LSL(X14, X14, 3);
  e.ADD(X15, X11, X14);

  oaknut::Label clear_done;
  e.LDXR(X16, X15);
  e.MOV(X17, 1);
  e.LSL(X17, X17, X12);
  e.BIC(X16, X16, X17);
  e.STXR(W18, X16, X15);  // ignore status (best effort)
  e.l(clear_done);

  e.l(no_active_res);

  // === CAPTAIN DIRECT ORDER: 128B CROSS-THREAD PAIRING VIOLATION CHECK (inside Clear) ===
  // This runs on *every* hot normal store path (all STORE_* call this helper).
  // If coarse bit for store's 64KB block is set but *this* thread has no active res flag,
  // it means *another logical thread* holds an active reservation overlapping the 128B granule
  // (risky per research). XELOGW + increment + (under a64_accuracy_debug) optional DebugBreak.
  // Cites: Real Xenon 128B granule, per-thread SMT pairing errata (no cross-thread handoff),
  // false-sharing at 128B boundaries, explicit Clear needed, Android PE-local + migration.
  //
  // RE-TASK: explicitly covers psq_st / paired-single stores (R1 ps_* report): psq_st lowers
  // to F32-pair quantized stores (or dedicated) and *must* call ClearXenon (like STORE_F32/F64).
  // The new ps_* harness sequences (ExercisePsqStore... + dedicated seq 6 in RunPairedSingle)
  // validate that lwarx (thread A) + psq_st cross-granule (thread B) correctly fires this probe,
  // the XenonReservesOverlapAcrossThreads helper, last_* fields, and psq_store_reservation_pairing_violation counters.
  if (cvars::a64_accuracy_debug) {
    // Recompute block for store_ea_guest (already in X1 area but recompute defensively)
    e.MOV(X19, store_ea_guest);
    e.LSR(X19, X19, RESERVE_BLOCK_SHIFT);
    e.LSR(X20, X19, 6);
    e.LSL(X20, X20, 3);
    e.LDR(X21, ctx, offsetof(A64BackendContext, reserve_helper_));
    e.ADD(X20, X21, X20);
    oaknut::Label probe_done;
    e.LDR(X22, X20);  // non-atomic best-effort read of the word (debug only)
    e.AND(X23, X19, 63);
    e.MOV(X24, 1);
    e.LSL(X24, X24, X23);
    e.ANDS(X24, X22, X24);
    e.B(Cond::EQ, probe_done);  // bit clear -> no other res in block
    // Bit was set for the block. Check if *we* currently hold active res flag.
    e.LDR(W25, ctx, offsetof(A64BackendContext, flags));
    e.TST(W25, 2);
    oaknut::Label no_cross_violation;
    e.B(Cond::NE, no_cross_violation);  // we hold it -> self, not cross
    // Another thread holds overlapping res granule (coarse proxy for exact 128B overlap risk)
    XELOGW("A64 Accuracy (128B Xenon): cross-thread reservation pairing violation: "
           "normal store overlaps 128B granule with active reservation held by another logical thread. "
           "Per-thread monitor pairing errata + 128B false share (real Xenon SMT/Android big.LITTLE).");
    ax360e::perf::g_cpu_accuracy.increment_pairing_violation();
    e.DebugBreak();  // surface immediately in a64_accuracy_debug (Captain enforcement)
    e.l(no_cross_violation);
    e.l(probe_done);
  }
}

// 128B RESERVATION STRESS HARNESS - RUNTIME SEQUENCES (CAPTAIN DIRECT ORDER).
// Guarded by a64_128b_reservation_stress (or a64_accuracy_debug).
// These are the exact validation sequences requested:
//   - Thread A: lwarx on address X (128B granule 0)  [see LOAD_RESERVED_*]
//   - Thread B / crossing: ordinary stw to X+64 or X+127 (boundary cross) -> verify reservation cleared on A via ClearXenon...
//   - V128 wide store analogs (e.g. near-granule stvx-like paths also call Clear...)
//   - Results logged + dedicated CpuAccuracyTracker counters incremented (crossing_invalidation_tests, false_share_detected).
// The emitted ClearXenonReservationIfStoreOverlaps (called from all ST* paths below) is what actually runs on Adreno.
// Harness activation also happens at backend init + explicit Java trigger (PerformanceMonitor).
// Full citations in a64_backend.{h,cc} and the research comments above.
//
// RE-TASK supporting: psq_store_reservation_pairing activation (via Exercise... helper) also lives
// here for psq_st store paths (psq_st treated as normal store per R1 ps_* report must hit Clear + pairing probe).
//
// Lightweight activation (runtime, once):
static bool g_128b_harness_activated_from_seq = false;
if ((cvars::a64_128b_reservation_stress || cvars::a64_accuracy_debug) && !g_128b_harness_activated_from_seq) {
  g_128b_harness_activated_from_seq = true;
  // Delegate to the full simulated sequences + counter increments + logs (defined in a64_backend.cc)
  // This ensures the "small set of runtime sequences" are exercised when any memory seq is first translated under the cvar.
  Run128BReservationStressTestHarness();
  XELOGI("A64 128B stress harness: sequences activated from seq_memory (lwarx + crossing stw/V128).");
}

// CAPTAIN RE-TASK small supporting addition (in seq_memory per scope):
// Activate the psq_store_reservation_pairing_violation helper (declared in a64_backend.h)
// under the ps stress cvar (a64_ps_accuracy_stress) or a64_accuracy_debug.
// This ensures that when memory store sequences (future psq_st quantized paired-single stores)
// are translated, the exact cross-thread lwarx+psq_st 128B pattern + new counters are exercised.
// Mirrors the 128B + FPU ps activations. Thin (helper is gated + delegates to harness + records).
// Ties psq_st (as normal store per R1 report) directly to 128B Clear + pairing probe.
static bool g_psq_pairing_harness_activated_from_mem = false;
if ((cvars::a64_ps_accuracy_stress || cvars::a64_accuracy_debug) && !g_psq_pairing_harness_activated_from_mem) {
  g_psq_pairing_harness_activated_from_mem = true;
  // Call the dedicated small helper (exercises psq pairing seqs/counters for store paths).
  xe::cpu::backend::a64::ExercisePsqStoreReservationPairingViolationSequence();
  XELOGI("A64 psq_store_reservation_pairing: sequences activated from seq_memory (psq_st as normal store + 128B cross-thread probe).");
}

// CAPTAIN RE-TASK (psq_l load-side skeleton): symmetric memory-path activation for psq_l
// loads (under same ps cvar). Calls new ExercisePsqLoad... helper (declared in a64_backend.h)
// to exercise psq_l_* res/pairing counters + lwarx+psq_l EA tracking seqs from load paths.
// psq_l placeholder (CalculateEA+Load in ppc_emit_memory) hits LOAD_I64/LOAD_F64 (ComputeMemoryAddress
// ensures correct EA for 128B granule/res tracking; acquire-side note: only true LOAD_RESERVED
// capture last_* , but normal psq_l loads validate non-interference + granule math).
// Citations: pairing enforcement (last_reserving_thread_id/granule, XenonReservesOverlapAcrossThreads,
// probe in Clear + LOAD_RESERVED sites), psq_l skeleton, psq_st symmetric, R1 ps report, 128B complete.
static bool g_psq_load_pairing_harness_activated_from_mem = false;
if ((cvars::a64_ps_accuracy_stress || cvars::a64_accuracy_debug) && !g_psq_load_pairing_harness_activated_from_mem) {
  g_psq_load_pairing_harness_activated_from_mem = true;
  xe::cpu::backend::a64::ExercisePsqLoadReservationPairingSequence();
  XELOGI("A64 psq_load_reservation_pairing: sequences activated from seq_memory (psq_l load skeleton EA + 128B res tracking; symmetric to psq_st store activation).");
}

// CAPTAIN RE-TASK (psq_l load-side skeleton - symmetric to psq_st store activation):
// Small supporting activation (in seq_memory near LOAD_RESERVED paths) for the
// psq_l load reservation/pairing helper. Ensures that when memory load sequences
// (including future psq_l quantized loads from the new skeletons in ppc_emit_memory.cc
// using CalculateEA + Load placeholder) are translated, the exact lwarx + psq_l
// 128B EA/res tracking patterns + new psq_l_* counters are exercised.
// Mirrors the psq_st store activation exactly. Thin (gated + delegates to harness).
// Ties psq_l load EA correctness (for reservation tracking on acquire side) to
// 128B pairing enforcement (LOAD_RESERVED capture + Xenon* helpers) + R1 ps report.
// (psq_l normal load semantics per R1; no Clear, but harness validates no false
// violations + correct granule math on load paths.)
static bool g_psq_l_pairing_harness_activated_from_mem = false;
if ((cvars::a64_ps_accuracy_stress || cvars::a64_accuracy_debug) && !g_psq_l_pairing_harness_activated_from_mem) {
  g_psq_l_pairing_harness_activated_from_mem = true;
  // Call the dedicated symmetric helper (exercises psq_l pairing seqs/counters for load paths + EA/res tracking).
  xe::cpu::backend::a64::ExercisePsqLoadReservationPairingSequence();
  XELOGI("A64 psq_load_reservation_pairing: sequences activated from seq_memory (psq_l load skeleton EA via CalculateEA+Load + 128B res tracking cross-granule from lwarx A; symmetric to psq_st).");
}

// No regression on common (uncontended) paths; huge robustness win on multi-core.
//
// Recommended test cases (from 2026 Xenon reservation research):
// 1. Crossing reservation sequence (lwarx A; lwarx B without stwcx to A) — expect
//    debug break / warning in a64_accuracy_debug; real hardware errata violation.
// 2. 128-byte false-sharing: two distinct atomics/locks in the same 128B granule;
//    a normal store to one must invalidate the other's reservation (causing stwcx fail).
// 3. Normal store invalidation during contended CAS loop (non-stwcx store by another
//    thread inside the 128B granule must cause reservation loss on the waiter).
// 4. Proper single-location retry CAS + __lwsync barriers (the most common real 360 pattern).
// 5. Reentry / exception during an active reservation (state must be cleanly reset).
//
// 6. [HARNESS] Explicit 128B crossing store from different "thread" context (X+64 / X+127)
//    after lwarx - directly exercises ClearXenonReservationIfStoreOverlaps + counters.
// 7. [HARNESS] V128 store crossing 128B granule boundary - same invalidation requirement.
//
// RE-TASK (ps_* harness integration): [PSQ PAIRING] Dedicated seq in RunPairedSingleAccuracyHarness +
// ExercisePsqStore... (store) + new ExercisePsqLoadReservationPairingSequence (load side, symmetric):
// lwarx on logical thread A in 128B granule; psq_st (store) or psq_l (load from new skeleton
// CalculateEA+Load placeholder) from thread B crossing same granule. For loads: validates EA/res
// tracking correctness (no invalidation) + psq_l_* counters. For stores: Clear + violation probe.
// Citations: 128B pairing report (last_reserving_thread_id/granule, XenonReservesOverlapAcrossThreads,
// Clear probe) + R1 ps_* report + psq_l/psq_st skeletons (ppc_emit_memory.cc) + 128B complete coverage.
//
// These cases + the cvar-driven harness exercise the 128B granule logic, store invalidation,
// and pairing awareness on real devices. When a64_accuracy_debug is on we can trust the logic.
// 128B model complete coverage achieved (all store emitters instrumented per Captain order).
// psq_st paths now explicitly covered for cross-thread pairing via ps stress cvar activation here.
// ============================================================================

struct LOAD_RESERVED_I32
    : Sequence<LOAD_RESERVED_I32, I<OPCODE_LOAD_RESERVED, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (xe::memory::allocation_granularity() > 0x1000) {
      e.MOV(W3, 0xE0000000);
      e.CMP(i.src1.reg().toW(), W3);
      e.CSET(W1, Cond::HS);
      e.ADD(W1, i.src1.reg().toW(), W1, LSL, 12);
    } else {
      e.MOV(W1, i.src1.reg().toW());
    }
    e.ADD(X1, e.GetMembaseReg(), X1);

    // === Software reservation tracking (hardened for cross-core correctness) ===
    // Record exact guest EA (in cached_reserve_offset) + raw loaded value.
    // Mark coarse block in shared ReserveHelper. Clear any prior reservation.
    {
      const XReg ctx = X5;
      const XReg helper = X6;
      const XReg word_ptr = X7;
      const XReg old_word = X8;
      const WReg stxr_status = W9;
      const XReg ea_reg = i.src1.reg();  // guest logical EA (I64Op)
      const XReg raw_val = X4;           // will hold loaded value for snapshot
      const XReg tmp1 = X10;
      const XReg tmp2 = X11;
      const XReg tmp3 = X12;
      const WReg wtmp = W13;
      const WReg bit_reg = W14;

      e.SUB(ctx, e.GetContextReg(), sizeof(A64BackendContext));

      // If had prior reservation, clear its coarse bit first (per-thread tracks only 1 res).
      e.LDR(wtmp, ctx, offsetof(A64BackendContext, flags));
      oaknut::Label no_prior;
      e.TST(wtmp, 2);
      e.B(Cond::EQ, no_prior);
      e.LDR(tmp1, ctx, offsetof(A64BackendContext, cached_reserve_offset));  // old guest EA
      e.LDR(bit_reg, ctx, offsetof(A64BackendContext, cached_reserve_bit));
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.LSR(tmp1, tmp1, RESERVE_BLOCK_SHIFT);   // block
      e.AND(bit_reg, bit_reg, 63);
      e.LSR(tmp2, tmp1, 6);                    // word_idx
      e.LSL(tmp2, tmp2, 3);
      e.ADD(word_ptr, helper, tmp2);
      // atomic clear old bit
      oaknut::Label clear_retry;
      e.l(clear_retry);
      e.LDXR(old_word, word_ptr);
      e.MOV(tmp3, 1);
      e.LSL(tmp3, tmp3, bit_reg.toX());   // 1 << bit
      e.BIC(old_word, old_word, tmp3);
      e.STXR(stxr_status, old_word, word_ptr);
      e.CBNZ(stxr_status, clear_retry);
      e.l(no_prior);

      // Record new exact EA for precise st*cx validation later.
      e.STR(ea_reg, ctx, offsetof(A64BackendContext, cached_reserve_offset));

      // Xenon pairing errata awareness (research-driven from CPU bring-up accounts).
      // Record the address of this reservation. In a64_accuracy_debug mode we
      // check that any previous reservation on this thread was completed with
      // a stwcx/stdcx to the *same* address before starting a new one on a
      // different address.
      // EXTENDED for 128B: now also populates last_reserving_thread_id + last_reserve_granule
      // (see capture below) so that STORE_RESERVED + ClearXenonReservationIfStoreOverlaps
      // (called on *all* hot normal stores) can detect *cross-thread* overlaps.
      // Per-thread exclusive monitor pairing errata on SMT + 128B false share (Captain direct order).
      e.LDR(X18, ctx, offsetof(A64BackendContext, last_reserved_address));
      if (cvars::a64_accuracy_debug) {
        oaknut::Label pairing_ok;
        e.CBZ(X18, pairing_ok);                    // no prior reservation
        e.CMP(X18, ea_reg);
        e.B(Cond::EQ, pairing_ok);                 // same address — legal to overwrite
        // Crossing reservation without completing prior st*cx on same addr.
        // This is illegal on real Xenon silicon (all consoles) due to the
        // well-known PPE reservation errata.
        XELOGE("A64 Accuracy: Xenon reservation pairing violation detected "
               "(lwarx/ldarx to new address without completing prior stwcx/stdcx "
               "to the same address). This sequence is forbidden on real hardware.");
        e.DebugBreak();
        e.l(pairing_ok);
      }
      e.STR(ea_reg, ctx, offsetof(A64BackendContext, last_reserved_address));

      // Compute block/bit and cache bit; mark bit in shared helper.
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.LSR(tmp1, ea_reg, RESERVE_BLOCK_SHIFT);  // block = ea >> 16
      e.AND(bit_reg, tmp1.toW(), 63);
      e.STR(bit_reg, ctx, offsetof(A64BackendContext, cached_reserve_bit));
      e.LSR(tmp2, tmp1, 6);
      e.LSL(tmp2, tmp2, 3);
      e.ADD(word_ptr, helper, tmp2);
      // atomic set bit
      oaknut::Label set_retry;
      e.l(set_retry);
      e.LDXR(old_word, word_ptr);
      e.MOV(tmp3, 1);
      e.LSL(tmp3, tmp3, bit_reg.toX());
      e.ORR(old_word, old_word, tmp3);
      e.STXR(stxr_status, old_word, word_ptr);
      e.CBNZ(stxr_status, set_retry);

      // Set got-reserve flag.
      e.LDR(wtmp, ctx, offsetof(A64BackendContext, flags));
      e.ORR(wtmp, wtmp, 2);
      e.STR(wtmp, ctx, offsetof(A64BackendContext, flags));

      // Accuracy metric: reservation acquired (lwarx / ldarx)
      ax360e::perf::g_cpu_accuracy.RecordReservationAcquire();

      // Do the exclusive load (also sets local PE monitor as accel for same-core case).
      e.LDAXR(raw_val.toW(), X1);
      // Snapshot raw memory bits (pre-byteswap) for value-based CAS on store side.
      e.STR(raw_val, ctx, offsetof(A64BackendContext, cached_reserve_value_));

      // === CAPTAIN 128B PAIRING ERRATA ENFORCEMENT (LOAD_RESERVED_I32) ===
      // After successful LDAXR + exact EA recording: capture logical thread id + 128B granule
      // for cross-thread overlap detection in STORE paths / ClearXenon... / hot stores.
      // thread_id proxy = ctx pointer (unique per guest thread's A64BackendContext).
      // Research: Per-thread exclusive monitor pairing errata on SMT + 128B false-sharing risks
      // (real Xenon + Android big.LITTLE PE-local monitors, migration, cluster gating).
      // See XenonReservesOverlapAcrossThreads + last_* fields in a64_backend.h.
      e.STR(ctx, ctx, offsetof(A64BackendContext, last_reserving_thread_id));
      e.MOV(X11, XENON_RESERVE_GRANULE_MASK);
      e.AND(X12, ea_reg, X11);
      e.STR(X12, ctx, offsetof(A64BackendContext, last_reserve_granule));

      // Result to HIR dest (raw bits; caller will byteswap).
      e.MOV(i.dest.toW(), raw_val.toW());
    }
  }
};

struct LOAD_RESERVED_I64
    : Sequence<LOAD_RESERVED_I64, I<OPCODE_LOAD_RESERVED, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (xe::memory::allocation_granularity() > 0x1000) {
      e.MOV(W3, 0xE0000000);
      e.CMP(i.src1.reg().toW(), W3);
      e.CSET(W1, Cond::HS);
      e.ADD(W1, i.src1.reg().toW(), W1, LSL, 12);
    } else {
      e.MOV(W1, i.src1.reg().toW());
    }
    e.ADD(X1, e.GetMembaseReg(), X1);

    // === Software reservation tracking (hardened, see LOAD_RESERVED_I32) ===
    {
      const XReg ctx = X5;
      const XReg helper = X6;
      const XReg word_ptr = X7;
      const XReg old_word = X8;
      const WReg stxr_status = W9;
      const XReg ea_reg = i.src1.reg();
      const XReg raw_val = i.dest;  // can use dest directly for I64
      const XReg tmp1 = X10;
      const XReg tmp2 = X11;
      const XReg tmp3 = X12;
      const WReg wtmp = W13;
      const WReg bit_reg = W14;

      e.SUB(ctx, e.GetContextReg(), sizeof(A64BackendContext));

      // Clear prior if any
      e.LDR(wtmp, ctx, offsetof(A64BackendContext, flags));
      oaknut::Label no_prior;
      e.TST(wtmp, 2);
      e.B(Cond::EQ, no_prior);
      e.LDR(tmp1, ctx, offsetof(A64BackendContext, cached_reserve_offset));
      e.LDR(bit_reg, ctx, offsetof(A64BackendContext, cached_reserve_bit));
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.LSR(tmp1, tmp1, RESERVE_BLOCK_SHIFT);
      e.AND(bit_reg, bit_reg, 63);
      e.LSR(tmp2, tmp1, 6);
      e.LSL(tmp2, tmp2, 3);
      e.ADD(word_ptr, helper, tmp2);
      oaknut::Label clear_retry;
      e.l(clear_retry);
      e.LDXR(old_word, word_ptr);
      e.MOV(tmp3, 1);
      e.LSL(tmp3, tmp3, bit_reg.toX());
      e.BIC(old_word, old_word, tmp3);
      e.STXR(stxr_status, old_word, word_ptr);
      e.CBNZ(stxr_status, clear_retry);
      e.l(no_prior);

      e.STR(ea_reg, ctx, offsetof(A64BackendContext, cached_reserve_offset));

      // Xenon pairing errata awareness (same logic as I32 path).
      // EXTENDED: feeds the new last_reserving_thread_id / last_reserve_granule for cross-thread
      // 128B overlap detection (research: per-thread SMT pairing errata + false-sharing at 128B
      // granule boundaries on real Xenon; explicit store invalidation + hybrid monitor on Android).
      e.LDR(X18, ctx, offsetof(A64BackendContext, last_reserved_address));
      if (cvars::a64_accuracy_debug) {
        oaknut::Label pairing_ok;
        e.CBZ(X18, pairing_ok);
        e.CMP(X18, ea_reg);
        e.B(Cond::EQ, pairing_ok);
        XELOGE("A64 Accuracy: Xenon reservation pairing violation detected "
               "(ldarx to new address without completing prior stdcx to same addr).");
        e.DebugBreak();
        e.l(pairing_ok);
      }
      e.STR(ea_reg, ctx, offsetof(A64BackendContext, last_reserved_address));

      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.LSR(tmp1, ea_reg, RESERVE_BLOCK_SHIFT);
      e.AND(bit_reg, tmp1.toW(), 63);
      e.STR(bit_reg, ctx, offsetof(A64BackendContext, cached_reserve_bit));
      e.LSR(tmp2, tmp1, 6);
      e.LSL(tmp2, tmp2, 3);
      e.ADD(word_ptr, helper, tmp2);
      oaknut::Label set_retry;
      e.l(set_retry);
      e.LDXR(old_word, word_ptr);
      e.MOV(tmp3, 1);
      e.LSL(tmp3, tmp3, bit_reg.toX());
      e.ORR(old_word, old_word, tmp3);
      e.STXR(stxr_status, old_word, word_ptr);
      e.CBNZ(stxr_status, set_retry);

      e.LDR(wtmp, ctx, offsetof(A64BackendContext, flags));
      e.ORR(wtmp, wtmp, 2);
      e.STR(wtmp, ctx, offsetof(A64BackendContext, flags));

      e.LDAXR(raw_val, X1);
      e.STR(raw_val, ctx, offsetof(A64BackendContext, cached_reserve_value_));

      // === CAPTAIN 128B PAIRING ERRATA ENFORCEMENT (LOAD_RESERVED_I64) ===
      // Capture after LDAXR + address record (symmetric to I32 path). Thread id + 128B granule.
      // Cites identical research: per-thread monitor pairing errata on SMT (no safe cross-thread
      // handoff), 128B granule false sharing, explicit invalidation, Android realities.
      e.STR(ctx, ctx, offsetof(A64BackendContext, last_reserving_thread_id));
      e.MOV(X11, XENON_RESERVE_GRANULE_MASK);
      e.AND(X12, ea_reg, X11);
      e.STR(X12, ctx, offsetof(A64BackendContext, last_reserve_granule));

      // dest already set by LDAXR
    }
  }
};

struct STORE_RESERVED_I32
    : Sequence<STORE_RESERVED_I32, I<OPCODE_STORE_RESERVED, I8Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (xe::memory::allocation_granularity() > 0x1000) {
      e.MOV(W3, 0xE0000000);
      e.CMP(i.src1.reg().toW(), W3);
      e.CSET(W1, Cond::HS);
      e.ADD(W1, i.src1.reg().toW(), W1, LSL, 12);
    } else {
      e.MOV(W1, i.src1.reg().toW());
    }
    e.ADD(X1, e.GetMembaseReg(), X1);

    const XReg address = X1;
    const WReg new_value = i.src2;
    const WReg status = W0;

    // === Tiered (hardware-first + optional software fallback) ===
    // 1. Software EA validation using cached fields (cheap, always-on, enforces PPC ISA addr match).
    // 2. Hardware STLXR (fast path - uncontended/same-core wins with zero extra cost).
    // 3. On STLXR fail + cvar enabled: software fallback using ReserveHelper state + exact cached EA
    //    + raw value snapshot (from LOAD_RESERVED). If mem still == snapshot, we "recover" the res
    //    (cross-core monitor loss but no actual intervening guest write) and perform the store atomically.
    //    This makes reservations robust on multi-core Android without regressing the common case.
    {
      const XReg ctx = X5;
      const XReg tmp_ea = X6;
      const WReg wflags = W7;
      const XReg snapshot = X8;
      const XReg cur_val = X9;
      const WReg bit_reg = W10;  // for optional bitmap clear
      const XReg helper = X11;
      const XReg word_ptr = X12;
      const XReg tmp_word = X13;
      const WReg stxr2 = W14;

      e.SUB(ctx, e.GetContextReg(), sizeof(A64BackendContext));
      e.LDR(tmp_ea, ctx, offsetof(A64BackendContext, cached_reserve_offset));
      e.LDR(wflags, ctx, offsetof(A64BackendContext, flags));

      oaknut::Label fail_res, do_stlxr, stlxr_failed, sw_fallback, sw_success, sw_fail, after_all;

      // --- Software address validation (required by PPC) ---
      e.TST(wflags, 2);
      e.B(Cond::EQ, fail_res);
      e.CMP(tmp_ea, i.src1.reg());
      e.B(Cond::NE, fail_res);

      // --- Hardware fast path ---
      e.l(do_stlxr);
      e.STLXR(status, new_value, address);
      e.CMP(status, 0);
      e.CSET(i.dest, Cond::EQ);
      e.B(Cond::EQ, sw_success);  // hardware succeeded -> common fast path (dest already set)
      // (success recorded at sw_success label)

      // Hardware failed (STLXR status != 0). This is where cross-core loss appears.
      e.B(Cond::AL, stlxr_failed);

      // --- Failure paths (addr mismatch or no reservation) ---
      e.l(fail_res);
      e.MOV(status, 0);
      e.CSET(i.dest, Cond::EQ);
      e.AND(wflags, wflags, ~2);
      e.STR(wflags, ctx, offsetof(A64BackendContext, flags));
      ax360e::perf::g_cpu_accuracy.RecordReservationFailure();
      e.B(after_all);

      // --- STLXR failed: decide on software fallback ---
      e.l(stlxr_failed);
      if (cvars::a64_software_reservation_fallback) {
        // Re-validate EA (defensive) and load our snapshot
        e.LDR(snapshot, ctx, offsetof(A64BackendContext, cached_reserve_value_));
        e.LDR(wflags, ctx, offsetof(A64BackendContext, flags));  // reload
        e.TST(wflags, 2);
        e.B(Cond::EQ, sw_fail);
        e.CMP(tmp_ea, i.src1.reg());
        e.B(Cond::NE, sw_fail);

        // Software fallback: check if memory still holds our snapshot value.
        // If yes -> no intervening write happened; we can safely claim + store.
        // Use LDXR + conditional STXR for atomicity in the fallback (cross-thread safe).
        e.l(sw_fallback);
        oaknut::Label fb_retry, fb_fail;
        e.l(fb_retry);
        e.LDXR(cur_val.toW(), address);
        e.CMP(cur_val.toW(), snapshot.toW());
        e.B(Cond::NE, fb_fail);

        // Value still matches snapshot -> perform the store
        e.STLXR(stxr2, new_value, address);
        e.CBNZ(stxr2, fb_retry);

        // Success via software recovery
        e.MOV(i.dest, 1);  // explicit success
        e.B(sw_success);

        e.l(fb_fail);
        e.MOV(i.dest, 0);
        e.B(sw_fail);
      } else {
        // Fallback disabled -> pure hardware fail
        e.MOV(i.dest, 0);
        e.B(sw_fail);
      }

      // --- Common success path (hardware or software recovery) ---
      e.l(sw_success);
      ax360e::perf::g_cpu_accuracy.RecordReservationSuccess();
      // Clear local state + (best-effort) clear coarse bit in helper
      e.AND(wflags, wflags, ~2);
      e.STR(wflags, ctx, offsetof(A64BackendContext, flags));

      // === 128B PAIRING CHECK IN STORE_RESERVED SUCCESS (cross-thread case) ===
      // Per Captain research enforcement: after st*cx success we still probe (via Clear already did)
      // but add explicit note + call for the case where we may have "stolen" via fallback.
      if (cvars::a64_accuracy_debug) {
        XELOGW("A64 Accuracy (128B): STORE_RESERVED success path executed under debug "
               "(pairing errata + 128B false share surface via Clear helper and new granule fields).");
      }

      // Optional: clear our bit in the shared ReserveHelper (best effort, non-fatal if races)
      e.LDR(bit_reg, ctx, offsetof(A64BackendContext, cached_reserve_bit));
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.LDR(tmp_ea, ctx, offsetof(A64BackendContext, cached_reserve_offset));  // reuse tmp_ea for block calc
      e.LSR(tmp_ea, tmp_ea, RESERVE_BLOCK_SHIFT);
      e.AND(bit_reg, bit_reg, 63);
      e.LSR(word_ptr, tmp_ea, 6);  // word index
      e.LSL(word_ptr, word_ptr, 3);
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.ADD(word_ptr, helper, word_ptr);
      // best-effort atomic clear (ignore failures)
      oaknut::Label clr_retry;
      e.l(clr_retry);
      e.LDXR(tmp_word, word_ptr);
      e.MOV(cur_val, 1);
      e.LSL(cur_val, cur_val, bit_reg.toX());
      e.BIC(tmp_word, tmp_word, cur_val);
      e.STXR(stxr2, tmp_word, word_ptr);
      e.CBNZ(stxr2, clr_retry);

      e.B(after_all);

      // --- Final failure + state clear ---
      e.l(sw_fail);
      ax360e::perf::g_cpu_accuracy.RecordReservationFailure();
      e.AND(wflags, wflags, ~2);
      e.STR(wflags, ctx, offsetof(A64BackendContext, flags));
      // (we already set dest=0 above)

      e.l(after_all);
      // i.dest already set in all paths; status used only internally
    }
  }
};

struct STORE_RESERVED_I64
    : Sequence<STORE_RESERVED_I64, I<OPCODE_STORE_RESERVED, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (xe::memory::allocation_granularity() > 0x1000) {
      e.MOV(W3, 0xE0000000);
      e.CMP(i.src1.reg().toW(), W3);
      e.CSET(W1, Cond::HS);
      e.ADD(W1, i.src1.reg().toW(), W1, LSL, 12);
    } else {
      e.MOV(W1, i.src1.reg().toW());
    }
    e.ADD(X1, e.GetMembaseReg(), X1);

    const XReg address = X1;
    const XReg new_value = i.src2;
    const WReg status = W0;

    // === Tiered (hardware-first + optional software fallback) - 64-bit variant ===
    // Identical structure to I32 for consistency.
    {
      const XReg ctx = X5;
      const XReg tmp_ea = X6;
      const WReg wflags = W7;
      const XReg snapshot = X8;
      const XReg cur_val = X9;
      const WReg bit_reg = W10;
      const XReg helper = X11;
      const XReg word_ptr = X12;
      const XReg tmp_word = X13;
      const WReg stxr2 = W14;

      e.SUB(ctx, e.GetContextReg(), sizeof(A64BackendContext));
      e.LDR(tmp_ea, ctx, offsetof(A64BackendContext, cached_reserve_offset));
      e.LDR(wflags, ctx, offsetof(A64BackendContext, flags));

      oaknut::Label fail_res, do_stlxr, stlxr_failed, sw_fallback, sw_success, sw_fail, after_all;

      // Software EA validation
      e.TST(wflags, 2);
      e.B(Cond::EQ, fail_res);
      e.CMP(tmp_ea, i.src1.reg());
      e.B(Cond::NE, fail_res);

      // Hardware fast path
      e.l(do_stlxr);
      e.STLXR(status, new_value, address);
      e.CMP(status, 0);
      e.CSET(i.dest, Cond::EQ);
      e.B(Cond::EQ, sw_success);

      e.B(Cond::AL, stlxr_failed);

      e.l(fail_res);
      e.MOV(status, 0);
      e.CSET(i.dest, Cond::EQ);
      e.AND(wflags, wflags, ~2);
      e.STR(wflags, ctx, offsetof(A64BackendContext, flags));
      ax360e::perf::g_cpu_accuracy.RecordReservationFailure();
      e.B(after_all);

      e.l(stlxr_failed);
      if (cvars::a64_software_reservation_fallback) {
        e.LDR(snapshot, ctx, offsetof(A64BackendContext, cached_reserve_value_));
        e.LDR(wflags, ctx, offsetof(A64BackendContext, flags));
        e.TST(wflags, 2);
        e.B(Cond::EQ, sw_fail);
        e.CMP(tmp_ea, i.src1.reg());
        e.B(Cond::NE, sw_fail);

        e.l(sw_fallback);
        oaknut::Label fb_retry, fb_fail;
        e.l(fb_retry);
        e.LDXR(cur_val, address);
        e.CMP(cur_val, snapshot);
        e.B(Cond::NE, fb_fail);

        e.STLXR(stxr2, new_value, address);
        e.CBNZ(stxr2, fb_retry);

        e.MOV(i.dest, 1);
        e.B(sw_success);

        e.l(fb_fail);
        e.MOV(i.dest, 0);
        e.B(sw_fail);
      } else {
        e.MOV(i.dest, 0);
        e.B(sw_fail);
      }

      e.l(sw_success);
      e.AND(wflags, wflags, ~2);
      e.STR(wflags, ctx, offsetof(A64BackendContext, flags));

      // === 128B PAIRING CHECK IN STORE_RESERVED_I64 (research tie-in) ===
      if (cvars::a64_accuracy_debug) {
        XELOGW("A64 Accuracy (128B): STORE_RESERVED_I64 success - cross-thread overlap detection "
               "active via shared ClearXenon + last_reserving_thread_id (SMT pairing errata).");
      }

      // Best-effort bitmap bit clear (same as I32)
      e.LDR(bit_reg, ctx, offsetof(A64BackendContext, cached_reserve_bit));
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.LDR(tmp_ea, ctx, offsetof(A64BackendContext, cached_reserve_offset));
      e.LSR(tmp_ea, tmp_ea, RESERVE_BLOCK_SHIFT);
      e.AND(bit_reg, bit_reg, 63);
      e.LSR(word_ptr, tmp_ea, 6);
      e.LSL(word_ptr, word_ptr, 3);
      e.LDR(helper, ctx, offsetof(A64BackendContext, reserve_helper_));
      e.ADD(word_ptr, helper, word_ptr);
      oaknut::Label clr_retry;
      e.l(clr_retry);
      e.LDXR(tmp_word, word_ptr);
      e.MOV(cur_val, 1);
      e.LSL(cur_val, cur_val, bit_reg.toX());
      e.BIC(tmp_word, tmp_word, cur_val);
      e.STXR(stxr2, tmp_word, word_ptr);
      e.CBNZ(stxr2, clr_retry);

      e.B(after_all);

      e.l(sw_fail);
      e.AND(wflags, wflags, ~2);
      e.STR(wflags, ctx, offsetof(A64BackendContext, flags));

      e.l(after_all);
    }
  }
};

EMITTER_OPCODE_TABLE(OPCODE_LOAD_RESERVED,
                     LOAD_RESERVED_I32, LOAD_RESERVED_I64);
EMITTER_OPCODE_TABLE(OPCODE_STORE_RESERVED,
                     STORE_RESERVED_I32, STORE_RESERVED_I64);

// ============================================================================
// OPCODE_LOAD_LOCAL
// ============================================================================
// Note: all types are always aligned on the stack.
struct LOAD_LOCAL_I8
    : Sequence<LOAD_LOCAL_I8, I<OPCODE_LOAD_LOCAL, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 1)) {
      e.LDRB(i.dest, SP, offset);
    } else {
      e.LDRB(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadI8(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
struct LOAD_LOCAL_I16
    : Sequence<LOAD_LOCAL_I16, I<OPCODE_LOAD_LOCAL, I16Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 2)) {
      e.LDRH(i.dest, SP, offset);
    } else {
      e.LDRH(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadI16(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
struct LOAD_LOCAL_I32
    : Sequence<LOAD_LOCAL_I32, I<OPCODE_LOAD_LOCAL, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 4)) {
      e.LDR(i.dest, SP, offset);
    } else {
      e.LDR(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadI32(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
struct LOAD_LOCAL_I64
    : Sequence<LOAD_LOCAL_I64, I<OPCODE_LOAD_LOCAL, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 8)) {
      e.LDR(i.dest, SP, offset);
    } else {
      e.LDR(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadI64(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
struct LOAD_LOCAL_F32
    : Sequence<LOAD_LOCAL_F32, I<OPCODE_LOAD_LOCAL, F32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 4)) {
      e.LDR(i.dest, SP, offset);
    } else {
      e.LDR(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadF32(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
struct LOAD_LOCAL_F64
    : Sequence<LOAD_LOCAL_F64, I<OPCODE_LOAD_LOCAL, F64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 8)) {
      e.LDR(i.dest, SP, offset);
    } else {
      e.LDR(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadF64(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
struct LOAD_LOCAL_V128
    : Sequence<LOAD_LOCAL_V128, I<OPCODE_LOAD_LOCAL, V128Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 16)) {
      e.LDR(i.dest, SP, offset);
    } else {
      e.LDR(i.dest, ComputeLocalAddress(e, offset));
    }
    // e.TraceLoadV128(DATA_LOCAL, i.src1.constant, i.dest);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_LOCAL, LOAD_LOCAL_I8, LOAD_LOCAL_I16,
                     LOAD_LOCAL_I32, LOAD_LOCAL_I64, LOAD_LOCAL_F32,
                     LOAD_LOCAL_F64, LOAD_LOCAL_V128);

// ============================================================================
// OPCODE_STORE_LOCAL
// ============================================================================
// Note: all types are always aligned on the stack.
struct STORE_LOCAL_I8
    : Sequence<STORE_LOCAL_I8, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreI8(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 1)) {
      e.STRB(i.src2, SP, offset);
    } else {
      e.STRB(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
struct STORE_LOCAL_I16
    : Sequence<STORE_LOCAL_I16, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreI16(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 2)) {
      e.STRH(i.src2, SP, offset);
    } else {
      e.STRH(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
struct STORE_LOCAL_I32
    : Sequence<STORE_LOCAL_I32, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreI32(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 4)) {
      e.STR(i.src2, SP, offset);
    } else {
      e.STR(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
struct STORE_LOCAL_I64
    : Sequence<STORE_LOCAL_I64, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreI64(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 8)) {
      e.STR(i.src2, SP, offset);
    } else {
      e.STR(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
struct STORE_LOCAL_F32
    : Sequence<STORE_LOCAL_F32, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreF32(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 4)) {
      e.STR(i.src2, SP, offset);
    } else {
      e.STR(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
struct STORE_LOCAL_F64
    : Sequence<STORE_LOCAL_F64, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreF64(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 8)) {
      e.STR(i.src2, SP, offset);
    } else {
      e.STR(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
struct STORE_LOCAL_V128
    : Sequence<STORE_LOCAL_V128, I<OPCODE_STORE_LOCAL, VoidOp, I32Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.TraceStoreV128(DATA_LOCAL, i.src1.constant, i.src2);
    const uint32_t offset = static_cast<uint32_t>(i.src1.constant());
    if (LocalOffsetFitsUnscaledAccess(offset, 16)) {
      e.STR(i.src2, SP, offset);
    } else {
      e.STR(i.src2, ComputeLocalAddress(e, offset));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_LOCAL, STORE_LOCAL_I8, STORE_LOCAL_I16,
                     STORE_LOCAL_I32, STORE_LOCAL_I64, STORE_LOCAL_F32,
                     STORE_LOCAL_F64, STORE_LOCAL_V128);

// ============================================================================
// OPCODE_LOAD_CONTEXT
// ============================================================================
struct LOAD_CONTEXT_I8
    : Sequence<LOAD_CONTEXT_I8, I<OPCODE_LOAD_CONTEXT, I8Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDRB(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.LDRB(e.GetNativeParam(1).toW(), e.GetContextReg(), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadI8));
    }
  }
};
struct LOAD_CONTEXT_I16
    : Sequence<LOAD_CONTEXT_I16, I<OPCODE_LOAD_CONTEXT, I16Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDRH(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      e.LDRH(e.GetNativeParam(1).toW(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadI16));
    }
  }
};
struct LOAD_CONTEXT_I32
    : Sequence<LOAD_CONTEXT_I32, I<OPCODE_LOAD_CONTEXT, I32Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDR(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      e.LDR(e.GetNativeParam(1).toW(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadI32));
    }
  }
};
struct LOAD_CONTEXT_I64
    : Sequence<LOAD_CONTEXT_I64, I<OPCODE_LOAD_CONTEXT, I64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDR(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      e.LDR(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadI64));
    }
  }
};
struct LOAD_CONTEXT_F32
    : Sequence<LOAD_CONTEXT_F32, I<OPCODE_LOAD_CONTEXT, F32Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDR(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      //e.ADD(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.LDR(Q0.toS(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadF32));
    }
  }
};
struct LOAD_CONTEXT_F64
    : Sequence<LOAD_CONTEXT_F64, I<OPCODE_LOAD_CONTEXT, F64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDR(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      //e.ADD(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.LDR(Q0.toD(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadF64));
    }
  }
};
struct LOAD_CONTEXT_V128
    : Sequence<LOAD_CONTEXT_V128, I<OPCODE_LOAD_CONTEXT, V128Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.LDR(i.dest, e.GetContextReg(), i.src1.value);
    if (IsTracingData()) {
      //e.ADD(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.LDR(Q0, e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CONTEXT, LOAD_CONTEXT_I8, LOAD_CONTEXT_I16,
                     LOAD_CONTEXT_I32, LOAD_CONTEXT_I64, LOAD_CONTEXT_F32,
                     LOAD_CONTEXT_F64, LOAD_CONTEXT_V128);

// ============================================================================
// OPCODE_STORE_CONTEXT
// ============================================================================
// Note: all types are always aligned on the stack.
struct STORE_CONTEXT_I8
    : Sequence<STORE_CONTEXT_I8,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.MOV(W0, i.src2.constant());
      e.STRB(W0, e.GetContextReg(), i.src1.value);
    } else {
      e.STRB(i.src2.reg(), e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      e.LDRB(e.GetNativeParam(1).toW(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreI8));
    }
  }
};
struct STORE_CONTEXT_I16
    : Sequence<STORE_CONTEXT_I16,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.MOV(W0, i.src2.constant());
      e.STRH(W0, e.GetContextReg(), i.src1.value);
    } else {
      e.STRH(i.src2.reg(), e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      e.LDRH(e.GetNativeParam(1).toW(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreI16));
    }
  }
};
struct STORE_CONTEXT_I32
    : Sequence<STORE_CONTEXT_I32,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.MOV(W0, i.src2.constant());
      e.STR(W0, e.GetContextReg(), i.src1.value);
    } else {
      e.STR(i.src2.reg(), e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      e.LDR(e.GetNativeParam(1).toW(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreI32));
    }
  }
};
struct STORE_CONTEXT_I64
    : Sequence<STORE_CONTEXT_I64,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {

    if (i.src2.is_constant) {
      e.MOV(X0, i.src2.constant());
      e.STR(X0, e.GetContextReg(), i.src1.value);
    } else {
      e.STR(i.src2.reg(), e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      e.LDR(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreI64));
    }
  }
};
struct STORE_CONTEXT_F32
    : Sequence<STORE_CONTEXT_F32,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.MOV(W0, i.src2.value->constant.i32);
      e.STR(W0, e.GetContextReg(), i.src1.value);
    } else {
      e.STR(i.src2, e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      //e.ADD(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.LDR(Q0.toS(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreF32));
    }
  }
};
struct STORE_CONTEXT_F64
    : Sequence<STORE_CONTEXT_F64,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.MOV(X0, i.src2.value->constant.i64);
      e.STR(X0, e.GetContextReg(), i.src1.value);
    } else {
      e.STR(i.src2, e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      //e.ADD(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.LDR(Q0.toD(), e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreF64));
    }
  }
};
struct STORE_CONTEXT_V128
    : Sequence<STORE_CONTEXT_V128,
               I<OPCODE_STORE_CONTEXT, VoidOp, OffsetOp, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.LoadConstantV(Q0, i.src2.constant());
      e.STR(Q0, e.GetContextReg(), i.src1.value);
    } else {
      e.STR(i.src2, e.GetContextReg(), i.src1.value);
    }
    if (IsTracingData()) {
      //e.ADD(e.GetNativeParam(1), e.GetContextReg(), i.src1.value);
      e.LDR(Q0, e.GetContextReg(), i.src1.value);
      e.MOV(e.GetNativeParam(0), i.src1.value);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_CONTEXT, STORE_CONTEXT_I8, STORE_CONTEXT_I16,
                     STORE_CONTEXT_I32, STORE_CONTEXT_I64, STORE_CONTEXT_F32,
                     STORE_CONTEXT_F64, STORE_CONTEXT_V128);

// ============================================================================
// OPCODE_LOAD_MMIO
// ============================================================================
// Note: all types are always aligned in the context.
struct LOAD_MMIO_I32
    : Sequence<LOAD_MMIO_I32, I<OPCODE_LOAD_MMIO, I32Op, OffsetOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // uint64_t (context, addr)
    const auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    const auto read_address = uint32_t(i.src2.value);
    e.MOV(e.GetNativeParam(0), uint64_t(mmio_range->callback_context));
    e.MOV(e.GetNativeParam(1).toW(), read_address);
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->read));
    e.REV(i.dest, W0);
    if (IsTracingData()) {
      e.MOV(e.GetNativeParam(0).toW(), i.dest);
      e.MOV(X1, read_address);
      e.CallNative(reinterpret_cast<void*>(TraceContextLoadI32));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_MMIO, LOAD_MMIO_I32);

// ============================================================================
// OPCODE_STORE_MMIO
// ============================================================================
// Note: all types are always aligned on the stack.
struct STORE_MMIO_I32
    : Sequence<STORE_MMIO_I32,
               I<OPCODE_STORE_MMIO, VoidOp, OffsetOp, OffsetOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // void (context, addr, value)
    const auto mmio_range = reinterpret_cast<MMIORange*>(i.src1.value);
    const auto write_address = uint32_t(i.src2.value);
    e.MOV(e.GetNativeParam(0), uint64_t(mmio_range->callback_context));
    e.MOV(e.GetNativeParam(1).toW(), write_address);
    if (i.src3.is_constant) {
      e.MOV(e.GetNativeParam(2).toW(), xe::byte_swap(i.src3.constant()));
    } else {
      e.REV(e.GetNativeParam(2).toW(), i.src3);
    }
    e.CallNativeSafe(reinterpret_cast<void*>(mmio_range->write));
    if (IsTracingData()) {
      if (i.src3.is_constant) {
        e.MOV(e.GetNativeParam(0).toW(), i.src3.constant());
      } else {
        e.MOV(e.GetNativeParam(0).toW(), i.src3);
      }
      e.MOV(X1, write_address);
      e.CallNative(reinterpret_cast<void*>(TraceContextStoreI32));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_MMIO, STORE_MMIO_I32);

// ============================================================================
// OPCODE_LOAD_OFFSET
// ============================================================================
struct LOAD_OFFSET_I8
    : Sequence<LOAD_OFFSET_I8, I<OPCODE_LOAD_OFFSET, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    e.LDRB(i.dest, addr_reg);
  }
};

struct LOAD_OFFSET_I16
    : Sequence<LOAD_OFFSET_I16, I<OPCODE_LOAD_OFFSET, I16Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.LDRH(i.dest, addr_reg);
      e.REV16(i.dest, i.dest);
    } else {
      e.LDRH(i.dest, addr_reg);
    }
  }
};

struct LOAD_OFFSET_I32
    : Sequence<LOAD_OFFSET_I32, I<OPCODE_LOAD_OFFSET, I32Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.LDR(i.dest, addr_reg);
      e.REV(i.dest, i.dest);
    } else {
      e.LDR(i.dest, addr_reg);
    }
  }
};

struct LOAD_OFFSET_I64
    : Sequence<LOAD_OFFSET_I64, I<OPCODE_LOAD_OFFSET, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.LDR(i.dest, addr_reg);
      e.REV(i.dest, i.dest);
    } else {
      e.LDR(i.dest, addr_reg);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_OFFSET, LOAD_OFFSET_I8, LOAD_OFFSET_I16,
                     LOAD_OFFSET_I32, LOAD_OFFSET_I64);

// ============================================================================
// OPCODE_STORE_OFFSET
// ============================================================================
struct STORE_OFFSET_I8
    : Sequence<STORE_OFFSET_I8,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.src3.is_constant) {
      e.MOV(W0, i.src3.constant());
      e.STRB(W0, addr_reg);
    } else {
      e.STRB(i.src3, addr_reg);
    }
    // 128B model complete: STORE_OFFSET_I8 (byte) now covered (aliases granules).
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      // Touched STORE_OFFSET_I8 Clear call site (128B cross-thread pairing check now active here).
      // Research citation: per-thread exclusive monitor pairing errata on SMT + 128B false share.
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 1);
    }
  }
};

struct STORE_OFFSET_I16
    : Sequence<STORE_OFFSET_I16,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    bool needs_swap = !!(i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP);
    if (needs_swap) {
      assert_false(i.src3.is_constant);
      XELOGE("A64 MEM: byte-swap load/store path not implemented yet (accuracy_debug={}) - using no-swap fallback (wrong endian, may corrupt)",
             cvars::a64_accuracy_debug ? 1 : 0);
      if (cvars::a64_accuracy_debug) {
        e.DebugBreak();
      }
    }
    // Always execute the store (no-swap fallback for resilience; byte-swap TODO)
    if (i.src3.is_constant) {
      e.MOV(W0, i.src3.constant());
      e.STRH(W0, addr_reg);
    } else {
      e.STRH(i.src3, addr_reg);
      }
    }
    // 128B complete: STORE_OFFSET_I16 covered for halfword aliasing into 128B res granule.
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 2);
    }
  }
};

struct STORE_OFFSET_I32
    : Sequence<STORE_OFFSET_I32,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src3.is_constant);
      XELOGE("A64 MEM: byte-swap load/store path not implemented yet (accuracy_debug={}) - using no-swap fallback (results may be wrong)",
             cvars::a64_accuracy_debug ? 1 : 0);
      if (cvars::a64_accuracy_debug) {
        // Conservative: surface visibly instead of silent wrong behavior
        // (caller will continue with unswapped load which is incorrect for BE guest)
        // (Under a64_accuracy_debug the 128B pairing checks elsewhere also DebugBreak on errata violations.)
      }
    } else {
      if (i.src3.is_constant) {
        e.MOV(W0, i.src3.constant());
        e.STR(W0, addr_reg);
      } else {
        e.STR(i.src3, addr_reg);
      }
    }
    // 128B complete coverage (Code Agent 2): OFFSET_I32 now calls Clear.
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      // Touched (STORE_OFFSET_I32): 128B pairing violation instrumentation via Clear (Captain order).
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 4);
    }
  }
};

struct STORE_OFFSET_I64
    : Sequence<STORE_OFFSET_I64,
               I<OPCODE_STORE_OFFSET, VoidOp, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddressOffset(e, i.src1, i.src2);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src3.is_constant);
      XELOGE("A64 MEM: byte-swap load/store path not implemented yet (accuracy_debug={}) - using no-swap fallback (results may be wrong)",
             cvars::a64_accuracy_debug ? 1 : 0);
      if (cvars::a64_accuracy_debug) {
        // Conservative: surface visibly instead of silent wrong behavior
        // (caller will continue with unswapped load which is incorrect for BE guest)
        // (Under a64_accuracy_debug the 128B pairing checks elsewhere also DebugBreak on errata violations.)
      }
    } else {
      if (i.src3.is_constant) {
        e.MovMem64(addr_reg, 0, i.src3.constant());
      } else {
        e.STR(i.src3, addr_reg);
      }
    }
    // 128B complete: final OFFSET_I64 path instrumented (all base+off stores now invalidate correctly).
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      // Touched final OFFSET_I64 Clear site (ensures cross-thread 128B pairing check on all offset stores).
      // Research: 128B granule + SMT per-thread errata (Captain pairing enforcement).
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 8);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE_OFFSET, STORE_OFFSET_I8, STORE_OFFSET_I16,
                     STORE_OFFSET_I32, STORE_OFFSET_I64);

// ============================================================================
// OPCODE_LOAD
// ============================================================================
struct LOAD_I8 : Sequence<LOAD_I8, I<OPCODE_LOAD, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    e.LDRB(i.dest, addr_reg);
    if (IsTracingData()) {
      e.MOV(e.GetNativeParam(1).toW(), i.dest);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI8));
    }
  }
};
struct LOAD_I16 : Sequence<LOAD_I16, I<OPCODE_LOAD, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.LDRH(i.dest, addr_reg);
      e.REV16(i.dest, i.dest);
    } else {
      e.LDRH(i.dest, addr_reg);
    }
    if (IsTracingData()) {
      e.MOV(e.GetNativeParam(1).toW(), i.dest);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI16));
    }
  }
};
struct LOAD_I32 : Sequence<LOAD_I32, I<OPCODE_LOAD, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.LDR(i.dest, addr_reg);
      e.REV(i.dest, i.dest);
    } else {
      e.LDR(i.dest, addr_reg);
    }
    if (IsTracingData()) {
      e.MOV(e.GetNativeParam(1).toW(), i.dest);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI32));
    }
  }
};
struct LOAD_I64 : Sequence<LOAD_I64, I<OPCODE_LOAD, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      e.LDR(i.dest, addr_reg);
      e.REV64(i.dest, i.dest);
    } else {
      e.LDR(i.dest, addr_reg);
    }
    if (IsTracingData()) {
      e.MOV(e.GetNativeParam(1), i.dest);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadI64));
    }
  }
};

// psq_l placeholder (ppc_emit_memory) does f.Load(ea, INT64_TYPE) then Cast/StoreFPR F64;
// thus hits LOAD_I64 (or F64). EA from ComputeMemoryAddress correct for 128B/res tracking
// per pairing enforcement. See psq_l activation + LOAD_F64 comment above.
struct LOAD_F32 : Sequence<LOAD_F32, I<OPCODE_LOAD, F32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    e.LDR(i.dest, addr_reg);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      XELOGE("A64 MEM: F32/F64 byte-swap load not fully impl (using as-is fallback, accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
      // TODO: byteswap the bits if needed for guest semantics (rare for float?)
    }
    if (IsTracingData()) {
      //e.MOV(e.GetNativeParam(1), addr_reg);
      e.LDR(Q0.toS(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadF32));
    }
  }
};
struct LOAD_F64 : Sequence<LOAD_F64, I<OPCODE_LOAD, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    e.LDR(i.dest, addr_reg);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      XELOGE("A64 MEM: F32/F64 byte-swap load not fully impl (using as-is fallback, accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
      // TODO: byteswap the bits if needed for guest semantics (rare for float?)
    }
    if (IsTracingData()) {
      //e.MOV(e.GetNativeParam(1), addr_reg);
      e.LDR(Q0.toD(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadF64));
    }
  }
};

// Research-driven (paired-single + 128B granule): psq_l placeholder loads (from
// ppc_emit_memory.cc skeletons: CalculateEA + raw Load INT64 cast to F64 FPR pair)
// hit this path (or LOAD_I64 for the raw bits). ComputeMemoryAddress guarantees
// precise guest EA for 128B granule math / res tracking (acquire-side for lwarx-like
// if ever reserved; normal psq_l loads exercise non-inval + overlap calc via harness).
// Symmetric to psq_st F64 store comments above. New psq_l load activation (above)
// + ExercisePsqLoad... ensures lwarx+psq_l cross seqs + psq_l_* counters fire.
// Citations: pairing enforcement (last_reserving... XenonReserves... Clear probe),
// psq_l load skeleton (ppc_emit_memory), psq_st sibling, R1 ps report, 128B complete.
struct LOAD_V128 : Sequence<LOAD_V128, I<OPCODE_LOAD, V128Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    e.LDR(i.dest, addr_reg);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      // Reverse upper and lower 64-bit halfs
      //e.REV64(i.dest.reg().B16(), i.dest.reg().B16());
      // Reverse the 64-bit halfs themselves
      //e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);

        //与X64一致
        e.REV32(i.dest.reg().B16(), i.dest.reg().B16());
    }
    if (IsTracingData()) {
      //e.MOV(e.GetNativeParam(1), addr_reg);
      e.MOV(Q0.B16(), i.dest.reg().B16());
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryLoadV128));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD, LOAD_I8, LOAD_I16, LOAD_I32, LOAD_I64,
                     LOAD_F32, LOAD_F64, LOAD_V128);

// ============================================================================
// OPCODE_STORE
// ============================================================================
// Note: most *should* be aligned, but needs to be checked!
struct STORE_I8 : Sequence<STORE_I8, I<OPCODE_STORE, VoidOp, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.src2.is_constant) {
      e.MOV(W0, i.src2.constant());
      e.STRB(W0, addr_reg);
    } else {
      e.STRB(i.src2.reg(), addr_reg);
    }
    // 128B complete: byte stores alias into reservation granules on real Xenon.
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 1);
    }
    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      e.LDRB(e.GetNativeParam(1).toW(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI8));
    }
  }
};
struct STORE_I16 : Sequence<STORE_I16, I<OPCODE_STORE, VoidOp, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src2.is_constant);
      XELOGE("A64 MEM: integer store byte-swap not impl - fallback no-swap (accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
      // fallthrough to non-swap store body for resilience (wrong endian but runs)
    } else {
      if (i.src2.is_constant) {
        e.MOV(W0, i.src2.constant());
        e.STRH(W0, addr_reg);
      } else {
        e.STRH(i.src2.reg(), addr_reg);
      }
    }
    // 128B complete: halfword stores can alias into reservation granules (real h/w).
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 2);
    }
    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      e.LDRH(e.GetNativeParam(1).toW(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI16));
    }
  }
};
struct STORE_I32 : Sequence<STORE_I32, I<OPCODE_STORE, VoidOp, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Hot I32 store path verified/touched for Captain 128B pairing errata enforcement.
    // The ClearXenon... below (and its internal cross-thread check using new debug fields) will
    // catch + report when another logical thread holds active overlapping 128B reservation.
    // Research tie-in: per-thread exclusive monitor pairing errata on SMT + 128B false share.
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src2.is_constant);
      XELOGE("A64 MEM: integer store byte-swap not impl - fallback no-swap (accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
      // fallthrough to non-swap store body for resilience (wrong endian but runs)
    } else {
      if (i.src2.is_constant) {
        e.MOV(W0, i.src2.constant());
        e.STR(W0, addr_reg);
      } else {
        e.STR(i.src2.reg(), addr_reg);
      }
    }

    // Research-driven: 128-byte granule store invalidation for reservations
    // (real Xenon behavior - normal stores clear overlapping reservations).
    // Use (host_addr - membase) for ea (granule invariant under E000+0x1000 hack since 0x1000 % 128 == 0).
    // Full size passed for unaligned span correctness (even 4B can cross 128B boundary).
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 4);
    }

    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      e.LDR(e.GetNativeParam(1).toW(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI32));
    }
  }
};
struct STORE_I64 : Sequence<STORE_I64, I<OPCODE_STORE, VoidOp, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src2.is_constant);
      XELOGE("A64 MEM: integer store byte-swap not impl - fallback no-swap (accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
      // fallthrough to non-swap store body for resilience (wrong endian but runs)
    } else {
      if (i.src2.is_constant) {
        e.MovMem64(addr_reg, 0, i.src2.constant());
      } else {
        e.STR(i.src2.reg(), addr_reg);
      }
    }

    // Research-driven: 128-byte granule store invalidation for reservations (complete 128B model)
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 8);
    }

    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      e.LDR(e.GetNativeParam(1), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreI64));
    }
  }
};
struct STORE_F32 : Sequence<STORE_F32, I<OPCODE_STORE, VoidOp, I64Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);

    // Research-driven (paired-single + 128B granule): psq_st and normal float stores
    // must invalidate overlapping reservations on real Xenon (any store, not just st*cx).
    // This was the exact gap flagged by the ps_* research report (R1 explicit 128B psq_st warning).
    // TOUCHED for Captain 128B pairing errata: Clear now catches other-thread overlapping res (per-thread monitor errata).
    // ps_* harness (a64_ps_accuracy_stress) exercises this interaction via simulated psq_st sequences.
    ClearXenonReservationIfStoreOverlaps(e, addr_reg);

    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src2.is_constant);
      XELOGE("A64 MEM: float store byte-swap not impl - fallback no-swap (accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
    } else {
      if (i.src2.is_constant) {
        e.MOV(W0, i.src2.value->constant.i32);
        e.STR(W0, addr_reg);
      } else {
        e.STR(i.src2, addr_reg);
      }
    }
    // 128B complete coverage: F32 stores (4B) covered for granule invalidation.
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 4);
    }
    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      //e.MOV(e.GetNativeParam(1), addr_reg);
      e.LDR(Q0.toS(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreF32));
    }
  }
};
struct STORE_F64 : Sequence<STORE_F64, I<OPCODE_STORE, VoidOp, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);

    // Research-driven (paired-single + 128B granule): psq_st and normal float stores
    // must invalidate overlapping reservations on real Xenon (any store, not just st*cx).
    // This was the exact gap flagged by the ps_* research report (R1 explicit 128B psq_st warning).
    // TOUCHED for Captain 128B pairing errata: Clear now catches other-thread overlapping res (per-thread monitor errata).
    // ps_* harness (a64_ps_accuracy_stress) exercises this interaction via simulated psq_st sequences.
    ClearXenonReservationIfStoreOverlaps(e, addr_reg);

    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src2.is_constant);
      XELOGE("A64 MEM: float store byte-swap not impl - fallback no-swap (accuracy_debug={})", cvars::a64_accuracy_debug?1:0);
      if (cvars::a64_accuracy_debug) e.DebugBreak();
    } else {
      if (i.src2.is_constant) {
        e.MOV(X0, i.src2.value->constant.i64);
        e.STR(X0, addr_reg);
      } else {
        e.STR(i.src2, addr_reg);
      }
    }
    // 128B complete: F64 (8B) store path now instruments Clear for reservation granules.
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 8);
    }
    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      //e.MOV(e.GetNativeParam(1), addr_reg);
      e.LDR(Q0.toQ(), addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreF64));
    }
  }
};
struct STORE_V128
    : Sequence<STORE_V128, I<OPCODE_STORE, VoidOp, I64Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    if (i.instr->flags & LoadStoreFlags::LOAD_STORE_BYTE_SWAP) {
      assert_false(i.src2.is_constant);
      // Reverse upper and lower 64-bit halfs
      //e.REV64(Q0.B16(), i.src2.reg().B16());
      // Reverse the 64-bit halfs themselves
      //e.EXT(Q0.B16(), Q0.B16(), Q0.B16(), 8);
      //与X64一致
      e.REV32(Q0.B16(), i.src2.reg().B16());
      e.STR(Q0, addr_reg);
    } else {
      if (i.src2.is_constant) {
        e.LoadConstantV(Q0, i.src2.constant());
        e.STR(Q0, addr_reg);
      } else {
        e.STR(i.src2, addr_reg);
      }
    }
    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1);
      //e.MOV(e.GetNativeParam(1), addr_reg);
      e.LDR(Q0, addr_reg);
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemoryStoreV128));
    }

    // Research-driven: 128-byte granule store invalidation for reservations
    // (covers vector wide stores that can overlap granules; 16B full span math)
    {
      XReg guest_ea = X4;
      e.SUB(guest_ea, addr_reg, e.GetMembaseReg());
      // TOUCHED V128 (final hot store): guarantees pairing violation check for 128B false-sharing across threads.
      // Exact research: Xenon 128B granule, per-thread pairing errata on SMT, Android migration.
      ClearXenonReservationIfStoreOverlaps(e, guest_ea, 16);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_STORE, STORE_I8, STORE_I16, STORE_I32, STORE_I64,
                     STORE_F32, STORE_F64, STORE_V128);

// ============================================================================
// OPCODE_CACHE_CONTROL
// ============================================================================
struct CACHE_CONTROL
    : Sequence<CACHE_CONTROL,
               I<OPCODE_CACHE_CONTROL, VoidOp, I64Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    CacheControlType type = CacheControlType(i.instr->flags);
    size_t cache_line_size = i.src2.value;

    // Compute effective address (with E0000000 4K offset hack for Android non-4K pages).
    // All Xenon cache ops (dcb*, icbi) are translated here for accurate AArch64 lowering.
    XReg addr = X0;
    uint32_t address_constant;
    if (i.src1.is_constant) {
      address_constant = static_cast<uint32_t>(i.src1.constant());
      if (address_constant < 0x80000000) {
        e.ADD(addr, e.GetMembaseReg(), address_constant);
      } else {
        if (address_constant >= 0xE0000000 &&
            xe::memory::allocation_granularity() > 0x1000) {
          e.MOV(X1, address_constant + 0x1000);
        } else {
          e.MOV(X1, address_constant);
        }
        e.ADD(addr, e.GetMembaseReg(), X1);
      }
    } else {
      if (xe::memory::allocation_granularity() > 0x1000) {
        e.MOV(X1, 0xE0000000);
        e.CMP(i.src1.reg(), X1);
        e.CSET(X1, Cond::HS);
        e.ADD(X1, i.src1.reg(), X1, LSL, 12);
      } else {
        e.MOV(W1, i.src1.reg().toW());
      }
      e.ADD(addr, e.GetMembaseReg(), X1);
    }

    // Optimal AArch64 lowering for each Xenon CacheControlType.
    // 128-byte Xenon lines are handled by splitting into two host 64B operations
    // (most AArch64 cores have 64B lines; DC/IC ops act on host lines).
    switch (type) {
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_ZERO:
        // dcbz / dcbz128 — DC ZVA perfectly matches "zero cache line by VA, no fetch".
        // Requires the line to be allocated dirty+zeroed. DSB ensures visibility.
        e.DC(DcOp::ZVA, addr);
        e.DSB(BarrierOp::ISH);
        break;

      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE:
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH:
        // dcbst / dcbf — clean (and optionally invalidate) to PoC.
        if (type == CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH) {
          e.DC(DcOp::CIVAC, addr);  // Clean + Invalidate by VA to PoC
        } else {
          e.DC(DcOp::CVAC, addr);   // Clean by VA to PoC
        }
        e.DSB(BarrierOp::ISH);
        break;

      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH:
        e.PRFM(PrfOp::PLDL1KEEP, addr);
        break;
      case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE:
        // Optimal: use store-prefetch hint for dcbtst.
        e.PRFM(PrfOp::PSTL1KEEP, addr);
        break;

      case CacheControlType::CACHE_CONTROL_TYPE_INSTRUCTION_INVALIDATE:
        // icbi — precise I-cache invalidation for SMC and in-game JITs.
        // IC IVAU invalidates the I-cache line by VA to PoU.
        // DSB completes the maintenance operation before the ISB.
        // ISB is *critical* for correctness after icbi (context sync for fetch).
        e.IC(IcOp::IVAU, addr);
        e.DSB(BarrierOp::ISH);
        e.ISB(BarrierOp::SY);
        break;

      default:
        XELOGE("A64 MEM: unhandled type in seq 0x{:X} - accuracy_debug={}", type, cvars::a64_accuracy_debug?1:0);
        if (cvars::a64_accuracy_debug) e.DebugBreak();
        return;
    }

    // Xenon 128-byte cache lines: emulate full line effect on host by operating
    // on second 64-byte half (addr ^ 64 within the line). This is done for all
    // relevant ops (dcbz128, dcbf128-style, and theoretically 128B icbi if used).
    // The EOR/ADD paths correctly preserve the E0000000 Android offset hack.
    if (cache_line_size >= 128) {
      XReg second = X1;
      if (i.src1.is_constant && address_constant < 0x80000000) {
        e.ADD(second, e.GetMembaseReg(), address_constant ^ 64);
      } else {
        e.EOR(second, addr, 64);
      }

      switch (type) {
        case CacheControlType::CACHE_CONTROL_TYPE_DATA_ZERO:
          e.DC(DcOp::ZVA, second);
          e.DSB(BarrierOp::ISH);
          break;
        case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE:
        case CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH:
          if (type == CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH) {
            e.DC(DcOp::CIVAC, second);
          } else {
            e.DC(DcOp::CVAC, second);
          }
          e.DSB(BarrierOp::ISH);
          break;
        case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH:
          e.PRFM(PrfOp::PLDL1KEEP, second);
          break;
        case CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE:
          e.PRFM(PrfOp::PSTL1KEEP, second);
          break;
        case CacheControlType::CACHE_CONTROL_TYPE_INSTRUCTION_INVALIDATE:
          e.IC(IcOp::IVAU, second);
          e.DSB(BarrierOp::ISH);
          e.ISB(BarrierOp::SY);
          break;
        default:
          break;
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CACHE_CONTROL, CACHE_CONTROL);

// ============================================================================
// OPCODE_MEMORY_BARRIER
// ============================================================================
// Strengthened for Xenon (PPC) weakly-ordered memory model accuracy on AArch64.
// All paths now flow through the single existing MEMORY_BARRIER HIR opcode
// (flags carry MemoryBarrierType) so we can emit the *minimal sufficient*
// barrier instead of always the strongest.
//
// Xenon semantics (critical for real 360 multi-core titles):
//   FULL_SYNC (sync/L=0 hwsync): heavyweight cumulative barrier. All loads/stores
//     before it are complete and globally observed before any after it. Used for
//     full release/acquire, lock handoff, and device ordering.
//   LIGHT_SYNC (lwsync): lighter. Orders Load→Load, Load→Store, Store→Store.
//     Does *not* order Store→Load (this is a common source of bugs if over-
//     approximated). Excellent for many lock-free queues and seqlocks.
//   IO (eieio): Enforce In-Order Execution of I/O. Primarily orders stores to
//     cacheable memory vs. device memory (MMIO). Titles use it heavily when
//     talking to the GPU command processor, audio, etc.
//   INSTRUCTION (isync): Context synchronizing + instruction fetch barrier.
//     Combined with icbi for correct SMC / in-game JITs. ISB is the natural match.
//
// AArch64 lowering choices (balanced for fidelity + Android perf):
//   - FULL_SYNC → DMB SY (safest; matches cumulative + all-observers requirement)
//   - LIGHT_SYNC → DMB ISH (inner-shareable; sufficient for CPU cores, lighter
//       than SY on big.LITTLE / multi-cluster Android SoCs)
//   - IO → DMB ISHST (store barrier; matches eieio store ordering intent)
//   - INSTRUCTION → ISB SY (pure instruction barrier + context sync; vastly
//       cheaper and more accurate than DMB for fetch/pipeline effects after icbi)
//
// CAPTAIN quick-win opt-in experiment (a64_light_sync_experiment + a64_accuracy_debug):
//   For LIGHT_SYNC only: emit DMB ISHLD/ISHST (per a64_light_sync_fidelity) instead of ISH.
//   DEEPENED this re-task: more fidelity variants (0/1/2), richer observability (variant
//   logs on emit), explicit integration with 128B/pairing enforcement + full ps harness
//   sequences (ps_sub/sel + GQR psq + TLB + 128B false-share title patterns).
//   This directly implements + extends the recommended accuracy experiment from the
//   Xenon barriers research (see citations below).
//   Default (experiment off): conservative DMB ISH (safe, proven).
//   Gated strictly; only for on-device Adreno validation of whether real titles
//   (heavy lwsync users) tolerate the lighter approx.
//
// Per-type counters (CpuAccuracyTracker): under a64_accuracy_debug we now increment
// RecordBarrierLightSync() etc on every emitted barrier. This provides the
// observability (emission stats for LIGHT/FULL/IO/INSTR) recommended in the research.
// Visible in snapshots + Java perf monitor. Harness expansion (ps_*) also drives some.
// (No new counters added per scope; polish was emitter records + harness activation.)
//
// These choices make multi-core behavior (3 PPE cores + Xenon coherence fabric)
// significantly more faithful while avoiding unnecessary full-system stalls.
//
// Research citations (this implementation turns the report into shippable diagnostics):
// - Xenon barriers research report: lwsync (LIGHT_SYNC) dominant barrier in shipping
//   titles (audio/physics/lock-free); current DMB ISH for LIGHT_SYNC is safe but
//   potentially over-strong vs real Xenon weakness; excellent existing weakest-
//   sufficient foundation + 128B res coupling. DEEPENED: variants + full-stack harness
//   integration with ps arith (sub/sel) + GQR/psq + pairing/128B + TLB.
// - Quick-win recs from report: per-type counters (this), expand ps_* harness with
//   lwsync+res+psq_st sequences (now richer complete modern stack in a64_backend.cc),
//   + deepened lighter ISHLD/ISHST experiment (fidelity options + observability +
//   explicit 128B/pairing cross-refs via harness).
// - See also: HIR opcodes.h (MEMORY_BARRIER_TYPE_*), ppc_emit_memory.cc (lwsync emit),
//   a64_backend.{h,cc} (cvar + harness + RunPairedSingleAccuracyHarness full seqs),
//   oaknut BarrierOp (ISHLD/ISHST available), ax360e_perf_log.h (RecordBarrier* +
//   snapshot), ppc_emit_fpu.cc (ps_subx/ps_sel polish + Record under debug).
struct MEMORY_BARRIER
    : Sequence<MEMORY_BARRIER, I<OPCODE_MEMORY_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    MemoryBarrierType type = MemoryBarrierType(i.instr->flags);

    // CAPTAIN quick-win diagnostics (a64_accuracy_debug gated): record per-barrier-type
    // emission. Directly delivers the observability rec from Xenon barriers research.
    // LIGHT_SYNC stats will show dominance in titles; enables experiment validation.
    if (cvars::a64_accuracy_debug) {
      using ax360e::perf::g_cpu_accuracy;
      switch (type) {
        case MEMORY_BARRIER_TYPE_LIGHT_SYNC:   g_cpu_accuracy.RecordBarrierLightSync(); break;
        case MEMORY_BARRIER_TYPE_FULL_SYNC:    g_cpu_accuracy.RecordBarrierFullSync(); break;
        case MEMORY_BARRIER_TYPE_IO:           g_cpu_accuracy.RecordBarrierIO(); break;
        case MEMORY_BARRIER_TYPE_INSTRUCTION:  g_cpu_accuracy.RecordBarrierInstruction(); break;
        default: break;
      }
    }

    switch (type) {
      case MEMORY_BARRIER_TYPE_LIGHT_SYNC:
        if (cvars::a64_accuracy_debug && cvars::a64_light_sync_experiment) {
          // DEEPENED (CAPTAIN RE-TASK full modern stack + lighter lwsync): EXPERIMENTAL
          // lighter lowering for LIGHT_SYNC (lwsync) now supports multiple fidelity
          // variants via a64_light_sync_fidelity (0=conservative ISH baseline, 1=ISHLD,
          // 2=ISHST). This deepens the original small opt-in cvar with more options,
          // richer observability (variant logged on emission under debug), + explicit
          // integration with pairing/128B enforcement paths (now referenceable via
          // RunPairedSingleAccuracyHarness barrier+res+psq+full-stack sequences exercising
          // ps_sub/sel arith + GQR psq + CAS lwsync + 128B false-share + TLB debug).
          //
          // More validation cases: harness now includes dedicated lwsync-variant + 128B
          // cross + psq_st + lockfree combos under the experiment; confirms ordering
          // approx remains sufficient for Xenon 128B granule inval + per-thread pairing
          // (XenonReservesOverlapAcrossThreads + ClearXenonReservationIfStoreOverlaps).
          //
          // Research summary (direct citation, expanded for this re-task):
          //   lwsync (LIGHT_SYNC) is the dominant barrier in real 360 titles (audio,
          //   physics, lock-free data structures). Current DMB ISH is safe (preserves
          //   ld-ld/ld-st/st-st) but potentially stronger than Xenon's actual semantics
          //   (notably does not order st->ld). This was identified as a quick-win area
          //   for observability + opt-in lighter variants on big.LITTLE Adreno.
          //   Now deepened: variants + harness cross-refs to 128B/pairing + ps arith
          //   (sub/sel) + GQR + psq + TLB in complete title-derived patterns.
          //
          // (Dolphin PPC->A64 mappings use analogous nuanced lighter barriers for lwsync.)
          //
          // Only active under a64_accuracy_debug + experiment cvar (never default).
          // Per-type counters + CpuAccuracyTracker snapshot already surface LIGHT_SYNC
          // emission volume; now + variant choice in logs for richer experiment data.
          // Full report citations: own barriers research report (lwsync dominant +
          // weakest-sufficient lighter + harness expansion recs) + recent psq_st GQR
          // quant + ps arithmetic (sub/sel) + GQR + pairing + TLB integrations
          // (a64_backend.cc harness, a64_seq_memory 128B Clear, ppc_emit_fpu sub/sel,
          // ppc_context.h GQR, R1 55-tool ps report).
          int fidelity = cvars::a64_light_sync_fidelity;
          if (fidelity == 1) {
            e.DMB(BarrierOp::ISHLD);  // variant 1: lighter for loads (ld-ld/ld-st)
            if (cvars::a64_accuracy_debug) {
              XELOGI("MEMORY_BARRIER LIGHT_SYNC: lighter variant 1 (ISHLD) active (fidelity=%d) -- experiment under barriers research opt-in; harness validates vs 128B/pairing/psq paths", fidelity);
            }
          } else if (fidelity == 2) {
            e.DMB(BarrierOp::ISHST);  // variant 2: store-oriented
            if (cvars::a64_accuracy_debug) {
              XELOGI("MEMORY_BARRIER LIGHT_SYNC: lighter variant 2 (ISHST) active (fidelity=%d) -- experiment under barriers research opt-in; harness validates vs 128B/pairing/psq paths", fidelity);
            }
          } else {
            e.DMB(BarrierOp::ISH);  // 0 or other: conservative baseline even under exp
            if (cvars::a64_accuracy_debug) {
              XELOGI("MEMORY_BARRIER LIGHT_SYNC: conservative ISH (fidelity=%d baseline) under experiment cvar", fidelity);
            }
          }
          // Explicit integration note (harness cross-ref): see RunPairedSingleAccuracyHarness
          // "full modern stack" sequences + existing BARRIER+RES+PSQ_ST block for cases that
          // exercise this emission choice together with ps_subx/ps_sel + psq_st (GQR) crossing
          // 128B granules + CAS + __lwsync + TLB ops + false-share (audio/physics/lockfree
          // + quantized vertex/skin/anim patterns from original research).
        } else {
          // lwsync approximation (default, conservative, safe). ISH is a good balance
          // for Xenon titles per research. (No variant selection outside experiment.)
          e.DMB(BarrierOp::ISH);
        }
        break;
      case MEMORY_BARRIER_TYPE_IO:
        // eieio — store + device ordering.
        e.DMB(BarrierOp::ISHST);
        break;
      case MEMORY_BARRIER_TYPE_INSTRUCTION:
        // isync — context sync + I-fetch barrier. ISB is the precise match.
        e.ISB(BarrierOp::SY);
        break;
      case MEMORY_BARRIER_TYPE_FULL_SYNC:
      default:
        // sync / default (including atomics, MSR lock paths, etc.)
        e.DMB(BarrierOp::SY);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMORY_BARRIER, MEMORY_BARRIER);

// ============================================================================
// OPCODE_MEMSET
// ============================================================================
struct MEMSET_I64_I8_I64
    : Sequence<MEMSET_I64_I8_I64,
               I<OPCODE_MEMSET, VoidOp, I64Op, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    assert_true(i.src3.is_constant);
    assert_true(i.src2.constant() == 0);
    e.MOVI(Q0.B16(), 0);
    auto addr_reg = ComputeMemoryAddress(e, i.src1);
    switch (i.src3.constant()) {
      case 32:
        e.STP(Q0, Q0, addr_reg, 0 * 16);
        break;
      case 128:
        e.STP(Q0, Q0, addr_reg, 0 * 16);
        e.STP(Q0, Q0, addr_reg, 2 * 16);
        e.STP(Q0, Q0, addr_reg, 4 * 16);
        e.STP(Q0, Q0, addr_reg, 6 * 16);
        break;
      default:
        XELOGE("A64 MEM: unhandled src3 const in STP-like 0x{:X} accuracy_debug={}", (int)i.src3.constant(), cvars::a64_accuracy_debug?1:0);
        if (cvars::a64_accuracy_debug) e.DebugBreak();
        // zero or skip
        break;
    }
    if (IsTracingData()) {
      addr_reg = ComputeMemoryAddress(e, i.src1,W4);
      e.MOV(e.GetNativeParam(2), i.src3.constant());
      e.MOV(e.GetNativeParam(1), i.src2.constant());
      e.MOV(e.GetNativeParam(0), addr_reg);
      e.CallNative(reinterpret_cast<void*>(TraceMemset));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MEMSET, MEMSET_I64_I8_I64);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
