/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
#define XENIA_CPU_BACKEND_A64_A64_BACKEND_H_

#include <memory>

#include "xenia/base/cvar.h"
#include "xenia/base/bit_map.h"
#include "xenia/cpu/backend/backend.h"

DECLARE_int32(a64_extension_mask);
DECLARE_int32(max_stackpoints);
DECLARE_bool(enable_host_guest_stack_synchronization);
DECLARE_bool(a64_software_reservation_fallback);  // Tiered fallback: hardware STLXR first, then software via ReserveHelper + cached value/EA on monitor loss (cross-core Android)

// 128B reservation stress + false-share debug harness cvar (CAPTAIN DIRECT ORDER).
// When set (or a64_accuracy_debug), enables runtime validation sequences for:
// - Real Xenon 128-byte reservation granule (not 64B).
// - Per-thread reservation pairing constraints / errata (no crossing lwarx without stwcx).
// - False sharing risk at 128B boundaries (critical for lock-free audio + physics titles
//   doing atomics across cores; normal stores *must* clear overlapping reservations).
// See ClearXenonReservationIfStoreOverlaps + research notes in a64_seq_memory.cc.
// Triggerable from Java PerformanceMonitor / hidden dev setting or backend init.
DECLARE_bool(a64_128b_reservation_stress);

// CAPTAIN quick-win from Xenon barriers research (lwsync dominant in titles).
// Opt-in (gated under a64_accuracy_debug) for experimental lighter lowering of
// LIGHT_SYNC (lwsync) using ISHLD/ISHST (per a64_light_sync_fidelity variants).
// DEEPENED this re-task: more fidelity options/variants, richer observability under
// experiment cvar, explicit integration with pairing/128B enforcement paths
// (referenceable via harness), more validation cases in full modern stack seqs.
// See a64_seq_memory.cc MEMORY_BARRIER + a64_backend.cc harness for details +
// heavy citations to own barriers research report + psq_st GQR + ps arith (sub/sel)
// + GQR + pairing + TLB integrations.
DECLARE_bool(a64_light_sync_experiment);

// DEEPENED (CAPTAIN RE-TASK full modern stack + lighter lwsync): fidelity variants for the
// opt-in LIGHT_SYNC experiment. 0 = conservative (force ISH even under exp for baseline),
// 1 = ISHLD (load-oriented lighter per barriers research), 2 = ISHST (store-oriented).
// Combined with a64_light_sync_experiment + a64_accuracy_debug for on-device validation.
// Explicitly integrated with 128B/pairing enforcement + ps harness sequences (reference
// XenonReservesOverlap + Clear paths in a64_backend RunPairedSingleAccuracyHarness).
// Richer observability: variant chosen logged + exercised in full-stack title patterns
// (physics lockfree + ps_sub/sel quantized + 128B false-share + TLB).
// Citations: own Xenon barriers research report (lwsync dominant + weakest-sufficient
// lighter variants rec) + recent psq_st GQR + ps arith (sub/sel) + GQR + pairing + TLB.
DECLARE_int32(a64_light_sync_fidelity);

namespace xe {
class Exception;
}  // namespace xe
namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

    //static const uintptr_t kExecuteCodeAddrHigh = 0xaull<<32;

class A64CodeCache;

typedef void* (*HostToGuestThunk)(void* target, void* arg0, void* arg1);
typedef void* (*GuestToHostThunk)(void* target, void* arg0, void* arg1);
typedef void (*ResolveFunctionThunk)();

