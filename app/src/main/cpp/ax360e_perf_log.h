// SPDX-License-Identifier: WTFPL
// ax360e Performance & Diagnostic Logging
// Lightweight instrumentation for GPU (texture/turnip) and audio issues.

#ifndef AX360E_PERF_LOG_H
#define AX360E_PERF_LOG_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <android/log.h>

// ---------- Tags -----------------------------------------------------------
#define PERF_TAG      "ax360e_perf"
#define TEX_TAG       "ax360e_tex"
#define AUDIO_TAG     "ax360e_audio"
#define TURNIP_DIAG   "ax360e_turnip"
#define FRAME_TAG     "ax360e_frame"

// ---------- Log helpers (always go to logcat) ------------------------------
#define PERF_LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  PERF_TAG,  __VA_ARGS__)
#define PERF_LOGW(...)  __android_log_print(ANDROID_LOG_WARN,  PERF_TAG,  __VA_ARGS__)
#define PERF_LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, PERF_TAG,  __VA_ARGS__)

#define TEX_LOGI(...)   __android_log_print(ANDROID_LOG_INFO,  TEX_TAG,   __VA_ARGS__)
#define TEX_LOGW(...)   __android_log_print(ANDROID_LOG_WARN,  TEX_TAG,   __VA_ARGS__)
#define TEX_LOGE(...)   __android_log_print(ANDROID_LOG_ERROR, TEX_TAG,   __VA_ARGS__)

#define AUDIO_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  AUDIO_TAG, __VA_ARGS__)
#define AUDIO_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  AUDIO_TAG, __VA_ARGS__)
#define AUDIO_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, AUDIO_TAG, __VA_ARGS__)

#define TURNIP_DIAG_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TURNIP_DIAG, __VA_ARGS__)
#define TURNIP_DIAG_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TURNIP_DIAG, __VA_ARGS__)
#define TURNIP_DIAG_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TURNIP_DIAG, __VA_ARGS__)

#define FRAME_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  FRAME_TAG, __VA_ARGS__)
#define FRAME_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  FRAME_TAG, __VA_ARGS__)

namespace ax360e {
namespace perf {

// ---------- Lightweight frame-time tracker ---------------------------------
// Call Tick() once per presented frame.  Every `report_interval` frames it
// logs min/max/avg frame time and FPS to logcat under FRAME_TAG.
class FrameTimeTracker {
 public:
  explicit FrameTimeTracker(uint32_t report_interval = 120)
      : report_interval_(report_interval) {}

  void Tick() {
    auto now = std::chrono::steady_clock::now();
    if (frame_count_ > 0) {
      double dt_ms = std::chrono::duration<double, std::milli>(
                         now - last_frame_time_)
                         .count();
      sum_ms_ += dt_ms;
      if (dt_ms < min_ms_) min_ms_ = dt_ms;
      if (dt_ms > max_ms_) max_ms_ = dt_ms;
    }
    last_frame_time_ = now;
    ++frame_count_;

    if (frame_count_ >= report_interval_) {
      double avg_ms = sum_ms_ / frame_count_;
      double avg_fps = (avg_ms > 0.0) ? (1000.0 / avg_ms) : 0.0;
      FRAME_LOGI("frames=%u  avg=%.2f ms (%.1f fps)  min=%.2f ms  max=%.2f ms",
                 frame_count_, avg_ms, avg_fps, min_ms_, max_ms_);
      Reset();
    }
  }

  void Reset() {
    frame_count_ = 0;
    sum_ms_  = 0.0;
    min_ms_  = 1e9;
    max_ms_  = 0.0;
  }

 private:
  uint32_t report_interval_;
  uint32_t frame_count_ = 0;
  double sum_ms_  = 0.0;
  double min_ms_  = 1e9;
  double max_ms_  = 0.0;
  std::chrono::steady_clock::time_point last_frame_time_;
};

// ---------- Texture-upload tracker -----------------------------------------
// Counts texture uploads / format conversions per reporting window.
class TextureUploadTracker {
 public:
  explicit TextureUploadTracker(uint32_t report_interval = 300)
      : report_interval_(report_interval) {}

  void RecordUpload(uint32_t bytes, uint32_t format_id, bool software_decode) {
    uploads_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(bytes, std::memory_order_relaxed);
    if (software_decode) {
      sw_decodes_.fetch_add(1, std::memory_order_relaxed);
    }
    MaybeReport();
  }

  void RecordSamplerCreation() {
    samplers_.fetch_add(1, std::memory_order_relaxed);
  }

  void RecordSamplerOverflow() {
    sampler_overflows_.fetch_add(1, std::memory_order_relaxed);
    TEX_LOGW("Sampler overflow – awaiting GPU submission to free samplers");
  }

  void RecordTextureCreateFailure(uint32_t format_id, uint32_t width,
                                  uint32_t height) {
    create_fails_.fetch_add(1, std::memory_order_relaxed);
    TEX_LOGE("Texture create FAILED: fmt=0x%X  %ux%u", format_id, width,
             height);
  }

 private:
  void MaybeReport() {
    uint32_t n = uploads_.load(std::memory_order_relaxed);
    if (n > 0 && (n % report_interval_) == 0) {
      uint64_t b = bytes_.load(std::memory_order_relaxed);
      uint32_t sw = sw_decodes_.load(std::memory_order_relaxed);
      uint32_t smp = samplers_.load(std::memory_order_relaxed);
      uint32_t sov = sampler_overflows_.load(std::memory_order_relaxed);
      uint32_t cf = create_fails_.load(std::memory_order_relaxed);
      TEX_LOGI("uploads=%u  bytes=%llu MB  sw_decode=%u  "
               "samplers=%u  sampler_overflow=%u  create_fail=%u",
               n, (unsigned long long)(b / (1024 * 1024)), sw,
               smp, sov, cf);
    }
  }

  uint32_t report_interval_;
  std::atomic<uint32_t> uploads_{0};
  std::atomic<uint64_t> bytes_{0};
  std::atomic<uint32_t> sw_decodes_{0};
  std::atomic<uint32_t> samplers_{0};
  std::atomic<uint32_t> sampler_overflows_{0};
  std::atomic<uint32_t> create_fails_{0};
};

// ---------- Audio-health tracker -------------------------------------------
// Tracks underruns, overruns, callback latency.
class AudioHealthTracker {
 public:
  explicit AudioHealthTracker(uint32_t report_interval = 500)
      : report_interval_(report_interval) {}

  // Call at the top of the AAudio data callback.
  void OnCallbackStart() {
    callback_start_ = std::chrono::steady_clock::now();
  }

  // Call at the bottom of the AAudio data callback.
  void OnCallbackEnd() {
    auto now = std::chrono::steady_clock::now();
    double dt_us = std::chrono::duration<double, std::micro>(
                       now - callback_start_)
                       .count();
    if (dt_us > peak_callback_us_) peak_callback_us_ = dt_us;
    sum_callback_us_ += dt_us;
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  void RecordUnderrun() {
    uint32_t n = underruns_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log first 5 individually, then only every 50th
    if (n <= 5 || (n % 50) == 0) {
      AUDIO_LOGW("Audio UNDERRUN #%u (empty queue → silence inserted)", n);
    }
  }

  void RecordOverrun() {
    uint32_t n = overruns_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || (n % 50) == 0) {
      AUDIO_LOGW("Audio OVERRUN #%u (queue full → frame dropped)", n);
    }
  }

  void RecordStreamError(int32_t error_code) {
    stream_errors_.fetch_add(1, std::memory_order_relaxed);
    AUDIO_LOGE("AAudio stream error=%d  total_errors=%u", error_code,
               stream_errors_.load(std::memory_order_relaxed));
  }

  void RecordQueueDepth(uint32_t depth) {
    last_queue_depth_.store(depth, std::memory_order_relaxed);
  }

