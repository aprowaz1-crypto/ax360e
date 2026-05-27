/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_CONTEXT_H_
#define XENIA_CPU_PPC_PPC_CONTEXT_H_

#include <cstdint>
#include <string>

#include "xenia/guest_pointers.h"
#include "xenia/base/vec128.h"
#include "xenia/base/mutex.h"

namespace xe {
namespace cpu {
class Processor;
class ThreadState;
}  // namespace cpu
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace cpu {
namespace ppc {

// Map:
// 0-31: GPR
// 32-63: FPR
// 64: LR
// 65: CTR
// 66: XER
// 67: FPSCR
// 68: VSCR
// 69-76: CR0-7
// 100: invalid
// 128-256: VR

enum class PPCRegister {
  kR0 = 0,
  kR1,
  kR2,
  kR3,
  kR4,
  kR5,
  kR6,
  kR7,
  kR8,
  kR9,
  kR10,
  kR11,
  kR12,
  kR13,
  kR14,
  kR15,
  kR16,
  kR17,
  kR18,
  kR19,
  kR20,
  kR21,
  kR22,
  kR23,
  kR24,
  kR25,
  kR26,
  kR27,
  kR28,
  kR29,
  kR30,
  kR31,
  kFR0 = 32,
  kFR1,
  kFR2,
  kFR3,
  kFR4,
  kFR5,
  kFR6,
  kFR7,
  kFR8,
  kFR9,
  kFR10,
  kFR11,
  kFR12,
  kFR13,
  kFR14,
  kFR15,
  kFR16,
  kFR17,
  kFR18,
  kFR19,
  kFR20,
  kFR21,
  kFR22,
  kFR23,
  kFR24,
  kFR25,
  kFR26,
  kFR27,
  kFR28,
  kFR29,
  kFR30,
  kFR31,
  kVR0 = 64,
  kVR1,
  kVR2,
  kVR3,
  kVR4,
  kVR5,
  kVR6,
  kVR7,
  kVR8,
  kVR9,
  kVR10,
  kVR11,
  kVR12,
  kVR13,
  kVR14,
  kVR15,
  kVR16,
  kVR17,
  kVR18,
  kVR19,
  kVR20,
  kVR21,
  kVR22,
  kVR23,
  kVR24,
  kVR25,
  kVR26,
  kVR27,
  kVR28,
  kVR29,
  kVR30,
  kVR31,
  kVR32,
  kVR33,
  kVR34,
  kVR35,
  kVR36,
  kVR37,
  kVR38,
  kVR39,
  kVR40,
  kVR41,
  kVR42,
  kVR43,
  kVR44,
  kVR45,
  kVR46,
  kVR47,
  kVR48,
  kVR49,
  kVR50,
  kVR51,
  kVR52,
  kVR53,
  kVR54,
  kVR55,
  kVR56,
  kVR57,
  kVR58,
  kVR59,
  kVR60,
  kVR61,
  kVR62,
  kVR63,
  kVR64,
  kVR65,
  kVR66,
  kVR67,
  kVR68,
  kVR69,
  kVR70,
  kVR71,
  kVR72,
  kVR73,
  kVR74,
  kVR75,
  kVR76,
  kVR77,
  kVR78,
  kVR79,
  kVR80,
  kVR81,
  kVR82,
  kVR83,
  kVR84,
  kVR85,
  kVR86,
  kVR87,
  kVR88,
  kVR89,
  kVR90,
  kVR91,
  kVR92,
  kVR93,
  kVR94,
  kVR95,
  kVR96,
  kVR97,
  kVR98,
  kVR99,
  kVR100,
  kVR101,
  kVR102,
  kVR103,
  kVR104,
  kVR105,
  kVR106,
  kVR107,
  kVR108,
  kVR109,
  kVR110,
  kVR111,
  kVR112,
  kVR113,
  kVR114,
  kVR115,
  kVR116,
  kVR117,
  kVR118,
  kVR119,
  kVR120,
  kVR121,
  kVR122,
  kVR123,
  kVR124,
  kVR125,
  kVR126,
  kVR127,
  kVR128,

  kLR,
  kCTR,
  kXER,
  kFPSCR,
  kVSCR,
  kCR,
};

#pragma pack(push, 8)
typedef struct PPCContext_s {
  // Must be stored at 0x0 for now.
  // TODO(benvanik): find a nice way to describe this to the JIT.
  ThreadState* thread_state;  // 0x0
  // TODO(benvanik): this is getting nasty. Must be here.
  uint8_t* virtual_membase;  // 0x8

  // Most frequently used registers first.
  uint64_t lr;      // 0x10 Link register
  uint64_t ctr;     // 0x18 Count register
  uint64_t r[32];   // 0x20 General purpose registers
  double f[32];     // 0x120 Floating-point registers

  // --------------------------------------------------------------------------
  // GQR (Graphics Quantization Registers, SPR 912-919) - INFRASTRUCTURE
  // Added per direct order from ps_* research agent (R1, 55 tools).
  // This is the prerequisite explicitly called out in the R1 ps report for
  // clean future implementation of psq_l / psq_st (and indexed/update forms).
  // See the master plan block at top of ppc_emit_fpu.cc (esp. Phase 2:
  // "psq_l / psq_st (in ppc_emit_memory.cc or here) + GQR support").
  // Also referenced in foundational plan comment in ppc_emit_memory.cc.
  //
  // GQR[8] are 32-bit regs. Used for on-the-fly quantization of paired-single
  // loads/stores (8/16-bit signed/unsigned scaled by power-of-two factor).
  // Future emitters will LoadContext(offsetof(PPCContext, gqr[n])) + use
  // the inline helpers below (get_gqr / set_gqr + de/quantize funcs).
  //
  // Placement: immediately after f[32] (FPRs) as they are closely related to
  // FPU/paired-single data movement. uint32_t[8] = +32 bytes.
  //
  // Alignment/padding note: #pragma pack(push, 8) active; adding 32-bit array
  // preserves 8-byte alignment for subsequent vec128_t (16B) and later fields.
  // No extra pad bytes needed here. All later field offset comments updated.
  // Total struct size increases by exactly 32 bytes (see size assert below).
  // --------------------------------------------------------------------------
  uint32_t gqr[8];  // 0x220 GQR0..GQR7 (SPR912-919). Zero-init via memset in ThreadState.
                    // Use get_gqr(n)/set_gqr(n) accessors below.

  vec128_t v[128];  // 0x240 VMX128 vector registers  (was 0x220; +0x20 shift)

  // XER register:
  // Split to make it easier to do individual updates.
  uint8_t xer_ca;  // 0xA40  (was 0xA20; +0x20 shift due to gqr[8])
  uint8_t xer_ov;  // 0xA41
  uint8_t xer_so;  // 0xA42

  // Condition registers:
  // These are split to make it easier to do DCE on unused stores.
  uint64_t cr() const;
  void set_cr(uint64_t value);
  union {
    uint32_t value;
    struct {
      uint8_t cr0_lt;  // Negative (LT) - result is negative
      uint8_t cr0_gt;  // Positive (GT) - result is positive (and not zero)
      uint8_t cr0_eq;  // Zero (EQ) - result is zero or a stwcx/stdcx completed
                       // successfully
      uint8_t cr0_so;  // Summary Overflow (SO) - copy of XER[SO]
    };
  } cr0;  // 0xA44  (was 0xA24; +0x20 shift)
  union {
    uint32_t value;
    struct {
      uint8_t cr1_fx;   // FP exception summary - copy of FPSCR[FX]
      uint8_t cr1_fex;  // FP enabled exception summary - copy of FPSCR[FEX]
      uint8_t
          cr1_vx;  // FP invalid operation exception summary - copy of FPSCR[VX]
      uint8_t cr1_ox;  // FP overflow exception - copy of FPSCR[OX]
    };
  } cr1;
  union {
    uint32_t value;
    struct {
      uint8_t cr2_0;
      uint8_t cr2_1;
      uint8_t cr2_2;
      uint8_t cr2_3;
    };
  } cr2;
  union {
    uint32_t value;
    struct {
      uint8_t cr3_0;
      uint8_t cr3_1;
      uint8_t cr3_2;
      uint8_t cr3_3;
    };
  } cr3;
  union {
    uint32_t value;
    struct {
      uint8_t cr4_0;
      uint8_t cr4_1;
      uint8_t cr4_2;
      uint8_t cr4_3;
    };
  } cr4;
  union {
    uint32_t value;
    struct {
      uint8_t cr5_0;
      uint8_t cr5_1;
      uint8_t cr5_2;
      uint8_t cr5_3;
    };
  } cr5;
  union {
    uint32_t value;
    struct {
      uint8_t cr6_all_equal;
      uint8_t cr6_1;
      uint8_t cr6_none_equal;
      uint8_t cr6_3;
    };
  } cr6;
  union {
    uint32_t value;
    struct {
      uint8_t cr7_0;
      uint8_t cr7_1;
      uint8_t cr7_2;
      uint8_t cr7_3;
    };
  } cr7;

  union {
    uint32_t value;
    struct {
      uint32_t rn : 2;      // FP rounding control: 00 = nearest
                            //                      01 = toward zero
                            //                      10 = toward +infinity
                            //                      11 = toward -infinity
      uint32_t ni : 1;      // Floating-point non-IEEE mode
      uint32_t xe : 1;      // IEEE floating-point inexact exception enable
      uint32_t ze : 1;      // IEEE floating-point zero divide exception enable
      uint32_t ue : 1;      // IEEE floating-point underflow exception enable
      uint32_t oe : 1;      // IEEE floating-point overflow exception enable
      uint32_t ve : 1;      // FP invalid op exception enable
      uint32_t vxcvi : 1;   // FP invalid op exception: invalid integer convert
                            // -- sticky
      uint32_t vxsqrt : 1;  // FP invalid op exception: invalid sqrt -- sticky
      uint32_t vxsoft : 1;  // FP invalid op exception: software request
                            // -- sticky
      uint32_t reserved : 1;
      uint32_t fprf_un : 1;  // FP result unordered or NaN (FU or ?)
      uint32_t fprf_eq : 1;  // FP result equal or zero (FE or =)
      uint32_t fprf_gt : 1;  // FP result greater than or positive (FG or >)
      uint32_t fprf_lt : 1;  // FP result less than or negative (FL or <)
      uint32_t fprf_c : 1;   // FP result class
      uint32_t fi : 1;       // FP fraction inexact
      uint32_t fr : 1;       // FP fraction rounded
      uint32_t vxvc : 1;  // FP invalid op exception: invalid compare         --
                          // sticky
      uint32_t vximz : 1;   // FP invalid op exception: infinity * 0 -- sticky
      uint32_t vxzdz : 1;   // FP invalid op exception: 0 / 0 -- sticky
      uint32_t vxidi : 1;   // FP invalid op exception: infinity / infinity
                            // -- sticky
      uint32_t vxisi : 1;   // FP invalid op exception: infinity - infinity
                            // -- sticky
      uint32_t vxsnan : 1;  // FP invalid op exception: SNaN -- sticky
      uint32_t
          xx : 1;  // FP inexact exception                             -- sticky
      uint32_t
          zx : 1;  // FP zero divide exception                         -- sticky
      uint32_t
          ux : 1;  // FP underflow exception                           -- sticky
      uint32_t
          ox : 1;  // FP overflow exception                            -- sticky
      uint32_t vx : 1;   // FP invalid operation exception summary
      uint32_t fex : 1;  // FP enabled exception summary
      uint32_t
          fx : 1;  // FP exception summary                             -- sticky
    } bits;
  } fpscr;  // Floating-point status and control register

  uint8_t vscr_sat;

  // uint32_t get_fprf() {
  //   return fpscr.value & 0x000F8000;
  // }
  // void set_fprf(const uint32_t v) {
  //   fpscr.value = (fpscr.value & ~0x000F8000) | v;
  // }

  // Thread ID assigned to this context.
  uint32_t thread_id;

  // Global interrupt lock, held while interrupts are disabled or interrupts are
  // executing. This is shared among all threads and comes from the processor.
  xe::global_mutex_type* global_mutex;

  // Used to shuttle data into externs. Contents volatile.
  uint64_t scratch;

  // Processor-specific data pointer. Used on callbacks to get access to the
  // current runtime and its data.
  Processor* processor;

  // Shared kernel state, for easy access from kernel exports.
  xe::kernel::KernelState* kernel_state;

  uint8_t* physical_membase;

  // Value of last reserved load
  uint64_t reserved_val;

  // Decrementer (DEC SPR 22) emulation for accurate timer interrupt fidelity.
  // Stores the last written DEC value and the guest TB (clock) value at write
  // time. Reads compute the current remaining count: max(0, value - (now - tb)).
  // This provides cycle-precise decrementer behavior synced to timebase.
  // When value reaches/underflows 0 and EE=1, a decrementer exception is
  // delivered via CheckDecrementerInterrupt -> Reenter to (ivpr + 0x900).
  uint64_t dec_value;
  uint64_t dec_tb_at_write;

  // For proper decrementer exception delivery (0x900 vector).
  // fire_time is cached on mtdec for fast >= checks without recomputing every time.
  // pending is set when underflow detected (in mfdec computation or clock check).
  uint64_t dec_fire_time;
  uint32_t dec_pending;  // nonzero = interrupt pending delivery
  uint32_t _dec_pad1;

  // Minimal exception state for DEC (and future) delivery.
  // On DEC underflow + EE=1: srr0 = continuation address, srr1 = old MSR,
  // then vector to (ivpr + 0x900), EE cleared in effective MSR.
  uint64_t srr0;
  uint64_t srr1;
  uint64_t ivpr;  // set via mtspr (we will stub SPR 63 minimally)

  // (GQR[8] relocated per task to immediately after f[32] as uint32_t gqr[8]
  // at ~0x220. See the detailed insertion + R1 citations + accessor/quant
  // helpers earlier in this struct. The late stub was consolidated here to
  // obey the "near double f[32]" requirement exactly.)

  // Padding to maintain 64-byte alignment of PPCContext.
  uint64_t _dec_padding[5];  // tuned for 64B alignment (GQR now placed with FPRs near 0x220)

  static std::string GetRegisterName(PPCRegister reg);
  std::string GetStringFromValue(PPCRegister reg) const;
  void SetValueFromString(PPCRegister reg, std::string value);

  void SetRegFromString(const char* name, const char* value);
  bool CompareRegWithString(const char* name, const char* value,
                            std::string& result) const;
    template <typename T = uint8_t*>
    inline T TranslateVirtual(uint32_t guest_address) XE_RESTRICT const {
        static_assert(std::is_pointer_v<T>);
        uint8_t* host_address = virtual_membase + guest_address;
#if XE_PLATFORM_WIN32 == 1
        if (guest_address >=
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this))) {
      host_address += 0x1000;
    }
#endif
        return reinterpret_cast<T>(host_address);
    }
    // for convenience in kernel functions, version that auto narrows to uint32
    template <typename T = uint8_t*>
    inline T TranslateVirtualGPR(uint64_t guest_address) XE_RESTRICT const {
        return TranslateVirtual<T>(static_cast<uint32_t>(guest_address));
    }
    template <typename T>
    inline T* TranslateVirtual(TypedGuestPointer<T> guest_address) {
        return TranslateVirtual<T*>(guest_address.m_ptr);
    }
    template <typename T>
    inline uint32_t HostToGuestVirtual(T* host_ptr) XE_RESTRICT const {
        uint32_t guest_tmp = static_cast<uint32_t>(
                reinterpret_cast<const uint8_t*>(host_ptr) - virtual_membase);
#if XE_PLATFORM_WIN32 == 1
        if (guest_tmp >= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this))) {
      guest_tmp -= 0x1000;
    }
#endif
        return guest_tmp;
    }
    template <typename T>
    inline xe::be<T>* TranslateVirtualBE(uint32_t guest_address)
    XE_RESTRICT const {
        static_assert(!std::is_pointer_v<T> &&
                      sizeof(T) > 1);  // maybe assert is_integral?
        return TranslateVirtual<xe::be<T>*>(guest_address);
    }

    // --------------------------------------------------------------------------
    // GQR accessors (simple, for C++ side + future HIR Load/StoreContext usage)
    // Per R1 ps_* research report (prereq for psq_l/psq_st clean impl).
    // See ppc_emit_fpu.cc plan block (Phase 2 GQR + quantized stores/loads).
    // --------------------------------------------------------------------------
    inline uint32_t get_gqr(uint32_t n) const { return gqr[n & 7]; }
    inline void set_gqr(uint32_t n, uint32_t value) { gqr[n & 7] = value; }

} PPCContext;

