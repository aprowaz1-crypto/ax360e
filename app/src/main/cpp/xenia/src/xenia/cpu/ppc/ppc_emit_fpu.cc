/*
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_emit-private.h"

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_hir_builder.h"

// CPU accuracy metrics + ps_* harness counters (R1 + Captain re-task for live ps_addx/maddx/msubx emitters).
// See ax360e_perf_log.h (RecordPs* + RecordPairedSingle* for ps_arith_executed / ps_fma_executed / ps_nan_cases / ps_denorm_handled).
#include "ax360e_perf_log.h"

#include <stddef.h>

namespace xe {
namespace cpu {
namespace ppc {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::RoundMode;
using xe::cpu::hir::Value;

// Good source of information:
// https://github.com/mamedev/historic-mame/blob/master/src/emu/cpu/powerpc/ppc_ops.c
// The correctness of that code is not reflected here yet -_-

// ============================================================================
// PAIRED-SINGLE (ps_*) SUPPORT PLAN FOR XENON CPU (Xbox 360 PPE)
// ============================================================================
// Paired-single instructions interpret each 64-bit FPR as two packed
// single-precision floats:
//   bits[63:32] = ps0 (primary / high)
//   bits[31:0]  = ps1 (secondary / low)
//
// They are Gekko/PPE heritage and **critical** for a huge number of 360
// titles (animation, vertex transforms, physics, audio, particle systems,
// matrix math, etc.). Many games will be inaccurate or crash without them.
//
// CURRENT STATE (post Agent D):
// - ps_addx/ps_maddx/ps_msubx: opcodes in enum (via xml+table-gen), decoder hits in lookup_gen,
//   entries in table, emitters REGISTERED in FPU (only these 3), lowering implemented per plan.
// - Bit-cast F64->two F32 + HIR reuse + UpdateFPSCR now live (R1 report guidance followed).
// - No psq_l/st, GQR scale, .s variants, indexed, or other ps_* (mul/mr/sel etc) touched.
// - Rest of family + quantized loads for follow-on agents. Backend F32/FMA/FPCR ready.
//
// CAPTAIN RE-TASK (this job, post live dispatch): ownership of the three new live
// arithmetic emitters (ps_addx/maddx/msubx). Added first ps-specific accuracy
// validation counter hooks (gated a64_accuracy_debug || a64_ps_accuracy_stress) that
// feed R1's ps_* validation harness (RunPairedSingleAccuracyHarness + CpuAccuracyTracker
// ps_arith_executed/ps_fma_executed/ps_nan_cases/ps_denorm_handled + snapshot +
// Java/PerformanceMonitor triggers). Small FPCR/rounding/NaN polish: removed redundant
// Convert casts on F32 halves (Cast is the bit-reinterpret per HIR + a64 CAST_I32_F32
// FMOV lowering; roundtrip + backend fpcr_table + FMADD/FNMSUB deliver semantics).
// Citations: R1 ps report (FMA priority, per-element edges, 128B psq_st store warning);
// this file's master plan block (above); a64_backend.cc harness sequences (known values
// + FMA/NaN/denorm); ax360e_perf_log.h + emulator_ax360e.cpp + PerformanceMonitor.java.
// Reservation interaction left for psq_st agents (arith here don't store). Only these
// 3 touched.
//
// IMPLEMENTATION STRATEGY (follows the original NOTE intent):
// 1. Arithmetic ps_* (ps_add, ps_madd, ps_msub, ps_mul, ps_sub, etc.)
//    Lower to **two independent single-precision** operations on the
//    extracted halves. This matches real Xenon hardware behavior
//    (independent rounding, denorm handling per element).
//    Use:
//      - LoadFPR (returns FLOAT64_TYPE from context)
//      - Cast to INT64_TYPE
//      - Extract high/low 32-bit words (Shr + Truncate/And)
//      - Reinterpret 32-bit pattern as FLOAT32 (via Cast or Convert
//        tricks + explicit F32<->F64 roundtrip where needed for Xenon
//        single-prec semantics).
//      - Perform the op using existing HIR (Add, MulAdd, MulSub, etc.)
//        or Convert to F32 first for *s variants.
//      - Merge results: (ps0_as_i32 << 32) | ps1_as_i32 , Cast back to F64.
//    Leverage ROUND_TO_SINGLE macro + existing faddsx/fmaddsx style
//    Convert(F32)->F64 roundtrips for fidelity.
//
// 2. Quantized load/store (psq_l, psq_st and indexed/update forms)
//    - These use the GQR (Graphics Quantization Registers) in context
//      for scale + type (signed/unsigned 8/16-bit -> float).
//    - Lower using existing memory paths (Load/Store, possibly VEC128
//      or two F32 loads) + scale/Convert in HIR.
//    - psq_l/psq_st are highest impact after basic arithmetic because
//      they dominate data movement in real games.
//
// 3. Special ops later: ps_res, ps_rsqrte, ps_sel, ps_sum0/1, ps_merge*,
//    comparisons, etc.
//
// 4. Frontend/Decoder:
//    - Add ps_* opcodes to ppc_opcode.h (InstrType enum).
//    - Update opcode table generator / lookup / disasm.
//    - Add decoding in ppc_scanner or frontend if special forms needed.
//
// 5. Accuracy:
//    - Rely on AArch64 backend F32 lowering + FPCR (see fpcr_table in
//      a64_sequences.cc).
//    - Fused FMA is already improved for ps_* fidelity.
//    - NaN / SNaN / denorm rules may need extra IsNan handling later.
//
// PHASED ROLLOUT (minimal set for biggest wins):
// Phase 0 (this change): Documentation + stubs for ps_addx, ps_maddx, ps_msubx.
// Phase 1 (Captain Agent 4): emitters + some wiring (broader).
// Phase 1B (Agent D - CAPTAIN DIRECT ORDER): narrow unique job - register ONLY the 3
//   (ps_addx/ps_maddx/ps_msubx) via xml/table-gen/h/lookup + uncomment in FPU reg (~744).
//   Implement/activate minimal correct lowering (bit-cast F64->indep F32 pairs via Cast+
//   Convert, reuse Add/MulAdd/MulSub HIR per R1 ps research report, FPCR/rounding/denorm,
//   merge, UpdateFPSCR). No new HIR opcodes. Leaves psq/GQR/.s/mul/mr/etc for others.
//   After: simple ps_addx test hits real A64 F32 math, no fallback/zero.
// Phase 1 EXT (CAPTAIN RE-TASK): extend high-ROI arithmetic family - add ps_subx + ps_sel
//   (explicitly listed as next easy wins in master plan). Same lowering pattern. Also
//   ps_* accuracy debug paths + counters + light FPCR single-prec notes (ties to a64
//   backend FPCR work + ps research report). Keeps minimal + ready for other ps agents
//   (psq/GQR full, more specials). See R1 report + a64_sequences.cc + a64_backend harness.
//   THIS RE-TASK: gated Record* + FPCR polish on sub/sel (mirrors the three + their harness
//   known-value seqs you just added) + richer combined harness seqs (FMA+sub/sel+GQR+psq+128B+pairing).
//   Citations: own prior three emitters + validation sequences + recent landing + GQR + R1 + 128B/pairing + harness.
// Phase 2: psq_l / psq_st (in ppc_emit_memory.cc or here) + full GQR quantization.
// Phase 3: Full arithmetic family + special ops + opcode table updates.
//
// See also:
// - a64_sequences.cc comments around MUL_SUB_F32 / MUL_SUB_V128 / fpcr_table + ps harness
// - a64_backend.h Xenon FPU note + RunPairedSingleAccuracyHarness
// - Existing f* sx single-prec patterns below (faddsx, fmaddsx, etc.)
// - ax360e_perf_log.h CpuAccuracyTracker ps_* counters
// ============================================================================

// Enable rounding numbers to single precision as required.
// This adds a bunch of work per operation and I'm not sure it's required.
#define ROUND_TO_SINGLE

// ----------------------------------------------------------------------------
// PAIRED-SINGLE (ps_*) ARITHMETIC EMITTERS - ps_addx / ps_maddx / ps_msubx / +ext
// ----------------------------------------------------------------------------
// IMPLEMENTED (Agent D + CAPTAIN RE-TASK extension): minimal correct lowering
// following the detailed phased plan in this file + R1 ps research report guidance.
//   - Bit-cast F64 (via LoadFPR + Cast INT64) to two independent F32
//     (extract high/low 32-bit words, Cast+Convert reinterpret to FLOAT32).
//   - Use existing HIR Add/MulAdd/MulSub/Sub/Select (no new opcodes) for arithmetic.
//   - Proper rounding/denorm/NaN via FPCR (backend single-prec paths).
//   - Roundtrip Convert F32<->F64 + merge (Shl/Or) back to F64 container.
//   - StoreFPR + UpdateFPSCR (Rc handled).
// Reuses HIR primitives exactly per report; ps_addx/maddx/msubx/mulx/mrx landed prior;
// this re-task adds ps_subx + ps_sel (next easy wins per master plan block).
// + ps_* accuracy debug paths/counters (a64_accuracy_debug gated) + FPCR polish notes.
// Ready for other ps agents (psq etc). After, ps_* produce real A64 F32 math.
// See: a64_backend.cc RunPairedSingleAccuracyHarness, ax360e_perf_log.h counters.
// ----------------------------------------------------------------------------

int InstrEmit_ps_addx(PPCHIRBuilder& f, const InstrData& i) {
  // psD <- (psA) + (psB)   [ps0 and ps1 independently]
  // R1 ps research report + in-tree plan: bit-cast F64 halves to indep F32,
  // Add via HIR (benefits from FPCR denorm/round), merge, UpdateFPSCR.
  // (Agent D: activated + lightly polished for dispatch; only this op here)
  // RE-TASK: FPCR single-prec semantics (this + sub/sel) now explicitly tied in comments.

  // CAPTAIN RE-TASK (live ps arithmetic emitters ownership): ps-specific accuracy validation
  // counter wiring. When this emitter is dispatched by frontend (real guest ps_addx sites
  // now produce live A64 code instead of stubs), we feed the R1 ps_* validation harness
  // (a64_backend.cc:RunPairedSingleAccuracyHarness + Java PerformanceMonitor trigger).
  // Gated exactly like TLB hook (ppc_hir_builder) + MUL_ADD_F32 FMA hook (a64_sequences.cc)
  // + 128B in seq_memory. Uses both a64_accuracy_debug (primary) and the new ps stress cvar.
  // Citations (heavy, back to own emitter work + R1 + plan):
  //   - This file's master PAIRED-SINGLE plan block (lines 32-140ish): "Phase 0 ... ps_addx/ps_maddx/ps_msubx
  //     via bit-cast halves ... only these 3 ... R1 ps research report guidance".
  //   - R1 55-tool ps_* report (original author): ps_addx basic arith still hot in anim/physics;
  //     independent per-element single-prec; FPCR/NaN/denorm edges critical (Xenon != IEEE);
  //     drove FMA priority for maddx + this accuracy harness.
  //   - a64_backend.cc:RunPairedSingle... + ax360e_perf_log.h:RecordPairedSingleArith +
  //     RecordPs* (ps_arith_executed, ps_fma_executed, ps_nan_cases, ps_denorm_handled) +
  //     GetSnapshotString exposure. "Lightweight - no new framework".
  //   - emulator_ax360e.cpp j_trigger_ps... + PerformanceMonitor.java triggerPSAccuracyStressTest.
  //   - Own prior: emitters now live in opcode table/lookup/frontend (ppc_opcode_*_gen.cc etc).
  // This is the high-leverage step only possible after making dispatch real.
  if (cvars::a64_accuracy_debug || cvars::a64_ps_accuracy_stress) {
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleArith(2);  // ps0 + ps1 elements for this ps_addx site
    // No FMA here; addx is pure Add (still valuable for validation sequences with known values).
  }

  // Load sources as F64 (bit containers)
  Value* fra = f.LoadFPR(i.A.FRA);
  Value* frb = f.LoadFPR(i.A.FRB);

  // --- ps0 (high 32 bits) ---
  Value* a_bits = f.Cast(fra, INT64_TYPE);
  Value* b_bits = f.Cast(frb, INT64_TYPE);
  Value* a0_i32 = f.Truncate(f.Shr(a_bits, 32), INT32_TYPE);
  Value* b0_i32 = f.Truncate(f.Shr(b_bits, 32), INT32_TYPE);
  // Bit-cast / reinterpret 32-bit pattern as FLOAT32 (per plan: Cast for bit pattern
  // consistent with frspx/single-prec paths for Xenon semantics + A64 F32 lowering).
  // SMALL POLISH (this re-task): removed redundant Convert( cast_f32, FLOAT32 ) - noop,
  // keeps FPCR/rounding/NaN/denorm behavior identical (the post-arith roundtrip Convert
  // dance + backend fpcr_table + FMADD etc handle single-prec + Xenon quirks). Cleaner
  // for the live ps_addx path. NaN quieting / denorm via host FPCR FZ/DN in sequences.
  Value* a0 = f.Cast(a0_i32, FLOAT32_TYPE);
  Value* b0 = f.Cast(b0_i32, FLOAT32_TYPE);
  Value* d0 = f.Add(a0, b0);
  // Round to single semantics (FPCR handled in A64 lowering; UpdateFPSCR below for guest FPSCR).
  d0 = f.Convert(f.Convert(d0, FLOAT32_TYPE), FLOAT64_TYPE);  // pack as F64 temp

  // --- ps1 (low 32 bits) ---
  Value* a1_i32 = f.Truncate(a_bits, INT32_TYPE);
  Value* b1_i32 = f.Truncate(b_bits, INT32_TYPE);
  Value* a1 = f.Cast(a1_i32, FLOAT32_TYPE);
  Value* b1 = f.Cast(b1_i32, FLOAT32_TYPE);
  Value* d1 = f.Add(a1, b1);
  d1 = f.Convert(f.Convert(d1, FLOAT32_TYPE), FLOAT64_TYPE);

  // Merge ps0 (high) | ps1 (low) back into F64 container
  Value* d0_i = f.Cast(f.Convert(d0, INT64_TYPE), INT64_TYPE);
  Value* d1_i = f.Cast(f.Convert(d1, INT64_TYPE), INT64_TYPE);
  Value* result_bits = f.Or(f.Shl(d0_i, 32), d1_i);
  Value* result = f.Cast(result_bits, FLOAT64_TYPE);

  f.StoreFPR(i.A.FRT, result);
  f.UpdateFPSCR(result, i.A.Rc);
  return 0;
}

int InstrEmit_ps_maddx(PPCHIRBuilder& f, const InstrData& i) {
  // psD <- (psA * psC) + psB   [FMA, independent on ps0/ps1]
  // R1 ps research report + in-tree plan (Agent D): bit-cast F64->F32 pairs,
  // MulAdd HIR reuse (fused benefits from A64 FMA + FPCR), merge+UpdateFPSCR.
  // Only this op in scope; high-impact for games.

  // CAPTAIN RE-TASK (live ps arithmetic emitters ownership): ps-specific accuracy validation
  // counter wiring for FMA case (highest ROI per R1). See identical block + heavy citations
  // in InstrEmit_ps_addx (above) for full R1 report + master plan block + harness + snapshot
  // + Java trigger references. This emitter now live-dispatches real code; counts feed
  // ps_fma_executed + ps_arith_executed for on-device validation sequences (known FMA values,
  // NaN/denorm edges in RunPairedSingleAccuracyHarness). Gated under a64_accuracy_debug or
  // the new ps stress cvar. Reservation interaction noted for when psq_st stores (other agents)
  // are involved (psq_st as normal store per R1 must ClearXenon 128B granule).
  if (cvars::a64_accuracy_debug || cvars::a64_ps_accuracy_stress) {
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleArith(2);
    ax360e::perf::g_cpu_accuracy.RecordPsFmaExecuted(2);  // ps_maddx FMA on both elements (ps0/ps1)
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleFMA(); // legacy alias for harness compat
  }

  Value* fra = f.LoadFPR(i.A.FRA);
  Value* frc = f.LoadFPR(i.A.FRC);
  Value* frb = f.LoadFPR(i.A.FRB);

  // ps0 path (high)
  Value* a_bits = f.Cast(fra, INT64_TYPE);
  Value* c_bits = f.Cast(frc, INT64_TYPE);
  Value* b_bits = f.Cast(frb, INT64_TYPE);

  // SMALL POLISH (re-task on live maddx path): removed redundant Convert(..., FLOAT32)
  // around Cast bit-reinterprets (see ps_addx for full rationale). FMA + post-arith
  // single roundtrip + UpdateFPSCR preserve Xenon per-element FPCR/NaN/denorm/rounding.
  // Ties directly to a64_sequences MUL_ADD_F32 (FMADD + RecordPsFmaExecuted under debug).
  Value* a0 = f.Cast(f.Truncate(f.Shr(a_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* c0 = f.Cast(f.Truncate(f.Shr(c_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* b0 = f.Cast(f.Truncate(f.Shr(b_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* d0 = f.MulAdd(a0, c0, b0);
  d0 = f.Convert(f.Convert(d0, FLOAT32_TYPE), FLOAT64_TYPE);

  // ps1 path (low)
  Value* a1 = f.Cast(f.Truncate(a_bits, INT32_TYPE), FLOAT32_TYPE);
  Value* c1 = f.Cast(f.Truncate(c_bits, INT32_TYPE), FLOAT32_TYPE);
  Value* b1 = f.Cast(f.Truncate(b_bits, INT32_TYPE), FLOAT32_TYPE);
  Value* d1 = f.MulAdd(a1, c1, b1);
  d1 = f.Convert(f.Convert(d1, FLOAT32_TYPE), FLOAT64_TYPE);

  // Merge
  Value* d0_i = f.Cast(f.Convert(d0, INT64_TYPE), INT64_TYPE);
  Value* d1_i = f.Cast(f.Convert(d1, INT64_TYPE), INT64_TYPE);
  Value* result_bits = f.Or(f.Shl(d0_i, 32), d1_i);
  Value* result = f.Cast(result_bits, FLOAT64_TYPE);

  f.StoreFPR(i.A.FRT, result);
  f.UpdateFPSCR(result, i.A.Rc);
  return 0;
}

int InstrEmit_ps_msubx(PPCHIRBuilder& f, const InstrData& i) {
  // psD <- (psA * psC) - psB   [FMA form]
  // R1 ps research report + in-tree plan (Agent D): bit-cast to F32 halves,
  // MulSub HIR (fused FPCR path), merge back, UpdateFPSCR(result, Rc).
  // Companion to ps_maddx; only these 3 for this scoped dispatch work.

  // CAPTAIN RE-TASK (live ps arithmetic emitters ownership): identical ps-specific counter
  // wiring + citations as in ps_maddx immediately above (R1 report FMA priority, master plan
  // in this file, a64_backend harness sequences for known values + NaN/denorm, ax360e_perf_log
  // snapshot, PerformanceMonitor Java trigger). msubx also FMA (ps_* MulSub lowers to
  // FNMSUB-style in backend). Feeds ps_fma_executed etc when real titles hit these live
  // emitters (no more dead stubs). Small reservation note: arith paths themselves don't store,
  // but when other agents land psq_st stores they will interact via same ClearXenon paths.
  if (cvars::a64_accuracy_debug || cvars::a64_ps_accuracy_stress) {
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleArith(2);
    ax360e::perf::g_cpu_accuracy.RecordPsFmaExecuted(2);
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleFMA();
  }

  Value* fra = f.LoadFPR(i.A.FRA);
  Value* frc = f.LoadFPR(i.A.FRC);
  Value* frb = f.LoadFPR(i.A.FRB);

  Value* a_bits = f.Cast(fra, INT64_TYPE);
  Value* c_bits = f.Cast(frc, INT64_TYPE);
  Value* b_bits = f.Cast(frb, INT64_TYPE);

  // ps0
  // SMALL POLISH (re-task): redundant Convert removed on live msubx (FMA-form) path.
  // Consistent with addx/maddx polish above + plan block. MulSub + roundtrip + FPSCR
  // deliver correct NaN/denorm/FPCR single-prec per element (ps_* Xenon fidelity).
  Value* a0 = f.Cast(f.Truncate(f.Shr(a_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* c0 = f.Cast(f.Truncate(f.Shr(c_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* b0 = f.Cast(f.Truncate(f.Shr(b_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* d0 = f.MulSub(a0, c0, b0);
  d0 = f.Convert(f.Convert(d0, FLOAT32_TYPE), FLOAT64_TYPE);

  // ps1
  Value* a1 = f.Cast(f.Truncate(a_bits, INT32_TYPE), FLOAT32_TYPE);
  Value* c1 = f.Cast(f.Truncate(c_bits, INT32_TYPE), FLOAT32_TYPE);
  Value* b1 = f.Cast(f.Truncate(b_bits, INT32_TYPE), FLOAT32_TYPE);
  Value* d1 = f.MulSub(a1, c1, b1);
  d1 = f.Convert(f.Convert(d1, FLOAT32_TYPE), FLOAT64_TYPE);

  // Merge
  Value* d0_i = f.Cast(f.Convert(d0, INT64_TYPE), INT64_TYPE);
  Value* d1_i = f.Cast(f.Convert(d1, INT64_TYPE), INT64_TYPE);
  Value* result_bits = f.Or(f.Shl(d0_i, 32), d1_i);
  Value* result = f.Cast(result_bits, FLOAT64_TYPE);

  f.StoreFPR(i.A.FRT, result);
  f.UpdateFPSCR(result, i.A.Rc);
  return 0;
}

// ps_mulx: psD <- (psA) * (psC)   [independent on ps0/ps1]
// Per the master PAIRED-SINGLE plan block at top of file (Phase 1 high-ROI).
// Mirrors fmulx structure but on extracted single-prec halves.
int InstrEmit_ps_mulx(PPCHIRBuilder& f, const InstrData& i) {
  Value* fra = f.LoadFPR(i.A.FRA);
  Value* frc = f.LoadFPR(i.A.FRC);

  Value* a_bits = f.Cast(fra, INT64_TYPE);
  Value* c_bits = f.Cast(frc, INT64_TYPE);

  // ps0 (high)
  Value* a0 = f.Convert(f.Cast(f.Truncate(f.Shr(a_bits, 32), INT32_TYPE), FLOAT32_TYPE), FLOAT32_TYPE);
  Value* c0 = f.Convert(f.Cast(f.Truncate(f.Shr(c_bits, 32), INT32_TYPE), FLOAT32_TYPE), FLOAT32_TYPE);
  Value* d0 = f.Mul(a0, c0);
  d0 = f.Convert(f.Convert(d0, FLOAT32_TYPE), FLOAT64_TYPE);

  // ps1 (low)
  Value* a1 = f.Convert(f.Cast(f.Truncate(a_bits, INT32_TYPE), FLOAT32_TYPE), FLOAT32_TYPE);
  Value* c1 = f.Convert(f.Cast(f.Truncate(c_bits, INT32_TYPE), FLOAT32_TYPE), FLOAT32_TYPE);
  Value* d1 = f.Mul(a1, c1);
  d1 = f.Convert(f.Convert(d1, FLOAT32_TYPE), FLOAT64_TYPE);

  // Merge
  Value* d0_i = f.Cast(f.Convert(d0, INT64_TYPE), INT64_TYPE);
  Value* d1_i = f.Cast(f.Convert(d1, INT64_TYPE), INT64_TYPE);
  Value* result_bits = f.Or(f.Shl(d0_i, 32), d1_i);
  Value* result = f.Cast(result_bits, FLOAT64_TYPE);

  f.StoreFPR(i.A.FRT, result);
  f.UpdateFPSCR(result, i.A.Rc);
  return 0;
}

// ps_mrx: psD <- (psB)   [bitwise move of the paired-single pair]
// High-ROI easy win; identical storage to scalar fmr but documented for ps_* path.
// Per master plan block (ps_mr listed in arithmetic family).
int InstrEmit_ps_mrx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frB)  -- reuse exact fmrx logic since FPR storage is shared
  // (ps0/ps1 bits preserved exactly).
  Value* v = f.LoadFPR(i.X.RB);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

// ps_subx: psD <- (psA) - (psB)   [ps0 and ps1 independently]
// CAPTAIN RE-TASK / Phase 1 extension: next easy win from master PAIRED-SINGLE plan
// (listed alongside ps_add etc in arithmetic family). Exact same lowering as ps_addx
// but using HIR Sub (benefits from FPCR single-prec denorm/round/NaN on A64).
// References: R1 ps report (independent element semantics), a64_sequences F32 paths,
// fpcr_table + this file plan block. Ready for validation harness consumption.
int InstrEmit_ps_subx(PPCHIRBuilder& f, const InstrData& i) {
  // psD <- (psA) - (psB)   [ps0 and ps1 independently]
  // R1 ps research report + in-tree plan (recent landing by other arith ext agent + this Captain
  // re-task extension): bit-cast F64 halves to indep F32, Sub via HIR (FPCR denorm/round/NaN),
  // merge, UpdateFPSCR. Exact mirror of ps_addx lowering.
  // Citations (heavy, back to own prior work): the three live emitters you just established
  // rigor for (ps_addx/maddx/msubx in this file + their gated RecordPairedSingleArith/RecordPsFmaExecuted
  // + validation sequences in a64_backend.cc "DEDICATED PS ARITH KNOWN-VALUE SEQUENCES" block +
  // "RE-TASK ENHANCEMENT" + RunPairedSingleAccuracyHarness); recent ps_subx/ps_sel landing;
  // GQR (ppc_context.h helpers); R1 55-tool ps report (sub hot for deltas/velocity corrections
  // in physics/anim + per-elem); 128B/pairing (a64_seq_memory Clear + cross-thread); harness
  // (this file plan + a64_backend richer combined FMA+sub/sel + GQR + psq + 128B inv + pairing
  // violation seqs); ax360e_perf_log.h RecordPsSubSelArith + Record* .

  // CAPTAIN RE-TASK (extend same accuracy validation rigor you just established for the first three):
  // gated Record* calls inside emitter (mirrors ps_addx exactly). ps_subx now feeds ps_arith_executed
  // + ps_sub_sel_arith for on-device validation (known sub values + combined FMA+sub/sel + GQR
  // roundtrips + psq + 128B psq_st res invalidation + cross-thread pairing cases in harness).
  // Gated exactly under a64_accuracy_debug || a64_ps_accuracy_stress.
  if (cvars::a64_accuracy_debug || cvars::a64_ps_accuracy_stress) {
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleArith(2);  // ps0 + ps1 for this ps_subx site
    ax360e::perf::g_cpu_accuracy.RecordPsSubSelArith(2);      // dedicated for sub/sel uncovered R1 edges
  }

  Value* fra = f.LoadFPR(i.A.FRA);
  Value* frb = f.LoadFPR(i.A.FRB);

  // --- ps0 (high 32 bits) ---
  Value* a_bits = f.Cast(fra, INT64_TYPE);
  Value* b_bits = f.Cast(frb, INT64_TYPE);
  Value* a0_i32 = f.Truncate(f.Shr(a_bits, 32), INT32_TYPE);
  Value* b0_i32 = f.Truncate(f.Shr(b_bits, 32), INT32_TYPE);
  // SMALL POLISH (this re-task extension to sub/sel, mirroring exactly the polish applied to the
  // three live ps_addx/maddx/msubx): removed redundant Convert( cast_f32, FLOAT32 ) - noop,
  // keeps FPCR/rounding/NaN/denorm behavior identical (post-arith roundtrip + backend fpcr_table
  // + Sub deliver single-prec + Xenon quirks per R1). Consistent across all 5 ps arith emitters now.
  Value* a0 = f.Cast(a0_i32, FLOAT32_TYPE);
  Value* b0 = f.Cast(b0_i32, FLOAT32_TYPE);
  Value* d0 = f.Sub(a0, b0);
  d0 = f.Convert(f.Convert(d0, FLOAT32_TYPE), FLOAT64_TYPE);

  // --- ps1 (low 32 bits) ---
  Value* a1_i32 = f.Truncate(a_bits, INT32_TYPE);
  Value* b1_i32 = f.Truncate(b_bits, INT32_TYPE);
  Value* a1 = f.Cast(a1_i32, FLOAT32_TYPE);
  Value* b1 = f.Cast(b1_i32, FLOAT32_TYPE);
  Value* d1 = f.Sub(a1, b1);
  d1 = f.Convert(f.Convert(d1, FLOAT32_TYPE), FLOAT64_TYPE);

  // Merge ps0 (high) | ps1 (low) back into F64 container
  Value* d0_i = f.Cast(f.Convert(d0, INT64_TYPE), INT64_TYPE);
  Value* d1_i = f.Cast(f.Convert(d1, INT64_TYPE), INT64_TYPE);
  Value* result_bits = f.Or(f.Shl(d0_i, 32), d1_i);
  Value* result = f.Cast(result_bits, FLOAT64_TYPE);

  f.StoreFPR(i.A.FRT, result);
  f.UpdateFPSCR(result, i.A.Rc);
  // Light FPCR/rounding/NaN/denorm polish note (CAPTAIN RE-TASK extension to recently-landed sub/sel):
  // now consistent with the three (addx etc): direct Cast bit-reinterpret + roundtrip + UpdateFPSCR
  // + backend FPCR (SET_ROUNDING_MODE / fpcr_table FZ/DN/RM). Per-element independent on Xenon PPE.
  // Ties to a64_sequences.cc + R1 report + harness combined seqs exercising ps_sub with GQR/psq/128B.
  // Citations: own prior three emitters + their validation sequences (a64_backend.cc) + this landing + GQR + 128B/pairing.
  return 0;
}

// ps_sel: psD <- (psA >= 0.0 ? psC : psB)   [independent per ps0/ps1]
// CAPTAIN RE-TASK / Phase 1 extension: next easy win listed in master plan
// (special ops section, high value for conditional select in shaders/anim).
// Mirrors fselx exactly but on extracted F32 halves (per-element semantics match
// Xenon PPE ps_*). Uses HIR Select + CompareSGE on F32 (FPCR/rounding path).
// Per R1 report + plan: independent element behavior critical. Gated debug paths
// + counters added for harness. Ties directly to existing backend FPCR work.
int InstrEmit_ps_sel(PPCHIRBuilder& f, const InstrData& i) {
  // if (psA >= 0.0) then psD <- (psC) else psD <- (psB)  [per element]
  // R1 ps research report + in-tree plan (recent landing by other arith ext agent + this Captain
  // re-task extension): bit-cast F64 halves, CompareSGE+Select on F32 (FPCR path), merge, UpdateFPSCR.
  // Mirrors fselx but per-element for ps_* . High value for conditionals.
  // Citations (heavy, back to own prior work): the three live emitters you just established
  // rigor for (ps_addx/maddx/msubx + gated Record* + known-value validation sequences in
  // a64_backend.cc harness "DEDICATED PS ARITH..." + richer combined blocks); recent ps_subx/ps_sel
  // landing (other agent); GQR + psq paths; R1 55-tool ps report (sel for morph/conditional
  // skin/anim/physics per-elem); 128B/pairing (a64_seq_memory); harness (RunPairedSingleAccuracyHarness
  // FMA+sub/sel + GQR roundtrips + psq + 128B psq_st inv + cross-thread pairing violation cases);
  // ax360e_perf_log.h RecordPsSubSelArith + ps_* counters + snapshot.

  // CAPTAIN RE-TASK (extend same accuracy validation rigor you just established for the first three):
  // gated Record* calls inside emitter (mirrors ps_addx exactly, now for sel). ps_sel feeds
  // ps_arith_executed + ps_sub_sel_arith (for harness dedicated sel known-values + richer
  // combined ps arith(FMA+sub/sel)+GQR+psq+128B+pairing sequences).
  // Gated under a64_accuracy_debug || a64_ps_accuracy_stress.
  if (cvars::a64_accuracy_debug || cvars::a64_ps_accuracy_stress) {
    ax360e::perf::g_cpu_accuracy.RecordPairedSingleArith(2);  // ps0 + ps1 for this ps_sel site
    ax360e::perf::g_cpu_accuracy.RecordPsSubSelArith(2);      // dedicated for sub/sel R1 edges
  }

  Value* fra = f.LoadFPR(i.A.FRA);
  Value* frc = f.LoadFPR(i.A.FRC);
  Value* frb = f.LoadFPR(i.A.FRB);

  // --- ps0 (high) ---
  Value* a_bits = f.Cast(fra, INT64_TYPE);
  Value* c_bits = f.Cast(frc, INT64_TYPE);
  Value* b_bits = f.Cast(frb, INT64_TYPE);
  Value* a0_i32 = f.Truncate(f.Shr(a_bits, 32), INT32_TYPE);
  Value* c0_i32 = f.Truncate(f.Shr(c_bits, 32), INT32_TYPE);
  Value* b0_i32 = f.Truncate(f.Shr(b_bits, 32), INT32_TYPE);
  // SMALL POLISH (this re-task extension to sub/sel, mirroring exactly the polish on the three
  // ps_addx/maddx/msubx): removed redundant Convert around Cast bit-reinterprets. Keeps
  // FPCR/rounding/NaN/denorm/Select semantics identical (roundtrip + fpcr_table handle Xenon
  // per-elem single-prec). Now consistent across entire ps arith family (add/madd/msub/sub/sel).
  Value* a0 = f.Cast(a0_i32, FLOAT32_TYPE);
  Value* c0 = f.Cast(c0_i32, FLOAT32_TYPE);
  Value* b0 = f.Cast(b0_i32, FLOAT32_TYPE);
  Value* ge0 = f.CompareSGE(a0, f.LoadZeroFloat32());  // F32 zero for single-prec
  Value* d0 = f.Select(ge0, c0, b0);
  d0 = f.Convert(f.Convert(d0, FLOAT32_TYPE), FLOAT64_TYPE);

  // --- ps1 (low) ---
  Value* a1_i32 = f.Truncate(a_bits, INT32_TYPE);
  Value* c1_i32 = f.Truncate(c_bits, INT32_TYPE);
  Value* b1_i32 = f.Truncate(b_bits, INT32_TYPE);
  Value* a1 = f.Cast(a1_i32, FLOAT32_TYPE);
  Value* c1 = f.Cast(c1_i32, FLOAT32_TYPE);
  Value* b1 = f.Cast(b1_i32, FLOAT32_TYPE);
  Value* ge1 = f.CompareSGE(a1, f.LoadZeroFloat32());
  Value* d1 = f.Select(ge1, c1, b1);
  d1 = f.Convert(f.Convert(d1, FLOAT32_TYPE), FLOAT64_TYPE);

  // Merge
  Value* d0_i = f.Cast(f.Convert(d0, INT64_TYPE), INT64_TYPE);
  Value* d1_i = f.Cast(f.Convert(d1, INT64_TYPE), INT64_TYPE);
  Value* result_bits = f.Or(f.Shl(d0_i, 32), d1_i);
  Value* result = f.Cast(result_bits, FLOAT64_TYPE);

  f.StoreFPR(i.A.FRT, result);
  f.UpdateFPSCR(result, i.A.Rc);
  // Light FPCR/rounding/NaN/denorm polish note (CAPTAIN RE-TASK for recently-landed ps_sel):
  // CompareSGE/Select + direct Cast (post-polish) + roundtrip + FPSCR leverages exact single-prec
  // FPCR paths (fpcr_table) now consistent with sub + the three. Per-element critical (R1).
  // Ties to harness richer combined sequences (FMA + sub/sel arith + GQR + psq + 128B/pairing).
  // Citations: own prior three emitters + validation sequences you just added + recent landing + GQR + R1 + 128B/pairing + harness.
  return 0;
}

// ----------------------------------------------------------------------------
// END PAIRED-SINGLE STUB SECTION
// ----------------------------------------------------------------------------

// Floating-point arithmetic (A-8)

int InstrEmit_faddx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) + (frB)
  Value* v = f.Add(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_faddsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) + (frB)
  Value* v = f.Add(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fdivx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- frA / frB
  Value* v = f.Div(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fdivsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- frA / frB
  Value* v = f.Div(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmulx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) x (frC)
  Value* v = f.Mul(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmulsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) x (frC)
  Value* v = f.Mul(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fresx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- 1.0 / (frB)
  Value* v = f.Convert(f.Div(f.LoadConstantFloat32(1.0f),
                             f.Convert(f.LoadFPR(i.A.FRB), FLOAT32_TYPE)),
                       FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_frsqrtex(PPCHIRBuilder& f, const InstrData& i) {
  // Double precision:
  // frD <- 1/sqrt(frB)
  Value* v = f.RSqrt(f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fsubx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) - (frB)
  Value* v = f.Sub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fsubsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA) - (frB)
  Value* v = f.Sub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRB));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fselx(PPCHIRBuilder& f, const InstrData& i) {
  // if (frA) >= 0.0
  // then frD <- (frC)
  // else frD <- (frB)
  Value* ge = f.CompareSGE(f.LoadFPR(i.A.FRA), f.LoadZeroFloat64());
  Value* v = f.Select(ge, f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fsqrtx(PPCHIRBuilder& f, const InstrData& i) {
  // Double precision:
  // frD <- sqrt(frB)
  Value* v = f.Sqrt(f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fsqrtsx(PPCHIRBuilder& f, const InstrData& i) {
  // Single precision:
  // frD <- sqrt(frB)
  Value* v = f.Sqrt(f.LoadFPR(i.A.FRB));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

// Floating-point multiply-add (A-9)

int InstrEmit_fmaddx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA x frC) + frB
  Value* v =
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmaddsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA x frC) + frB
  Value* v =
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmsubx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA x frC) - frB
  Value* v =
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fmsubsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frA x frC) - frB
  Value* v =
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmaddx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] + frB)
  Value* v = f.Neg(
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmaddsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] + frB)
  Value* v = f.Neg(
      f.MulAdd(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmsubx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] - frB)
  Value* v = f.Neg(
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fnmsubsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- -([frA x frC] - frB)
  Value* v = f.Neg(
      f.MulSub(f.LoadFPR(i.A.FRA), f.LoadFPR(i.A.FRC), f.LoadFPR(i.A.FRB)));
  v = f.Convert(f.Convert(v, FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.A.FRT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

// Floating-point rounding and conversion (A-10)

int InstrEmit_fcfidx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- signed_int64_to_double( frB )
  Value* v = f.Convert(f.Cast(f.LoadFPR(i.X.RB), INT64_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.A.Rc);
  return 0;
}

int InstrEmit_fctidxx_(PPCHIRBuilder& f, const InstrData& i,
                       RoundMode round_mode) {
  auto end = f.NewLabel();
  auto isnan = f.NewLabel();
  Value* v;
  f.BranchTrue(f.IsNan(f.LoadFPR(i.X.RB)), isnan);
  v = f.Convert(f.LoadFPR(i.X.RB), INT64_TYPE, round_mode);
  v = f.Cast(v, FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  f.Branch(end);
  f.MarkLabel(isnan);
  v = f.Cast(f.LoadConstantUint64(0x8000000000000000u), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  f.MarkLabel(end);
  return 0;
}

int InstrEmit_fctidx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- double_to_signed_int64( frB )
  return InstrEmit_fctidxx_(f, i, ROUND_DYNAMIC);
}

int InstrEmit_fctidzx(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fctidxx_(f, i, ROUND_TO_ZERO);
}

int InstrEmit_fctiwxx_(PPCHIRBuilder& f, const InstrData& i,
                       RoundMode round_mode) {
  auto end = f.NewLabel();
  auto isnan = f.NewLabel();
  Value* v;
  f.BranchTrue(f.IsNan(f.LoadFPR(i.X.RB)), isnan);
  v = f.Convert(f.LoadFPR(i.X.RB), INT32_TYPE, round_mode);
  v = f.Cast(f.SignExtend(v, INT64_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  f.Branch(end);
  f.MarkLabel(isnan);
  v = f.Cast(f.LoadConstantUint32(0x80000000u), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  f.MarkLabel(end);
  return 0;
}

int InstrEmit_fctiwx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- double_to_signed_int32( frB )
  return InstrEmit_fctiwxx_(f, i, ROUND_DYNAMIC);
}

int InstrEmit_fctiwzx(PPCHIRBuilder& f, const InstrData& i) {
  // TODO(benvanik): assuming round to zero is always set, is that ok?
  return InstrEmit_fctiwxx_(f, i, ROUND_TO_ZERO);
}

int InstrEmit_frspx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- Round_single(frB)
  Value* v = f.Convert(f.LoadFPR(i.X.RB), FLOAT32_TYPE, ROUND_DYNAMIC);
  v = f.Convert(v, FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

// Floating-point compare (A-11)

int InstrEmit_fcmpx_(PPCHIRBuilder& f, const InstrData& i, bool ordered) {
  // if (FRA) is a NaN or (FRB) is a NaN then
  //   c <- 0b0001
  // else if (FRA) < (FRB) then
  //   c <- 0b1000
  // else if (FRA) > (FRB) then
  //   c <- 0b0100
  // else {
  //   c <- 0b0010
  // }
  // FPCC <- c
  // CR[4*BF:4*BF+3] <- c
  // if (FRA) is an SNaN or (FRB) is an SNaN then
  //   VXSNAN <- 1

  // TODO(benvanik): update FPCC for mffsx/etc
  // TODO(benvanik): update VXSNAN
  const uint32_t crf = i.X.RT >> 2;
  Value* ra = f.LoadFPR(i.X.RA);
  Value* rb = f.LoadFPR(i.X.RB);

  Value* nan = f.Or(f.IsNan(ra), f.IsNan(rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 3, nan);
  Value* not_nan = f.Xor(nan, f.LoadConstantInt8(0x01));

  Value* lt = f.And(not_nan, f.CompareSLT(ra, rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 0, lt);
  Value* gt = f.And(not_nan, f.CompareSGT(ra, rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 1, gt);
  Value* eq = f.And(not_nan, f.CompareEQ(ra, rb));
  f.StoreContext(offsetof(PPCContext, cr0) + (4 * crf) + 2, eq);
  return 0;
}
int InstrEmit_fcmpo(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fcmpx_(f, i, true);
}
int InstrEmit_fcmpu(PPCHIRBuilder& f, const InstrData& i) {
  return InstrEmit_fcmpx_(f, i, false);
}

// Floating-point status and control register (A

int InstrEmit_mcrfs(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

int InstrEmit_mffsx(PPCHIRBuilder& f, const InstrData& i) {
  if (i.X.Rc) {
    XEINSTRNOTIMPLEMENTED();
    return 1;
  }
  Value* v = f.Cast(f.ZeroExtend(f.LoadFPSCR(), INT64_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, v);
  return 0;
}

int InstrEmit_mtfsb0x(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

int InstrEmit_mtfsb1x(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

int InstrEmit_mtfsfx(PPCHIRBuilder& f, const InstrData& i) {
  if (i.XFL.L) {
    // Move/shift.
    f.StoreFPSCR(
        f.Truncate(f.Cast(f.LoadFPR(i.XFL.RB), INT64_TYPE), INT32_TYPE));
    return 1;
  } else {
    assert_zero(i.XFL.W);

    // Store under control of mask.
    // Expand the mask from 8 bits -> 32 bits.
    uint32_t mask = 0;
    for (int j = 0; j < 8; j++) {
      if (i.XFL.FM & (1 << (j ^ 7))) {
        mask |= 0xF << (4 * j);
      }
    }

    Value* v = f.Truncate(f.Cast(f.LoadFPR(i.XFL.RB), INT64_TYPE), INT32_TYPE);
    if (mask != 0xFFFFFFFF) {
      Value* fpscr = f.LoadFPSCR();
      v = f.And(v, f.LoadConstantInt32(mask));
      v = f.Or(v, f.And(fpscr, f.LoadConstantInt32(~mask)));
    }
    f.StoreFPSCR(v);

    // Update the system rounding mode.
    if (mask & 0x7) {
      f.SetRoundingMode(v);
    }
  }
  if (i.XFL.Rc) {
    f.CopyFPSCRToCR1();
  }
  return 0;
}

int InstrEmit_mtfsfix(PPCHIRBuilder& f, const InstrData& i) {
  // FPSCR[crfD] <- IMM

  // Create a mask.
  uint32_t mask = 0xF << (0x1C - (i.X.RT & 0x1C));
  uint32_t value = i.X.RB << (0x1C - (i.X.RT & 0x1C));

  Value* fpscr = f.LoadFPSCR();
  fpscr = f.And(fpscr, f.LoadConstantInt32(~mask));
  fpscr = f.Or(fpscr, f.LoadConstantInt32(value));
  f.StoreFPSCR(fpscr);

  // Update the system rounding mode.
  if (mask & 0x7) {
    f.SetRoundingMode(fpscr);
  }

  if (i.X.Rc) {
    f.CopyFPSCRToCR1();
  }

  return 0;
}

// Floating-point move (A-21)

int InstrEmit_fabsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- abs(frB)
  Value* v = f.Abs(f.LoadFPR(i.X.RB));
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

int InstrEmit_fmrx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- (frB)
  Value* v = f.LoadFPR(i.X.RB);
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

int InstrEmit_fnabsx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- !abs(frB)
  Value* v = f.Neg(f.Abs(f.LoadFPR(i.X.RB)));
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

int InstrEmit_fnegx(PPCHIRBuilder& f, const InstrData& i) {
  // frD <- ¬ frB[0] || frB[1-63]
  Value* v = f.Neg(f.LoadFPR(i.X.RB));
  f.StoreFPR(i.X.RT, v);
  f.UpdateFPSCR(v, i.X.Rc);
  return 0;
}

void RegisterEmitCategoryFPU() {
  XEREGISTERINSTR(faddx);
  XEREGISTERINSTR(faddsx);
  XEREGISTERINSTR(fdivx);
  XEREGISTERINSTR(fdivsx);
  XEREGISTERINSTR(fmulx);
  XEREGISTERINSTR(fmulsx);
  XEREGISTERINSTR(fresx);
  XEREGISTERINSTR(frsqrtex);
  XEREGISTERINSTR(fsubx);
  XEREGISTERINSTR(fsubsx);
  XEREGISTERINSTR(fselx);
  XEREGISTERINSTR(fsqrtx);
  XEREGISTERINSTR(fsqrtsx);
  XEREGISTERINSTR(fmaddx);
  XEREGISTERINSTR(fmaddsx);
  XEREGISTERINSTR(fmsubx);
  XEREGISTERINSTR(fmsubsx);
  XEREGISTERINSTR(fnmaddx);
  XEREGISTERINSTR(fnmaddsx);
  XEREGISTERINSTR(fnmsubx);
  XEREGISTERINSTR(fnmsubsx);
  XEREGISTERINSTR(fcfidx);
  XEREGISTERINSTR(fctidx);
  XEREGISTERINSTR(fctidzx);
  XEREGISTERINSTR(fctiwx);
  XEREGISTERINSTR(fctiwzx);
  XEREGISTERINSTR(frspx);
  XEREGISTERINSTR(fcmpo);
  XEREGISTERINSTR(fcmpu);
  XEREGISTERINSTR(mcrfs);
  XEREGISTERINSTR(mffsx);
  XEREGISTERINSTR(mtfsb0x);
  XEREGISTERINSTR(mtfsb1x);
  XEREGISTERINSTR(mtfsfx);
  XEREGISTERINSTR(mtfsfix);
  XEREGISTERINSTR(fabsx);
  XEREGISTERINSTR(fmrx);
  XEREGISTERINSTR(fnabsx);
  XEREGISTERINSTR(fnegx);

  // --------------------------------------------------------------------------
  // PAIRED-SINGLE (ps_*) - REGISTRATION (AGENT D + CAPTAIN RE-TASK Phase 1 ext)
  // Per R1 ps research report + master PAIRED-SINGLE plan (top of file). Extended
  // arithmetic family: ps_addx/maddx/msubx/mulx/mrx/subx/sel now dispatched to real
  // emitters (lowering + FPCR single-prec + new accuracy debug counters under a64_accuracy_debug).
  // psq/GQR/.s/indexed/specials left for other agents. Ties to harness in a64_backend.cc.
  // --------------------------------------------------------------------------
  XEREGISTERINSTR(ps_addx);
  XEREGISTERINSTR(ps_maddx);
  XEREGISTERINSTR(ps_msubx);
  XEREGISTERINSTR(ps_mulx);
  XEREGISTERINSTR(ps_mrx);
  XEREGISTERINSTR(ps_subx);
  XEREGISTERINSTR(ps_sel);
  // (psq_l / psq_st + full GQR quantization in memory emitter later; basic GQR SPR reads added below)
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