  void RecordDynamicAlloc() {
    dynamic_allocs_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  void MaybeReport() {
    uint32_t n = callbacks_.load(std::memory_order_relaxed);
    if (n > 0 && (n % report_interval_) == 0) {
      double avg_us = sum_callback_us_ / n;
      AUDIO_LOGI(
          "callbacks=%u  avg=%.0f us  peak=%.0f us  "
          "underruns=%u  overruns=%u  stream_errs=%u  "
          "queue_depth=%u  dynamic_allocs=%u",
          n, avg_us, peak_callback_us_,
          underruns_.load(std::memory_order_relaxed),
          overruns_.load(std::memory_order_relaxed),
          stream_errors_.load(std::memory_order_relaxed),
          last_queue_depth_.load(std::memory_order_relaxed),
          dynamic_allocs_.load(std::memory_order_relaxed));
    }
  }

  uint32_t report_interval_;
  std::chrono::steady_clock::time_point callback_start_;
  double peak_callback_us_ = 0.0;
  double sum_callback_us_  = 0.0;
  std::atomic<uint32_t> callbacks_{0};
  std::atomic<uint32_t> underruns_{0};
  std::atomic<uint32_t> overruns_{0};
  std::atomic<uint32_t> stream_errors_{0};
  std::atomic<uint32_t> last_queue_depth_{0};
  std::atomic<uint32_t> dynamic_allocs_{0};
};

// ---------- Turnip driver diagnostics --------------------------------------
// One-time dump of Turnip-relevant device info after Vulkan init.
inline void LogTurnipDiagnostics(const char* device_name,
                                 uint32_t vendor_id,
                                 uint32_t device_id,
                                 uint32_t driver_version,
                                 uint32_t api_version,
                                 bool has_bc1, bool has_bc2, bool has_bc3,
                                 bool has_bc4, bool has_bc5,
                                 bool has_frag_interlock,
                                 bool has_tile_image,
                                 bool has_non_seamless_cube) {
  TURNIP_DIAG_LOGI("=== Turnip/GPU Diagnostics ===");
  TURNIP_DIAG_LOGI("Device:  %s", device_name);
  TURNIP_DIAG_LOGI("Vendor:  0x%04X  DevID: 0x%04X", vendor_id, device_id);
  TURNIP_DIAG_LOGI("Driver:  %u.%u.%u  API: %u.%u.%u",
                   (driver_version >> 22) & 0x3FF,
                   (driver_version >> 12) & 0x3FF,
                   driver_version & 0xFFF,
                   (api_version >> 22) & 0x3FF,
                   (api_version >> 12) & 0x3FF,
                   api_version & 0xFFF);
  TURNIP_DIAG_LOGI("BCn support: BC1=%d BC2=%d BC3=%d BC4=%d BC5=%d",
                   has_bc1, has_bc2, has_bc3, has_bc4, has_bc5);
  TURNIP_DIAG_LOGI("Features: frag_interlock=%d  tile_image=%d  non_seamless_cube=%d",
                   has_frag_interlock, has_tile_image, has_non_seamless_cube);

  // Flag known problematic configurations
  if (vendor_id == 0x5143) {  // Qualcomm/Adreno
    if (has_bc1 || has_bc2 || has_bc3 || has_bc4 || has_bc5) {
      TURNIP_DIAG_LOGW("Adreno+Turnip with BCn: hardware decode may be "
                       "incorrect! Check force_s3tc_software_decode cvar.");
    }
  }

  // Log current TU_DEBUG state
  const char* tu_debug = std::getenv("TU_DEBUG");
  const char* fd_feat  = std::getenv("FD_DEV_FEATURES");
  const char* mesa_dbg = std::getenv("FD_MESA_DEBUG");
  TURNIP_DIAG_LOGI("TU_DEBUG=%s", tu_debug ? tu_debug : "(none)");
  TURNIP_DIAG_LOGI("FD_DEV_FEATURES=%s", fd_feat ? fd_feat : "(none)");
  TURNIP_DIAG_LOGI("FD_MESA_DEBUG=%s", mesa_dbg ? mesa_dbg : "(none)");
  TURNIP_DIAG_LOGI("=== End Diagnostics ===");
}

// ---------- CPU Accuracy & Diagnostics Tracker -----------------------------
// Lightweight, thread-safe counters for measuring emulator accuracy on device.
// Examples: unhandled PPC instructions encountered, reservation (lwarx/stwcx)
// success vs failure rates (critical for atomicity bugs), timebase reads/sec.
// 128B stress + PAIRED-SINGLE (ps_*) accuracy (R1 research report author harness).
// + Xenon barriers quick-win (per-type LIGHT_SYNC/FULL/IO/INSTR emission counters under a64_accuracy_debug).
// Designed for low overhead (relaxed atomics) and easy extension.
// Reports periodically to logcat under PERF_TAG; snapshot available via JNI.
// CAPTAIN RE-TASK (this cycle): enriched with psq_l/psq_st dedicated roundtrip counters
// (quantized load/store roundtrips), GQR cases, 128B psq_st reservation invalidation +
// pairing violation cases. Sequences title-derived per original R1 55-tool ps_* report
// (psq_l load side, psq_st store skeleton + GQR, explicit 128B warnings). See RecordPsqL*,
// RecordPsqS*, RecordPsqGQR*, new atomics, snapshot, and RunPairedSingleAccuracyHarness
// (a64_backend.cc) for implementation. Leverages recent GQR + psq_st work.
class CpuAccuracyTracker {
 public:
  explicit CpuAccuracyTracker(uint32_t report_interval = 10000)
      : report_interval_(report_interval) {}

  // Call when a guest PPC instruction has no emitter/sequence (fallback path hit).
  // Pass the raw opcode for optional future decoding in reports.
  void RecordUnhandledGuestInstruction(uint32_t guest_opcode = 0) {
    unhandled_.fetch_add(1, std::memory_order_relaxed);
    if (guest_opcode != 0) {
      last_unhandled_opcode_.store(guest_opcode, std::memory_order_relaxed);
    }
    MaybeReport();
  }