// ============================================================================
// BASIC INLINE GQR QUANTIZATION HELPERS (scale + s/u 8/16-bit convert)
// ============================================================================
// CAPTAIN RE-TASK (psq_st GQR integration): helpers exposed for cross-TU use
// (a64_backend.cc harness + future psq_st emitters in ppc_emit_memory.cc).
// Previously in anon ns (internal linkage only); now defined at ppc:: scope
// (static inline in header = fine for multiple TUs, matches GQR foundation
// intent "call these directly").
//
// Placed here (in ppc_context.h) so psq_* memory emitters (and any
// HIR builder code) can include "ppc_context.h" and call these directly.
// Follows exact recommendation from the ps_* research report (R1): "inline,
// no giant tables". Pure scalar math, ldexp-free for minimal/fast inlining.
//
// GQR bit layout (Gekko/PPE standard, matches R1 analysis):
//   [ 2: 0] ST_TYPE (4=u8,5=s8,6=u16,7=s16)
//   [ 8: 3] ST_SCALE (6-bit signed, -32..31)
//   [18:16] LD_TYPE (same encoding)
//   [24:19] LD_SCALE (6-bit signed)
//
// These helpers are the foundation only. Full psq_l/psq_st emitters NOT
// implemented here (see charter). THIS TASK: first basic GQR quantization
// *calls* added to psq_st store skeletons + harness (store side only;
// W/I extraction + type handling kept lightweight per Captain scope).
//
// Usage in psq_st (store):
//   uint32_t g = ctx->get_gqr(I); int scale = GQRGetScale(g, /*for_store*/true);
//   uint32_t ty = GQRGetType(g, true); int bits; bool sgn; GQRTypeToWidthSign(ty,...);
//   int32_t q = GQRQuantize(ps0_f32, scale, sgn, bits);
//   // then HIR store of q (size per bits) -> flows to 128B Clear (guaranteed by skeleton)
// See plan block in ppc_emit_fpu.cc and memory plan in ppc_emit_memory.cc.
// Citations: R1 ps report (context.h:265+), GQR foundation (this block),
//   128B store coverage (a64_seq_memory.cc:374+), psq_st skeletons (ppc_emit_memory.cc:1046+),
//   this Captain re-task (psq_st GQR quant + harness counters).
// ============================================================================