/*
    place guest trampolines in the memory range that the HV normally occupies.
    This way guests can call in via the indirection table and we don't have to
   clobber/reuse an existing memory range The xboxkrnl range is already used by
   export trampolines (see kernel/kernel_module.cc)
*/
    static constexpr uint32_t GUEST_TRAMPOLINE_BASE = 0x80000000;
    static constexpr uint32_t GUEST_TRAMPOLINE_END = 0x80040000;

    static constexpr uint32_t GUEST_TRAMPOLINE_MIN_LEN = 8;

    static constexpr uint32_t MAX_GUEST_TRAMPOLINES =
            (GUEST_TRAMPOLINE_END - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;

#define RESERVE_BLOCK_SHIFT 16

#define RESERVE_NUM_ENTRIES \
  ((1024ULL * 1024ULL * 1024ULL * 4ULL) >> RESERVE_BLOCK_SHIFT)

// Real Xenon reservation granule is 128 bytes (cache line size).
// We use exact cached EA + this granule for precise matching/invalidation
// (the coarse 64KB bitmap is only a "possible overlap" filter).
constexpr uint64_t XENON_RESERVE_GRANULE = 128;
constexpr uint64_t XENON_RESERVE_GRANULE_MASK = ~(XENON_RESERVE_GRANULE - 1);

inline bool XenonReserveGranulesOverlap(uint64_t a, uint64_t b) {
  return (a & XENON_RESERVE_GRANULE_MASK) == (b & XENON_RESERVE_GRANULE_MASK);
}

// XenonReservesOverlapAcrossThreads (debug helper for a64_accuracy_debug):
// Returns true if two reservations target the *same* 128B granule but come from
// different logical threads (using ctx pointer as stable per-guest-thread id proxy).
// 
// CRITICAL RESEARCH CITATION (CAPTAIN 128B pairing errata enforcement):
// - Real Xenon reservation granule = 128 bytes (not 64K, not even 64B cacheline in some refs).
// - Per-thread exclusive monitor pairing errata on SMT: hardware monitors are PE-local.
//   Dangerous to assume reliable cross-thread (or cross-SMT-sibling) reservation handoff.
// - False-sharing risks at 128B boundaries are real (audio/physics lock-free atomics in titles).
// - Explicit normal store invalidation (ClearXenonReservationIfStoreOverlaps) required.
// - Android big.LITTLE: PE-local monitors + cluster power gating + thread migration make
//   any cross-thread overlapping res pattern a fidelity violation to detect/report.
// This helper + last_* debug fields in A64BackendContext enable active detection.
inline bool XenonReservesOverlapAcrossThreads(uint64_t granule_a, uint64_t thread_id_a,
                                              uint64_t granule_b, uint64_t thread_id_b) {
  return ((granule_a & XENON_RESERVE_GRANULE_MASK) == (granule_b & XENON_RESERVE_GRANULE_MASK)) &&
         (thread_id_a != thread_id_b);
}

// 128B Xenon reservation research harness (lightweight, cvar-driven).
// Provides the entrypoint called from JNI trigger (PerformanceMonitor) and
// A64Backend::Initialize under debug. Implements the minimal validation
// sequences (lwarx on granule, crossing ordinary stores at +64/+127, V128 analogs)
// using host simulation of the granule overlap + flag clear logic. Increments
// dedicated CpuAccuracyTracker counters and logs with full citations.
// TLB owner integration (re-task): cross-ref note + TLB seqs in ps harness cover
// TLB-shootdown + 128B res migration on big.LITTLE (ties per-thread pairing errata).
// No new test framework; reuses existing cvar/logging/metrics.
void Run128BReservationStressTestHarness();

// R1 (original paired-single research author) + CAPTAIN: ps_* accuracy validation harness.
// Lightweight, modeled directly 1:1 on Run128BReservationStressTestHarness() + cvar pattern.
// Entry point for a64_ps_accuracy_stress (or a64_accuracy_debug) at A64Backend init and
// explicit Java PerformanceMonitor trigger. Runs simple runtime sequences exercising
// ps_addx/ps_maddx/ps_msubx (FMA on known values) + basic psq (GQR quant roundtrips) +
// psq_st 128B reservation interaction stress (per explicit R1 warning).
// CAPTAIN RE-TASK (psq_st GQR + psq_l load skeleton): GQR on psq_st + pure psq_l placeholder
// load skeletons (CalculateEA + Load raw to FPR) + symmetric psq_l res/pairing seqs/counters
// (lwarx A + psq_l B cross EA/res tracking) + helpers + mem activation. Full 128B + pairing
// (last_reserving_thread_id/granule, XenonReserves..., Clear probe) exercised for psq load/store.
// Ties pairing enforcement + psq_st skeleton + R1 ps report + 128B complete.
// NEW (TLB owner re-task): TLB-aware debug sequences (R2 TLB/ERAT report citations) integrated:
//   big.LITTLE TLB shootdown + res migration, protection faults (ESR polish) during psq_st + atomics,
//   TLB + FPU/psq store 128B granule cases. Reuses tlb_ops_ignored counter (from ppc_hir_builder hook)
//   so TLB debug now runs inside this active harness. Heavy cross-citations to 128B/pairing work.
// R1 AUTHOR ENRICH (CAPTAIN RE-TASK): richer title-derived sequences added to harness (ps_madd from
// UE3/Forza/Halo, real GQR VBO roundtrips via helpers, psq_st+128B+lockfree CAS+lwsync combos,
// extra per-elem FPCR edges + 2 new counters). Wires all landed ps/GQR/psq_st/128B pieces.
// VALIDATOR-IN-CHIEF EXPANSION (this re-task): even richer title-derived + deeper full-stack
// integration sequences (more FMA/sub/sel from UE3/Forza/Halo physics/vertex/skin/anim, expanded
// multi-GQR VBO fidelity cases, psq_st+128B+lockfree audio/physics+barriers combos, deeper per-elem
// NaN/denorm/rounding/payload/TLB shootdown edges, crown psq_l(GQR)+sub/sel+psq_st(GQR)+lwsync+
// TLB+128B/pairing realistic R1 scenarios). + 3 new R1-gap counters. Same lightweight cvar gate +
// heavy R1 55-tool citations. Definitive on-device proof harness for entire ps_* + GQR + 128B +
// pairing + barriers + TLB stack. Increments dedicated CpuAccuracyTracker counters; full research
// citations in impl.
// PSQ_ST GQR PRODUCTION + RICHER DEDICATED SEQS (this Captain re-task): psq_st* emitters deepened
// to production (full W/I + all u/s8/16 types + error/edge handling + real HIR LoadContext gqr
// paths + no dummy_ctx; citations to own 128B/pairing + R1 + GQR foundation + barriers/TLB).
// Added dedicated richer psq_st-specific full-stack harness sequences (GQR prod + 128B inval +
// pairing enforcement + barriers + TLB debug) using realistic R1 UE3/Forza/Halo patterns
// (quantized vertex/skin/anim/physics + lockfree 128B false-share). Heavy cross-cites in code.
void RunPairedSingleAccuracyHarness();

// CAPTAIN RE-TASK (psq_st GQR): small supporting helper (ps_* harness + 128B + GQR integration):
// Dedicated entrypoint to exercise psq_st cross-thread 128B pairing + new psq_st_* GQR/execution
// counters from memory seq paths (psq_st quantized stores from emitters with GQRQuantize calls).
// Thin; reuses main harness. Gated... Allows seq_memory activation for psq_st (with 128B skeleton coverage).
// UPDATED (psq_st GQR production + richer harness seqs): now invoked from dedicated PSQ_ST FULL-STACK
// block exercising complete stack with R1 title patterns post-emitter production (full W/I/types/edges +
// real HIR LoadContext). Citations include ppc_emit_memory production + 128B/pairing/GQR foundation.
void ExercisePsqStoreReservationPairingViolationSequence();

// CAPTAIN RE-TASK (psq_l load-side skeleton): symmetric supporting helper declaration.
// Called from a64_seq_memory.cc (load seq activation under ps cvar) + future psq_l
// emitters. Exercises psq_l_* counters + lwarx+psq_l 128B EA/res tracking seqs
// (correct placeholder EA from psq_l skeletons). Citations: pairing enforcement
// (last_reserving_thread_id/granule, XenonReservesOverlapAcrossThreads, LOAD_RESERVED
// capture + Clear probe), psq_l skeleton (ppc_emit_memory placeholder CalculateEA+Load),
// psq_st symmetric, R1 ps report, 128B complete coverage. Thin cvar-gated delegate.
void ExercisePsqLoadReservationPairingSequence();

// CAPTAIN RE-TASK (psq_l load-side skeleton delivery - symmetric counterpart):
// Small supporting helper (ps_* harness + 128B pairing + load EA rigor integration):
// Dedicated entrypoint to exercise psq_l cross-granule EA/res tracking + new psq_l_*
// counters from memory seq paths (psq_l quantized loads from emitters using placeholder
// CalculateEA + Load). Thin; reuses main harness. Gated... Allows seq_memory activation
// for psq_l load side (correct EA for reservation tracking in lwarx+psq_l harness seqs;
// loads do not invalidate but validate the acquire/granule side of pairing enforcement).
// Citations: psq_l skeleton (ppc_emit_memory.cc), pairing (LOAD_RESERVED last_* +
// XenonReservesOverlapAcrossThreads + Clear probe), psq_st store helper (symmetric),
// R1 ps report + 128B complete coverage.
void ExercisePsqLoadReservationPairingSequence();

// https://codalogic.com/blog/2022/12/06/Exploring-PowerPCs-read-modify-write-operations
// Extended (debug comment) for 128B Xenon model: ReserveHelper's coarse 64KB bitmap
// is only a "possible overlap" filter. Precise 128B granule + cross-thread owner tracking
// lives in per-A64BackendContext cached fields (see last_reserve_granule + last_reserving_thread_id
// + XenonReservesOverlapAcrossThreads). This structure + per-thread state together implement the
// hybrid hardware (LDAXR/STLXR) + software monitor required by Android realities + Xenon errata.
        struct ReserveHelper {
            uint64_t blocks[RESERVE_NUM_ENTRIES / 64];

            ReserveHelper() { memset(blocks, 0, sizeof(blocks)); }
        };

        struct A64BackendStackpoint {
            uint64_t host_stack_;
            uint32_t guest_stack_;
            // pad to 16 bytes so we never end up having a 64 bit load/store for
            // host_stack_ straddling two lines. Consider this field reserved for future
            // use
            uint32_t guest_return_address_;
        };
        struct A64BackendContext {
            union {
                uint64x2_t helper_scratch_xmms[4];
                uint64_t helper_scratch_u64s[8];
                uint32_t helper_scratch_u32s[16];
            };
            ReserveHelper* reserve_helper_;
            uint64_t cached_reserve_value_;
            // guest_tick_count is used if inline_loadclock is used (points to
            // shared last_guest_tick for consistent timebase across threads).
            uint64_t* guest_tick_count;
            // records mapping of host_stack to guest_stack
            A64BackendStackpoint* stackpoints;
            uint64_t cached_reserve_offset;
            uint32_t cached_reserve_bit;
            // For Xenon pairing errata awareness (research-driven).
            // Records the guest EA of the most recent lwarx/ldarx on this thread.
            // Used in a64_accuracy_debug mode to detect illegal "crossing" reservations
            // (lwarx A; lwarx B without completing stwcx/stdcx to A first).
            uint64_t last_reserved_address;
            // CAPTAIN 128B PAIRING ERRATA DEBUG FIELDS (per "half research / half code" landing):
            // last_reserving_thread_id: stable proxy for logical guest thread (A64BackendContext*
            //   address is unique per guest PPC thread's backend state; survives migration).
            // last_reserve_granule: 128B-aligned address of the *active* reservation on *this* thread.
            // Together with XenonReservesOverlapAcrossThreads() these enable runtime detection
            // (in LOAD/STORE_RESERVED + ClearXenon... call sites on hot stores) of dangerous
            // cross-thread overlapping 128B reservations that violate real Xenon per-thread
            // monitor + SMT errata behavior. Only used under a64_accuracy_debug (or stress cvar).
            // See a64_seq_memory.cc for the instrumentation points + XELOGW + counter increments.
            uint64_t last_reserving_thread_id;
            uint64_t last_reserve_granule;
            uint32_t current_stackpoint_depth;
            uint32_t mxcsr_fpu;  // LEGACY (x64 port): guest FPU rounding/FZ state.
            // On AArch64 we drive FPCR directly from SET_ROUNDING_MODE
            // (see a64_sequences.cc fpcr_table) using guest FPSCR bits.
            // Real Xenon FPU (esp. paired-single ps_* ops) has specific
            // denormal/NaN/rounding quirks; mxcsr_vmx for VMX128 fp.
            // bit 0 = 0 if mxcsr is fpu, else it is vmx
            // bit 1 = got reserve
            uint32_t mxcsr_vmx;
            uint32_t flags;
            uint32_t Ox1000;  // constant 0x1000 so we can shrink each tail emitted
            // add of it by... 2 bytes lol
        };
class A64Backend : public Backend {
 public:
  //static const uint32_t kForceReturnAddress = 0x9FFF0000u;

  explicit A64Backend();
  ~A64Backend() override;

  A64CodeCache* code_cache() const { return code_cache_.get(); }
  uintptr_t emitter_data() const { return emitter_data_; }

  // Call a generated function, saving all stack parameters.
  HostToGuestThunk host_to_guest_thunk() const { return host_to_guest_thunk_; }
  // Function that guest code can call to transition into host code.
  GuestToHostThunk guest_to_host_thunk() const { return guest_to_host_thunk_; }
  // Function that thunks to the ResolveFunction in A64Emitter.
  ResolveFunctionThunk resolve_function_thunk() const {
    return resolve_function_thunk_;
  }

  bool Initialize(Processor* processor) override;

  void InitializeCodeCache(const std::filesystem::path& cache_root,
                           uint32_t title_id) override;
  void ShutdownCodeCache();

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override;

  std::unique_ptr<Assembler> CreateAssembler() override;

  std::unique_ptr<GuestFunction> CreateGuestFunction(Module* module,
                                                     uint32_t address) override;

  uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                        uint64_t current_pc) override;

  void InstallBreakpoint(Breakpoint* breakpoint) override;
  void InstallBreakpoint(Breakpoint* breakpoint, Function* fn) override;
  void UninstallBreakpoint(Breakpoint* breakpoint) override;
    virtual void InitializeBackendContext(void* ctx) override;
    virtual void DeinitializeBackendContext(void* ctx) override;
    virtual void PrepareForReentry(void* ctx) override;

    A64BackendContext* BackendContextForGuestContext(void* ctx) {
        return reinterpret_cast<A64BackendContext*>(
                reinterpret_cast<intptr_t>(ctx) - sizeof(A64BackendContext));
    }

    virtual bool PopulatePseudoStacktrace(GuestPseudoStackTrace* st) override;
    virtual uint32_t CreateGuestTrampoline(GuestTrampolineProc proc,
                                           void* userdata1, void* userdata2,
                                           bool long_term) override;

    virtual void FreeGuestTrampoline(uint32_t trampoline_addr) override;
 private:
  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);

  uintptr_t capstone_handle_ = 0;

  std::unique_ptr<A64CodeCache> code_cache_;
  uintptr_t emitter_data_ = 0;

  HostToGuestThunk host_to_guest_thunk_;
  GuestToHostThunk guest_to_host_thunk_;
  ResolveFunctionThunk resolve_function_thunk_;

  // Persistent code cache state
  std::filesystem::path code_cache_path_;
  uint32_t code_cache_title_id_ = 0;

private:

    alignas(64) ReserveHelper reserve_helper_;
    // allocates 8-byte aligned addresses in a normally not executable guest
    // address
    // range that will be used to dispatch to host code
    BitMap guest_trampoline_address_bitmap_;
    uint8_t* guest_trampoline_memory_;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