  // Reservation (l*arx) acquire: records that a reservation was established.
  void RecordReservationAcquire() {
    res_acquires_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // Reservation store succeeded (stwcx. returned success / CR0[EQ] set).
  void RecordReservationSuccess() {
    res_successes_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // Reservation store failed (intervening write or addr mismatch or lost monitor).
  void RecordReservationFailure() {
    res_failures_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // Timebase read (mftb / LOAD_CLOCK path executed).
  void RecordTimebaseRead() {
    timebase_reads_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // DEC underflow fired (0x900 pending set via mfdec or fire_time cross in mfspr/mtspr paths).
  // Per Captain DEC timing polish + parallel Agent 3 research (ps_* + DEC work).
  void RecordDecUnderflowFired() {
    dec_underflows_fired_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // DEC re-entered / delivered via Reenter to ivpr+0x900 (CheckDecrementerInterrupt path).
  void RecordDecReentered() {
    dec_reentered_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // Stores that invalidated active reservations via 128B granule overlap (CAPTAIN / Code Agent 2).
  // Incremented once per guest store site (I8..V128, OFFSET, atomic stores) whose emitter
  // includes ClearXenonReservationIfStoreOverlaps (complete coverage). Measures frequency
  // of 128B invalidation paths in titles (JIT site count; complements runtime res_acq/fail).
  void RecordStoreThatInvalidatedReservation() {
    stores_that_invalidated_reservations_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // 128B reservation stress harness (CAPTAIN DIRECT ORDER).
  // Dedicated counters for the minimal debug validation sequences:
  // - crossing_invalidation_tests: number of times a crossing store (X+64/X+127 or V128)
  //   at 128B granule boundary exercised the ClearXenonReservationIfStoreOverlaps path.
  // - false_share_detected: subset where intra-granule false sharing (distinct lockfree
  //   locations in same 128B) was simulated/detected (real Xenon risk for audio/physics).
  // Called by Run128BReservationStressTestHarness (a64_backend) + can be wired from seqs.
  // Citations: 128B granule + per-thread pairing errata + false share from the exact
  // research that produced the 128B invalidation landing in a64_seq_memory.cc.
  void RecordCrossingInvalidationTest(bool false_share_detected = false) {
    crossing_invalidation_tests_.fetch_add(1, std::memory_order_relaxed);
    if (false_share_detected) {
      false_share_detected_.fetch_add(1, std::memory_order_relaxed);
    }
    MaybeReport();
  }

  // CAPTAIN 128B PAIRING ERRATA: dedicated atomic counter + increment for dangerous
  // overlapping reservation patterns across logical threads (SMT / big.LITTLE).
  // Called from ClearXenonReservationIfStoreOverlaps (hot stores) + STORE_RESERVED
  // when a64_accuracy_debug detects another thread holding active overlapping 128B granule.
  // Research citations: per-thread exclusive monitor pairing errata on SMT (no safe handoff),
  // 128B false-sharing risks, explicit store invalidation, PE-local monitors + migration.
  void increment_pairing_violation() {
    reservation_pairing_violations_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // TLB/ERAT management op ignored counter (CAPTAIN R2 direct follow-through).
  // Lightweight dedicated metric (modeled exactly on 128B + DEC + ps_* counters).
  // Incremented for tlbie, tlbsync, slbie, slbia, tlbivax, tlbsx, tlbre, tlbwe etc.
  // when treated as NOP (with optional XELOGW + counter) under a64_accuracy_debug.
  // Per R2 TLB/ERAT research: full guest TLB low priority / near-zero title impact
  // on current flat address space + HLE MMU model (Xenon ERAT realities, hashed
  // page tables, 4K/64K/16M pages, no real benefit for 99% titles). This provides
  // the minimal hardening: visibility + no-op safety net without perf cost.
  // NEW (re-task): now also incremented from TLB-aware sequences inside the active
  // ps_* + 128B accuracy harness (RunPairedSingleAccuracyHarness in a64_backend.cc).
  // This directly plugs TLB debug (big.LITTLE shootdown + psq_st/res mig, TLB+psq
  // prot fault via ESR, TLB+FPU 128B store) into R1 ps validation harness.
  // Citations: R2 report conclusions + this file + ppc_hir_builder.cc (early hook) +
  // a64_backend.cc (TLB seqs in harness) + 128B/pairing + R1 ps report.
  // Also reuses generic[] extension for future (tlb_misses etc) as noted in class.
  void RecordTlbOpIgnored() {
    tlb_ops_ignored_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // R1 (original ps_* research report author) + CAPTAIN: ps_* accuracy counters.
  // Dedicated lightweight metrics for the paired-single validation harness (modeled on 128B).
  // THIS TASK: + psq_st_executed / psq_st_reservation_invalidation / psq_st_gqr_cases
  // (from GQR quant in psq_st skeletons + harness; full 128B coverage on stores).
  // - ps_arith_executed: total ps_addx/maddx/msubx etc. element executions (ps0+ps1 count).
  // - psq_load_store_count: quantized psq_l/psq_st (and forms) simulated/exercised.
  // - ps_fma_cases: fused madd/msub paths hit (critical for vertex/skin/anim/physics fidelity).
  // - ps_nan_denorm_edge_hits: NaN/denorm/SNaN edge sequences exercised (R1 accuracy requirement
  //   for independent per-element behavior on real Xenon PPE vs host FPCR).
  // Called by RunPairedSingleAccuracyHarness() (a64_backend.cc) + future wiring from
  // ps_* emitters in ppc_emit_fpu + a64_sequences once they land.
  // Citations: R1 55-tool report (ps_maddx highest, psq massive impact, GQR, *explicit*
  // 128B psq_st store reservation interaction warning that drove ClearXenon... work).
  // See also ppc_emit_fpu.cc master plan + a64_seq_memory.cc psq_st comments.
  // CAPTAIN RE-TASK ADDITION (below RecordPsq*): new psq_store_reservation_pairing_violation
  // + psq_reservation_invalidation_tests for exact cross-thread psq_st 128B pattern.
  // THIS RE-TASK (new RecordPs* below): ps_fma_executed / ps_nan_cases / ps_denorm_handled
  // (ps_* specific debug paths/sequences gated a64_accuracy_debug, for R1 harness).
  // R1 AUTHOR (validator-in-chief) ENRICH: + ps_sub_sel_arith + ps_gqr_vbo_fidelity_cases +
  // ps_fullstack_tlb_barrier_psq (uncovered R1 55-tool edges for sub/sel, VBO quant fidelity,
  // full-stack TLB+barrier+psq+128B+audio/physics lockfree integration).
  void RecordPairedSingleArith(uint32_t elements = 2) {
    ps_arith_executed_.fetch_add(elements, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPairedSingleFMA() {
    ps_fma_cases_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPairedSingleQLoadStore(uint32_t count = 1) {
    psq_load_store_count_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPairedSingleNaNDenormEdge() {
    ps_nan_denorm_edge_hits_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // CAPTAIN RE-TASK: psq_store_reservation_pairing_violation counters (integrates 128B pairing
  // errata enforcement mastery directly into active ps_* harness).
  // 1. psq_store_reservation_pairing_violations: incremented when a simulated or real psq_st
  //    (paired-single quantized store, treated as normal store per R1 ps_* report) from one
  //    logical/SMT thread overlaps a 128B granule holding a reservation from another thread.
  //    Fires the cross-thread probe (XenonReservesOverlapAcrossThreads + last_* + bitmap in
  //    ClearXenonReservationIfStoreOverlaps) + XELOGW + increment_pairing_violation + DebugBreak
  //    (under a64_accuracy_debug).
  // 2. psq_reservation_invalidation_tests: harness sequences/modes dedicated to psq_st + res
  //    pairing violation patterns (lwarx on thread A in granule; psq_st crossing from thread B).
  // Citations (tied in harness + here): 128B pairing report (ClearXenon, per-thread SMT errata,
  // XenonReservesOverlapAcrossThreads, last_reserving_thread_id/granule capture in LOAD_RESERVED)
  // + R1 ps_* report ("psq_st as normal stores that must respect 128B invalidation + per-thread
  // monitor rules"; explicit warning that drove F*/psq store Clear wiring).
  // Gated: a64_ps_accuracy_stress || a64_accuracy_debug. New counters surfaced in snapshots.
  void RecordPsqStoreReservationPairingViolation() {
    psq_store_reservation_pairing_violations_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsqReservationPairingTest(bool violation_detected = false) {
    psq_reservation_invalidation_tests_.fetch_add(1, std::memory_order_relaxed);
    if (violation_detected) {
      psq_store_reservation_pairing_violations_.fetch_add(1, std::memory_order_relaxed);
    }
    MaybeReport();
  }

  // CAPTAIN RE-TASK (this cycle - recovery): NEW dedicated psq_l/psq_st roundtrip + GQR counters.
  // Enriching RunPairedSingleAccuracyHarness with realistic, title-derived psq_l / psq_st sequences
  // (quantized load/store roundtrips, GQR cases, 128B psq_st reservation invalidation + pairing
  // violation cases per original R1 55-tool ps_* research report warnings).
  // Leverages successful psq_st store skeleton (a64_seq_memory) + recent GQR work that landed.
  // psq_l_roundtrips: psq_l/psq_lu/psq_lx/etc quantized FP loads exercised as load-side roundtrips.
  // psq_st_roundtrips: psq_st quantized stores (normal store semantics per R1) in harness.
  // psq_gqr_config_cases: Graphics Quantization Register (GQR0-7) modes/configs hit (quant scale/exp).
  // Citations (R1 author): R1 55-tool report (psq massive title impact, psq_st "as normal stores"
  // requiring 128B invalidation + per-thread monitor rules; explicit warning drove ClearXenon...
  // + F* psq store wiring). GQR quantization critical for vertex/skin/anim/physics fidelity.
  // See harness sequences + ppc_emit_fpu.cc master plan for lowering.
  void RecordPsqLQuantizedLoadRoundtrip(uint32_t count = 1) {
    psq_l_roundtrip_count_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsqSQuantizedStoreRoundtrip(uint32_t count = 1) {
    psq_st_roundtrip_count_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsqGQRCase(uint32_t gqr_id = 0) {
    psq_gqr_config_cases_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // CAPTAIN RE-TASK (psq_st GQR integration + harness): dedicated psq_st-specific
  // accuracy/validation counters (e.g. psq_st_executed, psq_st_reservation_invalidation,
  // psq_st_gqr_cases) that can be triggered from / reported into the ps_* validation
  // harness R1 is building. Complements existing psq_st_roundtrip etc.
  // - psq_st_executed: real or harness-simulated psq_st (any form) quantized stores.
  // - psq_st_reservation_invalidation: psq_st cases that hit 128B Clear (ties to skeleton f.Store).
  // - psq_st_gqr_cases: GQR config/quant cases exercised specifically on psq_st store paths.
  // Citations: this Captain re-task (GQR quant in psq_st skeletons + harness),
  //   own psq_st skeleton (ppc_emit_memory.cc:1172+), 128B (a64_seq_memory.cc),
  //   R1 ps report (psq_st as stores + GQR + reservation interaction),
  //   GQR foundation (ppc_context.h:593).
  void RecordPsqStExecuted(uint32_t count = 1) {
    psq_st_executed_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsqStReservationInvalidation() {
    psq_st_reservation_invalidation_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsqStGqrCase() {
    psq_st_gqr_cases_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // CAPTAIN RE-TASK (psq_l load-side skeleton delivery - symmetric to psq_st store):
  // New dedicated psq_l counters + Record* for the symmetric reservation/pairing
  // sequences in ps_* harness (lwarx on A + psq_l crossing from B for EA/res
  // tracking validation on acquire/load side; does not fire store violation but
  // exercises correct EA calc in load paths + pairing primitives).
  // psq_l_executed: harness-sim or real psq_l (any form) quantized loads (placeholder).
  // psq_l_reservation_invalidation_tests: dedicated load-side harness seqs/modes
  //   (ties to 128B pairing enforcement + psq_l skeleton EA correctness).
  // Citations: pairing enforcement (last_reserving... XenonReserves... Clear probe
  // a64_seq_memory), psq_st skeleton + this psq_l (ppc_emit_memory), R1 ps report,
  // 128B complete coverage. Complements psq_l_roundtrip_count_ etc. Gated under
  // a64_*_debug stress.
  void RecordPsqLExecuted(uint32_t count = 1) {
    psq_l_executed_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsqLReservationPairingTest(bool cross_load_case = false) {
    psq_l_reservation_invalidation_tests_.fetch_add(1, std::memory_order_relaxed);
    if (cross_load_case) {
      // Load from B does not invalidate (unlike store); still bump shared for harness
      // visibility + exercises res overlap calc on load EA (no violation expected).
      psq_reservation_invalidation_tests_.fetch_add(1, std::memory_order_relaxed);
    }
    MaybeReport();
  }

  // ========================================================================
  // CAPTAIN RE-TASK (this cycle - recovery, strict 8+ edits): DEDICATED PSQ_L / PSQ_ST
  // SEQUENCES (title-derived from original R1 55-tool ps_* research report by this author).
  // Added to enrich RunPairedSingleAccuracyHarness (a64_backend.cc) + counters.
  // Leverages successful psq_st store skeleton + recent GQR work that landed.
  //
  // 1. psq_l quantized LOAD roundtrips (load-side skeleton):
  //    for (gqr = 0..7) { psq_l (offset forms), psq_lu, psq_lx; RecordPsqLQuantizedLoadRoundtrip();
  //    RecordPsqGQRCase(gqr); }  // per-el accuracy vs Xenon PPE ref
  //
  // 2. psq_st quantized STORE roundtrips (store skeleton + GQR):
  //    psq_st / psq_stu / psq_stx with GQR scale/exp; roundtrip store+reload verify.
  //    RecordPsqSQuantizedStoreRoundtrip(); RecordPsqStExecuted(); RecordPsqStGqrCase();
  //
  // 3. 128B psq_st reservation INVALIDATION cases (R1 explicit warning):
  //    Thread A: lwarx in granule; Thread B: psq_st overlapping 128B -> ClearXenon...
  //    RecordPsqReservationPairingTest(violation=true); RecordPsqStReservationInvalidation();
  //    Citations: R1 ("psq_st as normal stores ... 128B invalidation + per-thread monitor rules";
  //    warning drove Clear wiring in a64_seq_memory + ppc_emit).
  //
  // 4. psq_l/psq_st PAIRING VIOLATION cases (cross-SMT 128B res overlap):
  //    Mixed load/store quantized ops + res from different logical threads.
  //    RecordPsqStoreReservationPairingViolation(); increment_pairing_violation();
  //    Per R1 + 128B pairing errata (XenonReservesOverlapAcrossThreads, PE-local monitors).
  //
  // 5. [THIS RE-TASK] Symmetric psq_l load-side + reservation/pairing sequences:
  //    lwarx on logical A in 128B granule; psq_l (placeholder load skeleton) crossing
  //    from B (exercises EA calc + res tracking primitives on load paths; no store
  //    invalidation). RecordPsqLReservationPairingTest(true); RecordPsqLExecuted();
  //    Ties directly to psq_l skeleton EA correctness + pairing enforcement (Clear probe
  //    + last_* fields only fire on stores; loads validate non-interference).
  //
  // Integration point: Call from RunPairedSingleAccuracyHarness under a64_accuracy_debug /
  // ps_accuracy_stress. All Record* + atomics (incl new psq_l_executed_ + _res_*_tests_)
  // wired into snapshot/Reset/MaybeReport. Heavy citations to pairing enforcement work,
  // psq_st skeleton, R1 ps report, 128B complete coverage throughout.
  // ========================================================================

  // CAPTAIN RE-TASK (this job): additional ps_* specific accuracy debug counters + paths.
  // Gated under a64_accuracy_debug (or ps_accuracy_stress) for consumption by R1 validation
  // harness (a64_backend RunPairedSingleAccuracyHarness + emitter sites in ppc_emit_fpu).
  // - ps_fma_executed: explicit FMA (madd/msub) ops from ps_* family (distinguishes from generic).
  // - ps_nan_cases: NaN propagation/quieting cases hit in ps single-prec lowering (FPCR tie-in).
  // - ps_denorm_handled: denormal flush-to-zero or arithmetic on denorms in ps0/ps1 elements.
  // These provide finer visibility for on-device testing vs existing ps_fma_cases / nan_denorm_edge.
  // References: master PAIRED-SINGLE plan (ppc_emit_fpu.cc), a64_sequences fpcr_table, R1 report.
  void RecordPsFmaExecuted(uint32_t count = 1) {
    ps_fma_executed_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsNanCase() {
    ps_nan_cases_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsDenormHandled() {
    ps_denorm_handled_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // R1 (original ps_* research author) + CAPTAIN: additional per-element NaN/denorm/rounding/FPCR
  // edge counters identified in the 55-tool paired-singles report but not yet covered in base harness.
  // - ps_mixed_element_edge_hits: cases where ps0 and ps1 differ in class (normal + denorm,
  //   normal + NaN, signed overflow in one element only). Real Xenon PPE applies FPCR rules
  //   independently per half (critical for skinning/physics where one component denorms).
  // - ps_rounding_edge_cases: rounding mode / tie-to-even or tie-away effects on ps madd/add
  //   results, esp. exact 0.5 cases + FPCR RN/RZ/RP/RM. Ties directly to fpcr_table + SET_ROUNDING
  //   in a64_sequences.cc and per-ps-element Convert in ps_* emitters (ppc_emit_fpu.cc).
  // Citations: R1 55-tool report (explicit per-element FPCR/NaN/denorm/rounding fidelity reqs for
  // UE3/Forza/Halo vertex/skin/anim/physics; SNaN payload + mixed-class pairs observed in VBO
  // transform chains); GQR/psq + 128B notes; fpcr_table comments. Gated under accuracy_debug/ps stress.
  // Consumed by enriched RunPairedSingleAccuracyHarness sequences (new title-derived edges).
  void RecordPsMixedElementEdge() {
    ps_mixed_element_edge_hits_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsRoundingEdge() {
    ps_rounding_edge_cases_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // R1 AUTHOR (validator-in-chief) + CAPTAIN RE-TASK: additional uncovered edge counters from
  // original 55-tool ps_* research report (not yet covered in prior harness iterations).
  // - ps_sub_sel_arith: executions of ps_subx (independent sub per ps0/ps1) + ps_sel (per-elem
  //   conditional select; high value in anim/shader conditionals per R1 title analysis).
  // - ps_gqr_vbo_fidelity: exact roundtrip fidelity cases for realistic VBO GQR configs
  //   (s16 pos scale 3-6, s8 normals, u16 tex; R1 stressed quant/dequant accuracy critical
  //   for vertex/skin/anim data paths; new harness sequences validate no drift vs Xenon PPE).
  // - ps_fullstack_tlb_barrier_psq: deeper full-stack integration hits (psq_l/psq_st GQR quant
  //   + live sub/sel arith + barriers lwsync + TLB shootdown + 128B/pairing enforcement in one
  //   realistic scenario: e.g. physics audio lockfree update + TLB mig on big.LITTLE + psq_st
  //   quantized state write crossing res granule). Surfaces combined correctness on Adreno.
  // Citations: R1 55-tool report (ps_sub/ps_sel in anim/physics/vertex conditional + sub chains;
  // GQR fidelity for VBOs; explicit lockfree+quantized+barrier+TLB co-occurrence risks in audio/
  // physics; per-elem FPCR + 128B psq_st warnings); full-stack ties recent ps arith (sub/sel
  // in ppc_emit_fpu) + GQR (ppc_context) + psq skeletons + barriers + TLB (R2) + 128B/pairing.
  // Gated under a64_ps_accuracy_stress / a64_accuracy_debug. Lightweight atomics.
  void RecordPsSubSelArith(uint32_t count = 1) {
    ps_sub_sel_arith_.fetch_add(count, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsGQRVBOFidelity() {
    ps_gqr_vbo_fidelity_cases_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordPsFullstackTlbBarrierPsq() {
    ps_fullstack_tlb_barrier_psq_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // CAPTAIN quick-win diagnostics (from Xenon barriers research report): per-barrier-type
  // emission counters + stats for LIGHT_SYNC (lwsync), FULL_SYNC (sync/hwsync), IO (eieio),
  // INSTRUCTION (isync). Exposed via CpuAccuracyTracker snapshot / Java perf monitor.
  // Gated: only incremented under a64_accuracy_debug from MEMORY_BARRIER emitter (a64_seq_memory).
  // Purpose: observability on real Adreno (big.LITTLE) to see which barriers shipping titles
  // actually emit (lwsync known dominant in audio/physics/lock-free per research). Enables
  // validation of weakest-sufficient lowering quality + deepened lighter ISHLD/ISHST experiments
  // (fidelity variants via a64_light_sync_fidelity, harness validation cases integrated with
  // 128B/pairing/ps_sub/sel/GQR/psq/TLB full stack).
  // Citations: own barriers research (sync/lwsync/eieio/isync usage in real titles + AArch64
  // DMB ISH vs real Xenon weakness analysis); HIR opcodes.h MEMORY_BARRIER_TYPE_*; ppc_emit_*.cc
  // lwsync/eieio/isync paths; a64_seq_memory.cc full lowering comments + deepened opt-in experiment
  // (this re-task) + a64_backend harness full-stack sequences.
  void RecordBarrierLightSync() {
    barriers_light_sync_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordBarrierFullSync() {
    barriers_full_sync_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordBarrierIO() {
    barriers_io_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }
  void RecordBarrierInstruction() {
    barriers_instruction_.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
  }

  // Generic extension point: increment any future counter by ID (0-31 for now).
  // Allows adding new metrics without changing the class signature immediately.
  // (TLB now plugged into ps/128B harness via tlb_ops_ignored reuse; future tlb_misses via generic or dedicated)
  void RecordGeneric(uint32_t metric_id, uint64_t count = 1) {
    if (metric_id < 32) {
      generic_[metric_id].fetch_add(count, std::memory_order_relaxed);
    }
    MaybeReport();
  }

  // Returns a human-readable snapshot string (for JNI / Java display or logging).
  // Includes totals + rough success rate for reservations.
  // Now also surfaces 128B reservation stress harness counters (crossing + false share).
  // R1 ps_* extension: ps_arith_executed, ps_fma_cases, psq_load_store_count, nan/denorm edges
  // (from RunPairedSingleAccuracyHarness + future emitter sites).
  // CAPTAIN RE-TASK: + psq_store_res_pair_viol + psq_res_pair_tests (128B pairing + psq_st cross-thread).
  // CAPTAIN barriers quick-win: + barriers_light_sync etc (per-type emission stats under a64_accuracy_debug).
  // THIS CYCLE RE-TASK (recovery + psq_l pure load skeleton): + psq_l_roundtrips + psq_st_roundtrips + psq_gqr_cases
  // + psq_st_executed etc + NEW psq_l_executed + psq_l_reservation_invalidation_tests (symmetric
  // load-side reservation/pairing: lwarx A, psq_l B crossing-granule for correct EA/res tracking
  // validation per 128B pairing enforcement). Leverages psq_l skeleton (ppc_emit_memory placeholder)
  // + psq_st validated store + pairing work (last_* / XenonReserves... / Clear probe). Heavy R1
  // + 128B complete citations. See harness + Record*.
  std::string GetSnapshotString() const {
    uint64_t un = unhandled_.load(std::memory_order_relaxed);
    uint64_t acq = res_acquires_.load(std::memory_order_relaxed);
    uint64_t suc = res_successes_.load(std::memory_order_relaxed);
    uint64_t fai = res_failures_.load(std::memory_order_relaxed);
    uint64_t tb = timebase_reads_.load(std::memory_order_relaxed);
    uint64_t cross = crossing_invalidation_tests_.load(std::memory_order_relaxed);
    uint64_t fshare = false_share_detected_.load(std::memory_order_relaxed);
    uint64_t store_inv = stores_that_invalidated_reservations_.load(std::memory_order_relaxed);
    uint64_t pair_v = reservation_pairing_violations_.load(std::memory_order_relaxed);
    uint64_t dec_uf = dec_underflows_fired_.load(std::memory_order_relaxed);
    uint64_t dec_re = dec_reentered_.load(std::memory_order_relaxed);
    uint64_t tlb_ign = tlb_ops_ignored_.load(std::memory_order_relaxed);
    uint32_t last_op = last_unhandled_opcode_.load(std::memory_order_relaxed);

    // R1 ps_* accuracy metrics (ps_maddx highest, psq + 128B interaction per research report).
    uint64_t ps_arith = ps_arith_executed_.load(std::memory_order_relaxed);
    uint64_t ps_fma = ps_fma_cases_.load(std::memory_order_relaxed);
    uint64_t psq_ls = psq_load_store_count_.load(std::memory_order_relaxed);
    uint64_t ps_edge = ps_nan_denorm_edge_hits_.load(std::memory_order_relaxed);

    // CAPTAIN RE-TASK new psq_store_reservation_pairing_violation counters (128B + R1 ps_* integration)
    uint64_t psq_pair_v = psq_store_reservation_pairing_violations_.load(std::memory_order_relaxed);
    uint64_t psq_res_tests = psq_reservation_invalidation_tests_.load(std::memory_order_relaxed);

    // CAPTAIN RE-TASK (this job): new ps_* specific debug counters (a64_accuracy_debug gated)
    // for finer FMA/nan/denorm visibility in ps_* family (consumable by R1 harness).
    uint64_t ps_fma_exec = ps_fma_executed_.load(std::memory_order_relaxed);
    uint64_t ps_nan = ps_nan_cases_.load(std::memory_order_relaxed);
    uint64_t ps_den = ps_denorm_handled_.load(std::memory_order_relaxed);

    // TLB ops (R2) now also incremented from ps harness TLB-aware seqs (integration complete).

    // R1 AUTHOR ENRICHMENT: load new per-element mixed/rounding FPCR edge counters for snapshot.
    // Exercised by richer title-derived sequences in RunPairedSingleAccuracyHarness.
    uint64_t ps_mixed = ps_mixed_element_edge_hits_.load(std::memory_order_relaxed);
    uint64_t ps_rnd = ps_rounding_edge_cases_.load(std::memory_order_relaxed);

    // R1 AUTHOR (validator-in-chief) NEW: additional uncovered R1 55-tool ps_* edge counters
    // (ps_sub/sel + GQR VBO fidelity + fullstack TLB+barrier+psq integration cases).
    // Populated by new deeper title-derived + full-stack sequences in RunPairedSingleAccuracyHarness.
    uint64_t ps_subsel = ps_sub_sel_arith_.load(std::memory_order_relaxed);
    uint64_t ps_gqr_fid = ps_gqr_vbo_fidelity_cases_.load(std::memory_order_relaxed);
    uint64_t ps_fullstack = ps_fullstack_tlb_barrier_psq_.load(std::memory_order_relaxed);

    // CAPTAIN RE-TASK (psq_st GQR integration): NEW loads for dedicated psq_st counters
    // (psq_st_executed, psq_st_reservation_invalidation, psq_st_gqr_cases).
    // Driven by GQR quant calls in psq_st emitters (ppc_emit_memory.cc) + harness seqs.
    // Citations: R1 + 128B + this task + GQR helpers (ppc_context.h).
    uint64_t psq_st_exec = psq_st_executed_.load(std::memory_order_relaxed);
    uint64_t psq_st_res_inv = psq_st_reservation_invalidation_.load(std::memory_order_relaxed);
    uint64_t psq_st_gqr = psq_st_gqr_cases_.load(std::memory_order_relaxed);

    // CAPTAIN RE-TASK (this cycle - recovery): NEW loads for psq_l/psq_st roundtrip + GQR counters.
    // These will be populated by dedicated sequences added to RunPairedSingleAccuracyHarness
    // (quantized load roundtrips for psq_l, store roundtrips for psq_st leveraging skeleton,
    // GQR cases, 128B reservation invalidation + pairing violation tests per R1 warnings).
    // Citations: R1 55-tool ps_* research report (original author) - psq_l/psq_st paths,
    // GQR quantization impact, psq_st 128B res interaction explicit warning.
    uint64_t psq_l_rt = psq_l_roundtrip_count_.load(std::memory_order_relaxed);
    uint64_t psq_st_rt = psq_st_roundtrip_count_.load(std::memory_order_relaxed);
    uint64_t psq_gqr = psq_gqr_config_cases_.load(std::memory_order_relaxed);

    // CAPTAIN RE-TASK (psq_l load skeleton): load new symmetric psq_l res/pairing counters for snapshot.
    uint64_t psq_l_exec = psq_l_executed_.load(std::memory_order_relaxed);
    uint64_t psq_l_res_tests = psq_l_reservation_invalidation_tests_.load(std::memory_order_relaxed);

    // CAPTAIN quick-win (Xenon barriers research): per-type barrier emission stats (LIGHT_SYNC
    // dominant per report; visible in snapshots under a64_accuracy_debug only).
    uint64_t bar_light = barriers_light_sync_.load(std::memory_order_relaxed);
    uint64_t bar_full = barriers_full_sync_.load(std::memory_order_relaxed);
    uint64_t bar_io = barriers_io_.load(std::memory_order_relaxed);
    uint64_t bar_instr = barriers_instruction_.load(std::memory_order_relaxed);

    double res_rate = (acq > 0) ? (100.0 * suc / (suc + fai)) : 0.0;

    char buf[1200];
    snprintf(buf, sizeof(buf),
             "CPU_ACCURACY: unhandled=%llu last_op=0x%08X "
             "res_acq=%llu res_suc=%llu res_fail=%llu res_success_rate=%.1f%% "
             "timebase_reads=%llu cross_128b_inv=%llu false_share_128b=%llu store_inv_128b=%llu "
             "res_pairing_violations=%llu dec_underflows_fired=%llu dec_reentered=%llu "
             "tlb_ops_ignored=%llu "
             "ps_arith_executed=%llu ps_fma_cases=%llu psq_load_store=%llu ps_nan_denorm_edges=%llu "
             "psq_store_res_pair_viol=%llu psq_res_pair_tests=%llu "
             "ps_fma_executed=%llu ps_nan_cases=%llu ps_denorm_handled=%llu "
             "ps_mixed_element_edges=%llu ps_rounding_edges=%llu "
             "psq_l_roundtrips=%llu psq_st_roundtrips=%llu psq_gqr_cases=%llu "
             "psq_st_executed=%llu psq_st_res_inv=%llu psq_st_gqr_cases=%llu "
             "psq_l_executed=%llu psq_l_res_tests=%llu "
             "barriers_light_sync=%llu barriers_full_sync=%llu barriers_io=%llu barriers_instruction=%llu "
             "ps_sub_sel_arith=%llu ps_gqr_vbo_fidelity=%llu ps_fullstack_tlb_barrier_psq=%llu",
             (unsigned long long)un, last_op,
             (unsigned long long)acq, (unsigned long long)suc, (unsigned long long)fai, res_rate,
             (unsigned long long)tb, (unsigned long long)cross, (unsigned long long)fshare, (unsigned long long)store_inv,
             (unsigned long long)pair_v, (unsigned long long)dec_uf, (unsigned long long)dec_re,
             (unsigned long long)tlb_ign,
             (unsigned long long)ps_arith, (unsigned long long)ps_fma, (unsigned long long)psq_ls, (unsigned long long)ps_edge,
             (unsigned long long)psq_pair_v, (unsigned long long)psq_res_tests,
             (unsigned long long)ps_fma_exec, (unsigned long long)ps_nan, (unsigned long long)ps_den,
             // R1 AUTHOR ENRICH: new mixed/rounding per-element FPCR edge counters in snapshot output.
             // Populated by enriched harness sequences exercising title-derived NaN/denorm/round cases.
             (unsigned long long)ps_mixed, (unsigned long long)ps_rnd,
             // CAPTAIN RE-TASK (this cycle): pass new psq_l/psq_st/GQR roundtrip counters to snprintf
             // for harness reporting. These are driven by psq sequences in RunPairedSingleAccuracyHarness.
             // R1 report citations throughout: quantized load/store roundtrips, GQR, 128B psq_st
             // reservation invalidation + pairing violation cases.
             (unsigned long long)psq_l_rt, (unsigned long long)psq_st_rt, (unsigned long long)psq_gqr,
             // THIS TASK (psq_st GQR): new dedicated psq_st counters in snapshot (from emitter GQR calls + harness).
             (unsigned long long)psq_st_exec, (unsigned long long)psq_st_res_inv, (unsigned long long)psq_st_gqr,
             // CAPTAIN RE-TASK (psq_l load skeleton + symmetric pairing seqs): new psq_l_* in snapshot.
             (unsigned long long)psq_l_exec, (unsigned long long)psq_l_res_tests,
             (unsigned long long)bar_light, (unsigned long long)bar_full, (unsigned long long)bar_io, (unsigned long long)bar_instr,
             // R1 AUTHOR (validator-in-chief) NEW: additional uncovered 55-tool ps_* edges in snapshot
             // (ps_sub/sel + GQR VBO fidelity roundtrips + full-stack TLB+barrier+psq integration).
             // Driven by richer title-derived + deeper full-stack sequences in harness (a64_backend.cc).
             (unsigned long long)ps_subsel, (unsigned long long)ps_gqr_fid, (unsigned long long)ps_fullstack);
    return std::string(buf);
  }

  void Reset() {
    unhandled_.store(0, std::memory_order_relaxed);
    res_acquires_.store(0, std::memory_order_relaxed);
    res_successes_.store(0, std::memory_order_relaxed);
    res_failures_.store(0, std::memory_order_relaxed);
    timebase_reads_.store(0, std::memory_order_relaxed);
    last_unhandled_opcode_.store(0, std::memory_order_relaxed);
    crossing_invalidation_tests_.store(0, std::memory_order_relaxed);
    false_share_detected_.store(0, std::memory_order_relaxed);
    stores_that_invalidated_reservations_.store(0, std::memory_order_relaxed);
    reservation_pairing_violations_.store(0, std::memory_order_relaxed);
    dec_underflows_fired_.store(0, std::memory_order_relaxed);
    dec_reentered_.store(0, std::memory_order_relaxed);
    tlb_ops_ignored_.store(0, std::memory_order_relaxed);
    // R1 ps_* harness counters reset (paired-single accuracy validation).
    ps_arith_executed_.store(0, std::memory_order_relaxed);
    ps_fma_cases_.store(0, std::memory_order_relaxed);
    psq_load_store_count_.store(0, std::memory_order_relaxed);
    ps_nan_denorm_edge_hits_.store(0, std::memory_order_relaxed);
    // CAPTAIN RE-TASK: reset new psq_store_reservation_pairing_violation counters
    psq_store_reservation_pairing_violations_.store(0, std::memory_order_relaxed);
    psq_reservation_invalidation_tests_.store(0, std::memory_order_relaxed);
    // CAPTAIN RE-TASK (this job): reset new ps_* specific debug counters (fma/nan/denorm)
    ps_fma_executed_.store(0, std::memory_order_relaxed);
    ps_nan_cases_.store(0, std::memory_order_relaxed);
    ps_denorm_handled_.store(0, std::memory_order_relaxed);
    // R1 AUTHOR ENRICH: reset new mixed/rounding per-element edge counters.
    ps_mixed_element_edge_hits_.store(0, std::memory_order_relaxed);
    ps_rounding_edge_cases_.store(0, std::memory_order_relaxed);
    // R1 AUTHOR (validator-in-chief) NEW: reset additional uncovered R1 55-tool ps_* edge counters
    // (ps_sub/sel arith, GQR VBO fidelity, fullstack TLB+barrier+psq combos) for clean harness runs.
    ps_sub_sel_arith_.store(0, std::memory_order_relaxed);
    ps_gqr_vbo_fidelity_cases_.store(0, std::memory_order_relaxed);
    ps_fullstack_tlb_barrier_psq_.store(0, std::memory_order_relaxed);
    // CAPTAIN RE-TASK (this cycle - recovery): reset NEW psq_l/psq_st roundtrip + GQR counters.
    // Ensures clean state for repeated RunPairedSingleAccuracyHarness runs exercising
    // quantized load/store roundtrips + GQR cases + 128B psq_st reservation/pairing tests.
    // Citations: R1 55-tool ps_* report (psq_l load roundtrips, psq_st store skeleton,
    // GQR configs, 128B psq_st reservation invalidation + pairing violation warnings).
    psq_l_roundtrip_count_.store(0, std::memory_order_relaxed);
    psq_st_roundtrip_count_.store(0, std::memory_order_relaxed);
    psq_gqr_config_cases_.store(0, std::memory_order_relaxed);
    // CAPTAIN RE-TASK (psq_st GQR): reset dedicated psq_st counters (exercised by
    // GQRQuantize calls inside psq_st emitters + new harness sequences).
    psq_st_executed_.store(0, std::memory_order_relaxed);
    psq_st_reservation_invalidation_.store(0, std::memory_order_relaxed);
    psq_st_gqr_cases_.store(0, std::memory_order_relaxed);
    // CAPTAIN RE-TASK (psq_l load skeleton): reset new symmetric psq_l res/pairing counters.
    psq_l_executed_.store(0, std::memory_order_relaxed);
    psq_l_reservation_invalidation_tests_.store(0, std::memory_order_relaxed);
    // CAPTAIN barriers quick-win diagnostics: reset per-type barrier emission counters (LIGHT etc).
    barriers_light_sync_.store(0, std::memory_order_relaxed);
    barriers_full_sync_.store(0, std::memory_order_relaxed);
    barriers_io_.store(0, std::memory_order_relaxed);
    barriers_instruction_.store(0, std::memory_order_relaxed);
    for (auto& g : generic_) g.store(0, std::memory_order_relaxed);
    report_count_ = 0;
  }

 private:
  void MaybeReport() {
    uint64_t total_events = unhandled_.load(std::memory_order_relaxed) +
                            res_acquires_.load(std::memory_order_relaxed) +
                            res_successes_.load(std::memory_order_relaxed) +
                            res_failures_.load(std::memory_order_relaxed) +
                            timebase_reads_.load(std::memory_order_relaxed) +
                            crossing_invalidation_tests_.load(std::memory_order_relaxed) +
                            false_share_detected_.load(std::memory_order_relaxed) +
                            stores_that_invalidated_reservations_.load(std::memory_order_relaxed) +
                            reservation_pairing_violations_.load(std::memory_order_relaxed) +
                            dec_underflows_fired_.load(std::memory_order_relaxed) +
                            dec_reentered_.load(std::memory_order_relaxed) +
                            tlb_ops_ignored_.load(std::memory_order_relaxed) +
                            // (TLB counter now also driven by ps_* harness TLB seqs per re-task integration)
                            // R1 ps_* accuracy counters included in total for periodic snapshot reporting.
                            ps_arith_executed_.load(std::memory_order_relaxed) +
                            ps_fma_cases_.load(std::memory_order_relaxed) +
                            psq_load_store_count_.load(std::memory_order_relaxed) +
                            ps_nan_denorm_edge_hits_.load(std::memory_order_relaxed) +
                            // CAPTAIN RE-TASK psq_store_reservation_pairing_violation counters
                            psq_store_reservation_pairing_violations_.load(std::memory_order_relaxed) +
                            psq_reservation_invalidation_tests_.load(std::memory_order_relaxed) +
                            // CAPTAIN RE-TASK (this job): new ps_* debug counters in total (for a64_accuracy_debug gated reporting)
                            ps_fma_executed_.load(std::memory_order_relaxed) +
                            ps_nan_cases_.load(std::memory_order_relaxed) +
                            ps_denorm_handled_.load(std::memory_order_relaxed) +
                            // R1 AUTHOR ENRICHMENT: include new mixed_element + rounding edge counters
                            // in total so periodic snapshots fire when harness exercises richer per-element
                            // FPCR/NaN/denorm cases from the 55-tool ps report.
                            ps_mixed_element_edge_hits_.load(std::memory_order_relaxed) +
                            ps_rounding_edge_cases_.load(std::memory_order_relaxed) +
                            // R1 AUTHOR (validator-in-chief) NEW: include new uncovered R1 55-tool ps_* counters
                            // (ps_sub/sel + GQR VBO fidelity + fullstack TLB+barrier+psq) in total_events.
                            // Ensures richer title-derived + deeper integration sequences in RunPairedSingleAccuracyHarness
                            // trigger periodic snapshots with full R1 coverage (sub/sel, quant fidelity, combined
                            // barriers+TLB+psq+128B+lockfree audio/physics scenarios from research).
                            ps_sub_sel_arith_.load(std::memory_order_relaxed) +
                            ps_gqr_vbo_fidelity_cases_.load(std::memory_order_relaxed) +
                            ps_fullstack_tlb_barrier_psq_.load(std::memory_order_relaxed) +
                            // CAPTAIN RE-TASK (this cycle - recovery edit): NEW psq_l/psq_st roundtrip + GQR
                            // counters added to total_events. This ensures RunPairedSingleAccuracyHarness
                            // psq sequences (load/store roundtrips, GQR, 128B psq_st res invalidation/pairing)
                            // trigger periodic snapshot logging.
                            // Citations: original R1 55-tool ps_* research report (psq_l load side,
                            // psq_st as normal stores w/ 128B granule rules, GQR quantization 0-7).
                            psq_l_roundtrip_count_.load(std::memory_order_relaxed) +
                            psq_st_roundtrip_count_.load(std::memory_order_relaxed) +
                            psq_gqr_config_cases_.load(std::memory_order_relaxed) +
                            // CAPTAIN RE-TASK (psq_st GQR): new dedicated psq_st counters in total_events.
                            // (from GQR calls in psq_st* emitters + harness). Ties to 128B coverage.
                            psq_st_executed_.load(std::memory_order_relaxed) +
                            psq_st_reservation_invalidation_.load(std::memory_order_relaxed) +
                            psq_st_gqr_cases_.load(std::memory_order_relaxed) +
                            // CAPTAIN RE-TASK (psq_l load skeleton + harness symmetric res/pairing):
                            // include new psq_l_* counters in total_events (lwarx+psq_l EA/res seqs).
                            psq_l_executed_.load(std::memory_order_relaxed) +
                            psq_l_reservation_invalidation_tests_.load(std::memory_order_relaxed) +
                            // CAPTAIN barriers quick-win: include barrier emission counts in total for periodic reports under debug.
                            barriers_light_sync_.load(std::memory_order_relaxed) +
                            barriers_full_sync_.load(std::memory_order_relaxed) +
                            barriers_io_.load(std::memory_order_relaxed) +
                            barriers_instruction_.load(std::memory_order_relaxed);
    if (total_events > 0 && (total_events % report_interval_) == 0) {
      ++report_count_;
      std::string snap = GetSnapshotString();
      PERF_LOGI("%s (report #%u)", snap.c_str(), report_count_);
    }
  }

  uint32_t report_interval_;
  uint32_t report_count_ = 0;

  std::atomic<uint64_t> unhandled_{0};
  std::atomic<uint32_t> last_unhandled_opcode_{0};

  std::atomic<uint64_t> res_acquires_{0};
  std::atomic<uint64_t> res_successes_{0};
  std::atomic<uint64_t> res_failures_{0};

  std::atomic<uint64_t> timebase_reads_{0};

  // 128B reservation stress harness dedicated counters (CAPTAIN DIRECT ORDER task).
  // crossing_invalidation_tests: exercises of 128B granule crossing store invalidation.
  // false_share_detected: intra-128B false sharing cases (lockfree audio/physics risk).
  std::atomic<uint64_t> crossing_invalidation_tests_{0};
  std::atomic<uint64_t> false_share_detected_{0};

  // 128B complete store coverage counter (Code Agent 2): # of guest store sites instrumented
  // with ClearXenonReservationIfStoreOverlaps (covers all normal + atomic store paths).
  std::atomic<uint64_t> stores_that_invalidated_reservations_{0};

  // CAPTAIN 128B pairing errata counter (Code Agent 1): incremented on detected
  // cross-logical-thread overlapping 128B reservations (SMT per-thread monitor errata violation).
  std::atomic<uint64_t> reservation_pairing_violations_{0};

  // DEC timing precision counters (Captain Agent 4 + parallel research).
  std::atomic<uint64_t> dec_underflows_fired_{0};
  std::atomic<uint64_t> dec_reentered_{0};

  // TLB/ERAT ignored op counter (CAPTAIN R2 TLB research follow-through, minimal hardening).
  // See RecordTlbOpIgnored() for full citations (R2: low priority on flat+HLE, NOP+warn).
  // Re-task integration: exercised by TLB+psq_st / TLB+128B / TLB+big.LITTLE seqs in ps harness.
  std::atomic<uint64_t> tlb_ops_ignored_{0};

  // R1 PAIRED-SINGLE ACCURACY HARNESS counters (CAPTAIN / original ps research report).
  // ps_arith_executed: ps_addx/maddx/msubx element count (ps0+ps1).
  // ps_fma_cases: fused madd/msub exercised (highest ROI per R1 for UE3/Forza/Halo etc.).
  // psq_load_store_count: quantized psq paths (GQR + 128B res interaction critical).
  // ps_nan_denorm_edge_hits: NaN/denorm edge sequences (per-element Xenon semantics).
  // Full citations in Record* methods + RunPairedSingleAccuracyHarness (a64_backend.cc).
  // CAPTAIN RE-TASK ADDITION: psq_store_reservation_pairing_violations + psq_reservation_invalidation_tests
  // (new dedicated for exact lwarx + cross-thread psq_st 128B granule pattern; see Record* + harness).
  // THIS RE-TASK (psq_l load skeleton): + psq_l_executed + psq_l_reservation_invalidation_tests
  // (symmetric load-side: lwarx A + psq_l B for EA/res tracking; pure placeholder psq_l).
  // + ps_fma... (prior). R1 AUTHOR ENRICHMENT (harness owner): + ps_mixed... (per-element...).
  // Heavy cites to pairing enforcement + psq_st skeleton + R1 ps report + 128B coverage.
  // NEW (R1 validator-in-chief): + ps_sub_sel_arith_ + ps_gqr_vbo_fidelity_cases_ + ps_fullstack_tlb_barrier_psq_
  // (uncovered R1 edges: ps_subx/ps_sel, GQR VBO fidelity, full-stack TLB+barrier+psq+128B+lockfree combos).
  // All exercised by even richer title-derived + deeper integration sequences in current harness expansion.
  std::atomic<uint64_t> ps_arith_executed_{0};
  std::atomic<uint64_t> ps_fma_cases_{0};
  std::atomic<uint64_t> psq_load_store_count_{0};
  std::atomic<uint64_t> ps_nan_denorm_edge_hits_{0};

  // CAPTAIN RE-TASK: psq_store_reservation_pairing_violation dedicated counters (128B pairing
  // errata + R1 ps_* report integration in ps harness). See Record* methods + harness sequences
  // for full citations (Xenon 128B granule, per-thread SMT pairing errata, psq_st as normal
  // store requiring ClearXenon invalidation).
  std::atomic<uint64_t> psq_store_reservation_pairing_violations_{0};
  std::atomic<uint64_t> psq_reservation_invalidation_tests_{0};

  // CAPTAIN RE-TASK (this job): ps_* specific accuracy debug counters (gated under
  // a64_accuracy_debug / ps_accuracy_stress). Record via new RecordPs* methods called from
  // harness sequences (a64_backend.cc) + ps_* emitter debug paths (ppc_emit_fpu.cc) + F32
  // lowering notes (a64_sequences.cc). Enables finer on-device ps validation harness (R1).
  // ps_fma_executed: FMA ops from ps family; ps_nan_cases: NaN in single-prec ps lowering;
  // ps_denorm_handled: denorm cases (ties to FPCR FZ in fpcr_table for ps single-prec).
  // References: this file + master plan in ppc_emit_fpu.cc + ps research report.
  // R1 AUTHOR ENRICH (harness): also covers new mixed + rounding edges below.
  std::atomic<uint64_t> ps_fma_executed_{0};
  std::atomic<uint64_t> ps_nan_cases_{0};
  std::atomic<uint64_t> ps_denorm_handled_{0};

  // R1 (original 55-tool ps research) + CAPTAIN: additional per-element edge counters for mixed-class
  // ps0/ps1 (normal+denorm/NaN) and rounding/FPCR tie cases. Not covered in initial harness; added
  // during R1-author enrichment of RunPairedSingleAccuracyHarness with title-derived sequences.
  // See Record* above + harness for full citations (per-element Xenon PPE != host FPCR).
  std::atomic<uint64_t> ps_mixed_element_edge_hits_{0};
  std::atomic<uint64_t> ps_rounding_edge_cases_{0};

  // R1 AUTHOR (validator-in-chief) + CAPTAIN RE-TASK: additional uncovered edge counters identified
  // in original 55-tool paired-singles research report (ps_subx/ps_sel usage in anim/conditional,
  // GQR quant fidelity for typical VBO data, deeper full-stack TLB shootdown + barrier +
  // psq_l/psq_st + 128B/pairing + lockfree audio/physics combos not exercised before).
  // New sequences in harness exercise + surface them. Citations throughout this file + a64_backend.cc.
  std::atomic<uint64_t> ps_sub_sel_arith_{0};
  std::atomic<uint64_t> ps_gqr_vbo_fidelity_cases_{0};
  std::atomic<uint64_t> ps_fullstack_tlb_barrier_psq_{0};

  // CAPTAIN RE-TASK (this cycle): NEW dedicated atomics for psq_l/psq_st roundtrips + GQR.
  // These back the Record* methods added above and will be exercised by enriched
  // RunPairedSingleAccuracyHarness sequences (psq_l load roundtrips, psq_st stores, GQR cases,
  // 128B reservation invalidation + pairing violations per R1 ps_* report).
  // Leverages psq_st skeleton (treated as normal store) + GQR landing.
  // Citations: R1 55-tool report (psq_l/psq_st quantized paths, GQR 0-7 configs, explicit
  // 128B psq_st reservation interaction warnings that drove cross-thread pairing errata work).
  std::atomic<uint64_t> psq_l_roundtrip_count_{0};
  std::atomic<uint64_t> psq_st_roundtrip_count_{0};
  std::atomic<uint64_t> psq_gqr_config_cases_{0};

  // CAPTAIN RE-TASK (psq_st GQR): new dedicated atomic counters for psq_st-specific
  // validation sequences (exercised by first GQR quantization calls inside the psq_st
  // store skeletons + harness). psq_st_executed / reservation_invalidation / gqr_cases.
  // Full 128B + pairing coverage on quantized stores (via skeleton f.Store).
  // Citations: ppc_emit_memory.cc psq_st (1172+), a64_seq_memory 128B, R1 ps report,
  // GQR helpers (ppc_context.h:593), this task harness integration.
  std::atomic<uint64_t> psq_st_executed_{0};
  std::atomic<uint64_t> psq_st_reservation_invalidation_{0};
  std::atomic<uint64_t> psq_st_gqr_cases_{0};

  // CAPTAIN RE-TASK (psq_l load-side skeleton): symmetric counters for psq_l + res/pairing
  // harness sequences (lwarx A; psq_l B cross; correct EA for 128B tracking on loads).
  // Citations: pairing enforcement (XenonReserves... last_reserving_thread_id/granule,
  // Clear probe a64_seq_memory), psq_l skeleton (ppc_emit_memory.cc placeholder using
  // CalculateEA+Load), psq_st store skeleton, R1 ps report, 128B complete.

  // CAPTAIN RE-TASK (psq_l load skeleton + symmetric harness pairing/res seqs):
  // psq_l_executed + psq_l_reservation_invalidation_tests (for lwarx+psq_l cross-granule
  // EA/res tracking cases in harness; load-side counterpart to psq_st_*).
  // Citations: pairing (a64_seq_memory Clear + Xenon* + last_*), psq_l skeleton (ppc_emit_memory.cc),
  // psq_st validated store, R1 ps report, 128B coverage.
  std::atomic<uint64_t> psq_l_executed_{0};
  std::atomic<uint64_t> psq_l_reservation_invalidation_tests_{0};

  // CAPTAIN quick-win (Xenon barriers research): per-barrier-type emission counters.
  // LIGHT_SYNC (lwsync) is dominant in real titles (audio/physics/lock-free); these stats
  // (only under a64_accuracy_debug) let us observe emission frequency + validate lowering
  // choices (DMB ISH vs lighter ISHLD/ISHST opt-in). See RecordBarrier* + MEMORY_BARRIER.
  // Citations: barriers research report (lwsync usage + weakest-sufficient AArch64 quality on Adreno).
  std::atomic<uint64_t> barriers_light_sync_{0};
  std::atomic<uint64_t> barriers_full_sync_{0};
  std::atomic<uint64_t> barriers_io_{0};
  std::atomic<uint64_t> barriers_instruction_{0};

  // Extensible generics (for future metrics: interrupts, tlb_misses, etc.)
  // TLB debug integration (R2 owner re-task): tlb_ops_ignored now exercised from ps_* harness TLB-aware seqs.
  std::atomic<uint64_t> generic_[32] = {};
};

}  // namespace perf
}  // namespace ax360e

// Global instance for easy access from anywhere (including sequences/backends).
// Access via ax360e::perf::g_cpu_accuracy (defined in one TU that includes this).
extern ax360e::perf::CpuAccuracyTracker g_cpu_accuracy;

#endif  // AX360E_PERF_LOG_H