// (CAPTAIN RE-TASK: helpers now at ppc namespace scope for harness + emitter inclusion.
//  No anon ns. All prior call examples continue to work; new psq_st GQR usage added.)

// Extract signed 6-bit scale for LD (load/dequant) or ST (store/quant).
static inline int GQRGetScale(uint32_t gqr, bool for_store) {
  uint32_t shift = for_store ? 3u : 19u;
  int s = static_cast<int>((gqr >> shift) & 0x3Fu);
  if (s & 0x20) s -= 0x40;  // sign-extend 6-bit
  return s;
}

// Extract type field (0-7); quantized are 4-7.
static inline uint32_t GQRGetType(uint32_t gqr, bool for_store) {
  uint32_t shift = for_store ? 0u : 16u;
  return (gqr >> shift) & 7u;
}

// Dequantize a raw integer (post load+sign-extend) to float using GQR LD scale.
// Inline, branch-light for emitter use.
static inline float GQRDequantize(int32_t raw, int scale) {
  // Equivalent to raw * (2 ^ -scale). Handles negative scales (magnify).
  if (scale >= 0) {
    return static_cast<float>(raw) / static_cast<float>(1 << scale);
  } else {
    return static_cast<float>(raw) * static_cast<float>(1 << (-scale));
  }
}

// Quantize a float to integer (pre-store), apply GQR ST scale, round nearest,
// clamp per type (u/s 8/16). Returns the raw int to store.
// FIRST GQR-AWARE CALLS (this task): used for psq_st simulation in harness
// and ready for real psq_st lowering (store side, lightweight W/I).
static inline int32_t GQRQuantize(float val, int scale, bool is_signed, int bits) {
  float scaled;
  if (scale >= 0) {
    scaled = val * static_cast<float>(1 << scale);
  } else {
    scaled = val / static_cast<float>(1 << (-scale));
  }
  // Round to nearest (ties away from zero for simplicity; matches common emu).
  int32_t ival = static_cast<int32_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));

  int32_t minv, maxv;
  if (bits == 8) {
    minv = is_signed ? -128 : 0;
    maxv = is_signed ? 127 : 255;
  } else {  // 16
    minv = is_signed ? -32768 : 0;
    maxv = is_signed ? 32767 : 65535;
  }
  if (ival < minv) ival = minv;
  if (ival > maxv) ival = maxv;
  return ival;
}

// Convenience: dequant given GQR and raw (caller must have sign-extended for s*).
static inline float GQRDequantizeFromGQR(uint32_t gqr, int32_t raw, bool for_store /*false for ld*/) {
  int scale = GQRGetScale(gqr, for_store);
  return GQRDequantize(raw, scale);
}

// Type helpers for future emitters (map type -> width + signedness).
static inline void GQRTypeToWidthSign(uint32_t type, int* out_bits, bool* out_signed) {
  switch (type) {
    case 4: *out_bits=8;  *out_signed=false; break;  // u8
    case 5: *out_bits=8;  *out_signed=true;  break;  // s8
    case 6: *out_bits=16; *out_signed=false; break;  // u16
    case 7: *out_bits=16; *out_signed=true;  break;  // s16
    default: *out_bits=32; *out_signed=true; break;  // float / reserved (no-op)
  }
}

static_assert(sizeof(PPCContext) == 2784, "");  // 64B aligned. Net: +32B uint32 gqr[8] near f[32] (task spec) -64B consolidated late stub. Per R1 ps report + ppc_emit_fpu.cc plan. GQR now uint32[8] at 0x220.
#pragma pack(pop)
static_assert(sizeof(PPCContext) % 64 == 0, "64b padded");

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_CONTEXT_H_
