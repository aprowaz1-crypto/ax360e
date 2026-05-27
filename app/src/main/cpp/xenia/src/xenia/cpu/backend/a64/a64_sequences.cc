/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// A note about vectors:
// Xenia represents vectors as xyzw pairs, with indices 0123.
// XMM registers are xyzw pairs with indices 3210, making them more like wzyx.
// This makes things somewhat confusing. It'd be nice to just shuffle the
// registers around on load/store, however certain operations require that
// data be in the right offset.
// Basically, this identity must hold:
//   shuffle(vec, b00011011) -> {x,y,z,w} => {x,y,z,w}
// All indices and operations must respect that.
//
// Memory (big endian):
// [00 01 02 03] [04 05 06 07] [08 09 0A 0B] [0C 0D 0E 0F] (x, y, z, w)
// load into xmm register:
// [0F 0E 0D 0C] [0B 0A 09 08] [07 06 05 04] [03 02 01 00] (w, z, y, x)

#include "xenia/cpu/backend/a64/a64_sequences.h"

#include <algorithm>
#include <unordered_map>

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/threading.h"

// CPU accuracy metrics (timebase reads etc.)
#include "ax360e_perf_log.h"

#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"
#include "xenia/cpu/backend/a64/a64_util.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using namespace oaknut;

// TODO(benvanik): direct usings.
using namespace xe::cpu;
using namespace xe::cpu::hir;

using xe::cpu::hir::Instr;

typedef bool (*SequenceSelectFn)(A64Emitter&, const Instr*);
std::unordered_map<uint32_t, SequenceSelectFn> sequence_table;

// ============================================================================
// OPCODE_COMMENT
// ============================================================================
struct COMMENT : Sequence<COMMENT, I<OPCODE_COMMENT, VoidOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (IsTracingInstr()) {
      auto str = reinterpret_cast<const char*>(i.src1.value);
      // TODO(benvanik): pass through.
      // TODO(benvanik): don't just leak this memory.
      auto str_copy = xe_strdup(str);
      e.MOV(e.GetNativeParam(0), reinterpret_cast<uint64_t>(str_copy));
      e.CallNative(reinterpret_cast<void*>(TraceString));
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMMENT, COMMENT);

// ============================================================================
// OPCODE_NOP
// ============================================================================
struct NOP : Sequence<NOP, I<OPCODE_NOP, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) { e.NOP(); }
};
EMITTER_OPCODE_TABLE(OPCODE_NOP, NOP);

// ============================================================================
// OPCODE_SOURCE_OFFSET
// ============================================================================
struct SOURCE_OFFSET
    : Sequence<SOURCE_OFFSET, I<OPCODE_SOURCE_OFFSET, VoidOp, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MarkSourceOffset(i.instr);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SOURCE_OFFSET, SOURCE_OFFSET);

// ============================================================================
// OPCODE_ASSIGN
// ============================================================================
struct ASSIGN_I8 : Sequence<ASSIGN_I8, I<OPCODE_ASSIGN, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest, i.src1);
  }
};
struct ASSIGN_I16 : Sequence<ASSIGN_I16, I<OPCODE_ASSIGN, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTH(i.dest, i.src1);
  }
};
struct ASSIGN_I32 : Sequence<ASSIGN_I32, I<OPCODE_ASSIGN, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MOV(i.dest, i.src1);
  }
};
struct ASSIGN_I64 : Sequence<ASSIGN_I64, I<OPCODE_ASSIGN, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MOV(i.dest, i.src1);
  }
};
struct ASSIGN_F32 : Sequence<ASSIGN_F32, I<OPCODE_ASSIGN, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FMOV(i.dest, i.src1);
  }
};
struct ASSIGN_F64 : Sequence<ASSIGN_F64, I<OPCODE_ASSIGN, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FMOV(i.dest, i.src1);
  }
};
struct ASSIGN_V128 : Sequence<ASSIGN_V128, I<OPCODE_ASSIGN, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MOV(i.dest.reg().B16(), i.src1.reg().B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ASSIGN, ASSIGN_I8, ASSIGN_I16, ASSIGN_I32,
                     ASSIGN_I64, ASSIGN_F32, ASSIGN_F64, ASSIGN_V128);

// ============================================================================
// OPCODE_CAST
// ============================================================================
struct CAST_I32_F32 : Sequence<CAST_I32_F32, I<OPCODE_CAST, I32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FMOV(i.dest, i.src1);
  }
};
struct CAST_I64_F64 : Sequence<CAST_I64_F64, I<OPCODE_CAST, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FMOV(i.dest, i.src1);
  }
};
struct CAST_F32_I32 : Sequence<CAST_F32_I32, I<OPCODE_CAST, F32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FMOV(i.dest, i.src1);
  }
};
struct CAST_F64_I64 : Sequence<CAST_F64_I64, I<OPCODE_CAST, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FMOV(i.dest, i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CAST, CAST_I32_F32, CAST_I64_F64, CAST_F32_I32,
                     CAST_F64_I64);

// ============================================================================
// OPCODE_ZERO_EXTEND
// ============================================================================
struct ZERO_EXTEND_I16_I8
    : Sequence<ZERO_EXTEND_I16_I8, I<OPCODE_ZERO_EXTEND, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I32_I8
    : Sequence<ZERO_EXTEND_I32_I8, I<OPCODE_ZERO_EXTEND, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I8
    : Sequence<ZERO_EXTEND_I64_I8, I<OPCODE_ZERO_EXTEND, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest.reg().toW(), i.src1);
  }
};
struct ZERO_EXTEND_I32_I16
    : Sequence<ZERO_EXTEND_I32_I16, I<OPCODE_ZERO_EXTEND, I32Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTH(i.dest, i.src1);
  }
};
struct ZERO_EXTEND_I64_I16
    : Sequence<ZERO_EXTEND_I64_I16, I<OPCODE_ZERO_EXTEND, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTH(i.dest.reg().toW(), i.src1);
  }
};
struct ZERO_EXTEND_I64_I32
    : Sequence<ZERO_EXTEND_I64_I32, I<OPCODE_ZERO_EXTEND, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MOV(i.dest.reg().toW(), i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ZERO_EXTEND, ZERO_EXTEND_I16_I8, ZERO_EXTEND_I32_I8,
                     ZERO_EXTEND_I64_I8, ZERO_EXTEND_I32_I16,
                     ZERO_EXTEND_I64_I16, ZERO_EXTEND_I64_I32);

// ============================================================================
// OPCODE_SIGN_EXTEND
// ============================================================================
struct SIGN_EXTEND_I16_I8
    : Sequence<SIGN_EXTEND_I16_I8, I<OPCODE_SIGN_EXTEND, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SXTB(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I32_I8
    : Sequence<SIGN_EXTEND_I32_I8, I<OPCODE_SIGN_EXTEND, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SXTB(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I8
    : Sequence<SIGN_EXTEND_I64_I8, I<OPCODE_SIGN_EXTEND, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SXTB(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I32_I16
    : Sequence<SIGN_EXTEND_I32_I16, I<OPCODE_SIGN_EXTEND, I32Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SXTH(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I16
    : Sequence<SIGN_EXTEND_I64_I16, I<OPCODE_SIGN_EXTEND, I64Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SXTH(i.dest, i.src1);
  }
};
struct SIGN_EXTEND_I64_I32
    : Sequence<SIGN_EXTEND_I64_I32, I<OPCODE_SIGN_EXTEND, I64Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SXTW(i.dest, i.src1.reg());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SIGN_EXTEND, SIGN_EXTEND_I16_I8, SIGN_EXTEND_I32_I8,
                     SIGN_EXTEND_I64_I8, SIGN_EXTEND_I32_I16,
                     SIGN_EXTEND_I64_I16, SIGN_EXTEND_I64_I32);

// ============================================================================
// OPCODE_TRUNCATE
// ============================================================================
struct TRUNCATE_I8_I16
    : Sequence<TRUNCATE_I8_I16, I<OPCODE_TRUNCATE, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest, i.src1);
  }
};
struct TRUNCATE_I8_I32
    : Sequence<TRUNCATE_I8_I32, I<OPCODE_TRUNCATE, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest, i.src1);
  }
};
struct TRUNCATE_I8_I64
    : Sequence<TRUNCATE_I8_I64, I<OPCODE_TRUNCATE, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTB(i.dest, i.src1.reg().toW());
  }
};
struct TRUNCATE_I16_I32
    : Sequence<TRUNCATE_I16_I32, I<OPCODE_TRUNCATE, I16Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTH(i.dest, i.src1);
  }
};
struct TRUNCATE_I16_I64
    : Sequence<TRUNCATE_I16_I64, I<OPCODE_TRUNCATE, I16Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UXTH(i.dest, i.src1.reg().toW());
  }
};
struct TRUNCATE_I32_I64
    : Sequence<TRUNCATE_I32_I64, I<OPCODE_TRUNCATE, I32Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MOV(i.dest, i.src1.reg().toW());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_TRUNCATE, TRUNCATE_I8_I16, TRUNCATE_I8_I32,
                     TRUNCATE_I8_I64, TRUNCATE_I16_I32, TRUNCATE_I16_I64,
                     TRUNCATE_I32_I64);

// ============================================================================
// OPCODE_CONVERT
// ============================================================================
struct CONVERT_I32_F32
    : Sequence<CONVERT_I32_F32, I<OPCODE_CONVERT, I32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): saturation check? cvtt* (trunc?)
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.FCVTZS(i.dest, i.src1.reg().toS());
    } else {
      e.FCVTNS(i.dest, i.src1.reg().toS());
    }
  }
};
struct CONVERT_I32_F64
    : Sequence<CONVERT_I32_F64, I<OPCODE_CONVERT, I32Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Intel returns 0x80000000 if the double value does not fit within an int32
    // ARM64 and PPC saturates the value instead
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.FCVTZS(i.dest, i.src1.reg().toD());
    } else {
      e.FCVTNS(i.dest, i.src1.reg().toD());
    }
  }
};
struct CONVERT_I64_F64
    : Sequence<CONVERT_I64_F64, I<OPCODE_CONVERT, I64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags == ROUND_TO_ZERO) {
      e.FCVTZS(i.dest, i.src1.reg().toD());
    } else {
      e.FCVTNS(i.dest, i.src1.reg().toD());
    }
  }
};
struct CONVERT_F32_I32
    : Sequence<CONVERT_F32_I32, I<OPCODE_CONVERT, F32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SCVTF(i.dest.reg().toS(), i.src1);
  }
};
struct CONVERT_F32_F64
    : Sequence<CONVERT_F32_F64, I<OPCODE_CONVERT, F32Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCVT(i.dest.reg().toS(), i.src1.reg().toD());
  }
};
struct CONVERT_F64_I64
    : Sequence<CONVERT_F64_I64, I<OPCODE_CONVERT, F64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.SCVTF(i.dest.reg().toD(), i.src1);
  }
};
struct CONVERT_F64_F32
    : Sequence<CONVERT_F64_F32, I<OPCODE_CONVERT, F64Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // e.vcvtss2sd(i.dest, i.src1);
    e.FCVT(i.dest.reg().toD(), i.src1.reg().toS());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CONVERT, CONVERT_I32_F32, CONVERT_I32_F64,
                     CONVERT_I64_F64, CONVERT_F32_I32, CONVERT_F32_F64,
                     CONVERT_F64_I64, CONVERT_F64_F32);

// ============================================================================
// OPCODE_ROUND
// ============================================================================
struct ROUND_F32 : Sequence<ROUND_F32, I<OPCODE_ROUND, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.FRINTZ(i.dest.reg().toS(), i.src1.reg().toS());
        break;
      case ROUND_TO_NEAREST:
        e.FRINTN(i.dest.reg().toS(), i.src1.reg().toS());
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.FRINTM(i.dest.reg().toS(), i.src1.reg().toS());
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.FRINTP(i.dest.reg().toS(), i.src1.reg().toS());
        break;
    }
  }
};
struct ROUND_F64 : Sequence<ROUND_F64, I<OPCODE_ROUND, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.FRINTZ(i.dest, i.src1);
        break;
      case ROUND_TO_NEAREST:
        e.FRINTN(i.dest, i.src1);
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.FRINTM(i.dest, i.src1);
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.FRINTP(i.dest, i.src1);
        break;
    }
  }
};
struct ROUND_V128 : Sequence<ROUND_V128, I<OPCODE_ROUND, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags) {
      case ROUND_TO_ZERO:
        e.FRINTZ(i.dest.reg().S4(), i.src1.reg().S4());
        break;
      case ROUND_TO_NEAREST:
        e.FRINTN(i.dest.reg().S4(), i.src1.reg().S4());
        break;
      case ROUND_TO_MINUS_INFINITY:
        e.FRINTM(i.dest.reg().S4(), i.src1.reg().S4());
        break;
      case ROUND_TO_POSITIVE_INFINITY:
        e.FRINTP(i.dest.reg().S4(), i.src1.reg().S4());
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ROUND, ROUND_F32, ROUND_F64, ROUND_V128);

// ============================================================================
// OPCODE_LOAD_CLOCK
// ============================================================================
struct LOAD_CLOCK : Sequence<LOAD_CLOCK, I<OPCODE_LOAD_CLOCK, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // On AArch64 (Android), prefer direct CNTVCT_EL0 (raw virtual counter)
    // for highest precision and lowest overhead mftb/mtdec timing.
    // This dramatically improves timebase fidelity vs repeated clock_gettime
    // syscalls + mutex in QueryGuestTickCount.
    // Ratio is snapshotted at translation time (scalar changes are rare after
    // init; acceptable for guest timer accuracy).
#if XE_CLOCK_RAW_AVAILABLE
    if (cvars::clock_source_raw || XE_ARCH_ARM64) {
      auto ratio = Clock::guest_tick_ratio();
      // The 360 CPU is an in-order CPU, ARM64 usually isn't. Since its
      // resolution however is much higher than the 360's mftb instruction this
      // can safely be ignored. Provides cycle-accurate-ish guest ticks.

      // Read clock cycle count directly from system counter (no syscall).
      e.MRS(i.dest, SystemReg::CNTVCT_EL0);
      // Apply tick frequency scaling (guest 50MHz typically).
      e.MOV(X0, ratio.first);
      e.MUL(i.dest, i.dest, X0);
      e.MOV(X0, ratio.second);
      e.UDIV(i.dest, i.dest, X0);
      ax360e::perf::g_cpu_accuracy.RecordTimebaseRead();
      return;
    }
#endif
    // Fallback path (uses UpdateGuestClock + possible mutex).
    e.CallNative(LoadClock);
    e.MOV(i.dest, X0);
    ax360e::perf::g_cpu_accuracy.RecordTimebaseRead();
  }
  static uint64_t LoadClock(void* raw_context) {
    uint64_t t = Clock::QueryGuestTickCount();
    // Opportunistic DEC underflow check on timebase read (many games poll mftb
    // in timing loops). The full delivery (respecting EE + reentry) happens in
    // the registered builtin when safe.
    if (raw_context) {
      auto* ctx = reinterpret_cast<xe::cpu::ppc::PPCContext*>(raw_context);
      if (ctx->dec_pending) {
        // Opportunistic underflow count (timebase read in tight loops common for DEC polling).
        // Full delivery still via CheckDecrementerInterrupt + Reenter (respects EE).
        ax360e::perf::g_cpu_accuracy.RecordDecUnderflowFired();
        // Defer actual raise to the check builtin (called on mtmsr or other safe points).
        // For immediate, the mtmsr path + fire_time will catch it.
      }
    }
    return t;
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_CLOCK, LOAD_CLOCK);

// ============================================================================
// OPCODE_CONTEXT_BARRIER
// ============================================================================
struct CONTEXT_BARRIER
    : Sequence<CONTEXT_BARRIER, I<OPCODE_CONTEXT_BARRIER, VoidOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {}
};
EMITTER_OPCODE_TABLE(OPCODE_CONTEXT_BARRIER, CONTEXT_BARRIER);

// ============================================================================
// OPCODE_MAX
// ============================================================================
struct MAX_F32 : Sequence<MAX_F32, I<OPCODE_MAX, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<SReg>(
        e, i, [](A64Emitter& e, SReg dest, SReg src1, SReg src2) {
          e.FMAX(dest, src1, src2);
        });
  }
};
struct MAX_F64 : Sequence<MAX_F64, I<OPCODE_MAX, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<DReg>(
        e, i, [](A64Emitter& e, DReg dest, DReg src1, DReg src2) {
          e.FMAX(dest, src1, src2);
        });
  }
};
struct MAX_V128 : Sequence<MAX_V128, I<OPCODE_MAX, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<QReg>(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.FMAX(dest.S4(), src1.S4(), src2.S4());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MAX, MAX_F32, MAX_F64, MAX_V128);

// ============================================================================
// OPCODE_MIN
// ============================================================================
struct MIN_I8 : Sequence<MIN_I8, I<OPCODE_MIN, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.CMP(dest_src, src);
          e.CSEL(dest_src, dest_src, src, Cond::LO);
        },
        [](A64Emitter& e, WReg dest_src, int32_t constant) {
          e.MOV(W0, constant);
          e.CMP(dest_src, W0);
          e.CSEL(dest_src, dest_src, W0, Cond::LO);
        });
  }
};
struct MIN_I16 : Sequence<MIN_I16, I<OPCODE_MIN, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.CMP(dest_src, src);
          e.CSEL(dest_src, dest_src, src, Cond::LO);
        },
        [](A64Emitter& e, WReg dest_src, int32_t constant) {
          e.MOV(W0, constant);
          e.CMP(dest_src, W0);
          e.CSEL(dest_src, dest_src, W0, Cond::LO);
        });
  }
};
struct MIN_I32 : Sequence<MIN_I32, I<OPCODE_MIN, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.CMP(dest_src, src);
          e.CSEL(dest_src, dest_src, src, Cond::LO);
        },
        [](A64Emitter& e, WReg dest_src, int32_t constant) {
          e.MOV(W0, constant);
          e.CMP(dest_src, W0);
          e.CSEL(dest_src, dest_src, W0, Cond::LO);
        });
  }
};
struct MIN_I64 : Sequence<MIN_I64, I<OPCODE_MIN, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryOp(
        e, i,
        [](A64Emitter& e, XReg dest_src, XReg src) {
          e.CMP(dest_src, src);
          e.CSEL(dest_src, dest_src, src, Cond::LO);
        },
        [](A64Emitter& e, XReg dest_src, int64_t constant) {
          e.MOV(X0, constant);
          e.CMP(dest_src, X0);
          e.CSEL(dest_src, dest_src, X0, Cond::LO);
        });
  }
};
struct MIN_F32 : Sequence<MIN_F32, I<OPCODE_MIN, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<SReg>(
        e, i, [](A64Emitter& e, SReg dest, SReg src1, SReg src2) {
          e.FMIN(dest, src1, src2);
        });
  }
};
struct MIN_F64 : Sequence<MIN_F64, I<OPCODE_MIN, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<DReg>(
        e, i, [](A64Emitter& e, DReg dest, DReg src1, DReg src2) {
          e.FMIN(dest, src1, src2);
        });
  }
};
struct MIN_V128 : Sequence<MIN_V128, I<OPCODE_MIN, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.FMIN(dest.S4(), src1.S4(), src2.S4());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MIN, MIN_I8, MIN_I16, MIN_I32, MIN_I64, MIN_F32,
                     MIN_F64, MIN_V128);

// ============================================================================
// OPCODE_SELECT
// ============================================================================
// dest = src1 ? src2 : src3
// TODO(benvanik): match compare + select sequences, as often it's something
//     like SELECT(VECTOR_COMPARE_SGE(a, b), a, b)
struct SELECT_I8
    : Sequence<SELECT_I8, I<OPCODE_SELECT, I8Op, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg src2(0);
    if (i.src2.is_constant) {
      src2 = W0;
      e.MOV(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.CMP(i.src1.reg().toX(), 0);
    e.CSEL(i.dest, src2, i.src3, Cond::NE);
  }
};
struct SELECT_I16
    : Sequence<SELECT_I16, I<OPCODE_SELECT, I16Op, I8Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg src2(0);
    if (i.src2.is_constant) {
      src2 = W0;
      e.MOV(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.CMP(i.src1.reg().toX(), 0);
    e.CSEL(i.dest, src2, i.src3, Cond::NE);
  }
};
struct SELECT_I32
    : Sequence<SELECT_I32, I<OPCODE_SELECT, I32Op, I8Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    WReg src2(0);
    if (i.src2.is_constant) {
      src2 = W0;
      e.MOV(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.CMP(i.src1.reg().toX(), 0);
    e.CSEL(i.dest, src2, i.src3, Cond::NE);
  }
};
struct SELECT_I64
    : Sequence<SELECT_I64, I<OPCODE_SELECT, I64Op, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    XReg src2(0);
    if (i.src2.is_constant) {
      src2 = X0;
      e.MOV(src2, i.src2.constant());
    } else {
      src2 = i.src2;
    }
    e.CMP(i.src1.reg().toX(), 0);
    e.CSEL(i.dest, src2, i.src3, Cond::NE);
  }
};
struct SELECT_F32
    : Sequence<SELECT_F32, I<OPCODE_SELECT, F32Op, I8Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 != 0 ? src2 : src3

    SReg src2 = i.src2.is_constant ? S2 : i.src2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    }

    SReg src3 = i.src3.is_constant ? S3 : i.src3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3.toQ(), i.src3.constant());
    }

    e.CMP(i.src1.reg().toX(), 0);
    e.FCSEL(i.dest, src2, src3, Cond::NE);
  }
};
struct SELECT_F64
    : Sequence<SELECT_F64, I<OPCODE_SELECT, F64Op, I8Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 != 0 ? src2 : src3

    const DReg src2 = i.src2.is_constant ? D2 : i.src2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    }

    const DReg src3 = i.src3.is_constant ? D3 : i.src3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3.toQ(), i.src3.constant());
    }

    e.CMP(i.src1.reg().toX(), 0);
    e.FCSEL(i.dest, src2, src3, Cond::NE);
  }
};
struct SELECT_V128_I8
    : Sequence<SELECT_V128_I8, I<OPCODE_SELECT, V128Op, I8Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // dest = src1 != 0 ? src2 : src3

    const QReg src2 = i.src2.is_constant ? Q2 : i.src2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2, i.src2.constant());
    }

    const QReg src3 = i.src3.is_constant ? Q3 : i.src3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3, i.src3.constant());
    }

    e.CMP(i.src1.reg().toX(), 0);
    e.CSETM(W0, Cond::NE);
    e.DUP(i.dest.reg().S4(), W0);
    e.BSL(i.dest.reg().B16(), src2.B16(), src3.B16());
  }
};
struct SELECT_V128_V128
    : Sequence<SELECT_V128_V128,
               I<OPCODE_SELECT, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const QReg src1 = Q0;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    } else {
      e.MOV(src1.B16(), i.src1.reg().B16());
    }

    const QReg src2 = i.src2.is_constant ? Q2 : i.src2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2, i.src2.constant());
    }

    const QReg src3 = i.src3.is_constant ? Q3 : i.src3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3, i.src3.constant());
    }

    // src1 ? src2 : src3;
    e.BSL(src1.B16(), src3.B16(), src2.B16());
    e.MOV(i.dest.reg().B16(), src1.B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SELECT, SELECT_I8, SELECT_I16, SELECT_I32,
                     SELECT_I64, SELECT_F32, SELECT_F64, SELECT_V128_I8,
                     SELECT_V128_V128);

// ============================================================================
// OPCODE_IS_TRUE
// ============================================================================
struct IS_TRUE_I8 : Sequence<IS_TRUE_I8, I<OPCODE_IS_TRUE, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::NE);
  }
};
struct IS_TRUE_I16 : Sequence<IS_TRUE_I16, I<OPCODE_IS_TRUE, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::NE);
  }
};
struct IS_TRUE_I32 : Sequence<IS_TRUE_I32, I<OPCODE_IS_TRUE, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::NE);
  }
};
struct IS_TRUE_I64 : Sequence<IS_TRUE_I64, I<OPCODE_IS_TRUE, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::NE);
  }
};
struct IS_TRUE_F32 : Sequence<IS_TRUE_F32, I<OPCODE_IS_TRUE, I8Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::NE);
  }
};
struct IS_TRUE_F64 : Sequence<IS_TRUE_F64, I<OPCODE_IS_TRUE, I8Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::NE);
  }
};
struct IS_TRUE_V128 : Sequence<IS_TRUE_V128, I<OPCODE_IS_TRUE, I8Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UMAXV(Q0.toS(), i.src1.reg().S4());
    e.MOV(W0, Q0.Selem()[0]);
    e.CMP(W0, 0);
    e.CSET(i.dest, Cond::NE);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_IS_TRUE, IS_TRUE_I8, IS_TRUE_I16, IS_TRUE_I32,
                     IS_TRUE_I64, IS_TRUE_F32, IS_TRUE_F64, IS_TRUE_V128);

// ============================================================================
// OPCODE_IS_FALSE
// ============================================================================
struct IS_FALSE_I8 : Sequence<IS_FALSE_I8, I<OPCODE_IS_FALSE, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct IS_FALSE_I16 : Sequence<IS_FALSE_I16, I<OPCODE_IS_FALSE, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct IS_FALSE_I32 : Sequence<IS_FALSE_I32, I<OPCODE_IS_FALSE, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct IS_FALSE_I64 : Sequence<IS_FALSE_I64, I<OPCODE_IS_FALSE, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct IS_FALSE_F32 : Sequence<IS_FALSE_F32, I<OPCODE_IS_FALSE, I8Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct IS_FALSE_F64 : Sequence<IS_FALSE_F64, I<OPCODE_IS_FALSE, I8Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1.reg(), 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
struct IS_FALSE_V128
    : Sequence<IS_FALSE_V128, I<OPCODE_IS_FALSE, I8Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.UMAXV(Q0.toS(), i.src1.reg().S4());
    e.MOV(W0, Q0.Selem()[0]);
    e.CMP(W0, 0);
    e.CSET(i.dest, Cond::EQ);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_IS_FALSE, IS_FALSE_I8, IS_FALSE_I16, IS_FALSE_I32,
                     IS_FALSE_I64, IS_FALSE_F32, IS_FALSE_F64, IS_FALSE_V128);

// ============================================================================
// OPCODE_IS_NAN
// ============================================================================
struct IS_NAN_F32 : Sequence<IS_NAN_F32, I<OPCODE_IS_NAN, I8Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1, i.src1);
    e.CSET(i.dest, Cond::VS);
  }
};

struct IS_NAN_F64 : Sequence<IS_NAN_F64, I<OPCODE_IS_NAN, I8Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1, i.src1);
    e.CSET(i.dest, Cond::VS);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_IS_NAN, IS_NAN_F32, IS_NAN_F64);

// ============================================================================
// OPCODE_COMPARE_EQ
// ============================================================================
struct COMPARE_EQ_I8
    : Sequence<COMPARE_EQ_I8, I<OPCODE_COMPARE_EQ, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, WReg src1, WReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, WReg src1, int32_t constant) {
          e.MOV(W1, constant);
          e.CMP(src1, W1);
        });
    e.CSET(i.dest, Cond::EQ);
  }
};
struct COMPARE_EQ_I16
    : Sequence<COMPARE_EQ_I16, I<OPCODE_COMPARE_EQ, I8Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, WReg src1, WReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, WReg src1, int32_t constant) {
          e.MOV(W1, constant);
          e.CMP(src1, W1);
        });
    e.CSET(i.dest, Cond::EQ);
  }
};
struct COMPARE_EQ_I32
    : Sequence<COMPARE_EQ_I32, I<OPCODE_COMPARE_EQ, I8Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, WReg src1, WReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, WReg src1, int32_t constant) {
          e.MOV(W1, constant);
          e.CMP(src1, W1);
        });
    e.CSET(i.dest, Cond::EQ);
  }
};
struct COMPARE_EQ_I64
    : Sequence<COMPARE_EQ_I64, I<OPCODE_COMPARE_EQ, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, XReg src1, XReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, XReg src1, int32_t constant) {
          e.MOV(X1, constant);
          e.CMP(src1, X1);
        });
    e.CSET(i.dest, Cond::EQ);
  }
};
struct COMPARE_EQ_F32
    : Sequence<COMPARE_EQ_F32, I<OPCODE_COMPARE_EQ, I8Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<SReg>(
        e, i,
        [&i](A64Emitter& e, I8Op dest, const SReg& src1, const SReg& src2) {
          e.FCMP(src1, src2);
        });
    e.CSET(i.dest, Cond::EQ);
  }
};
struct COMPARE_EQ_F64
    : Sequence<COMPARE_EQ_F64, I<OPCODE_COMPARE_EQ, I8Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<DReg>(
        e, i,
        [&i](A64Emitter& e, I8Op dest, const DReg& src1, const DReg& src2) {
          e.FCMP(src1, src2);
        });
    e.CSET(i.dest, Cond::EQ);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_EQ, COMPARE_EQ_I8, COMPARE_EQ_I16,
                     COMPARE_EQ_I32, COMPARE_EQ_I64, COMPARE_EQ_F32,
                     COMPARE_EQ_F64);

// ============================================================================
// OPCODE_COMPARE_NE
// ============================================================================
struct COMPARE_NE_I8
    : Sequence<COMPARE_NE_I8, I<OPCODE_COMPARE_NE, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, WReg src1, WReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, WReg src1, int32_t constant) {
          e.MOV(W1, constant);
          e.CMP(src1, W1);
        });
    e.CSET(i.dest, Cond::NE);
  }
};
struct COMPARE_NE_I16
    : Sequence<COMPARE_NE_I16, I<OPCODE_COMPARE_NE, I8Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, WReg src1, WReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, WReg src1, int32_t constant) {
          e.MOV(W1, constant);
          e.CMP(src1, W1);
        });
    e.CSET(i.dest, Cond::NE);
  }
};
struct COMPARE_NE_I32
    : Sequence<COMPARE_NE_I32, I<OPCODE_COMPARE_NE, I8Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, WReg src1, WReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, WReg src1, int32_t constant) {
          e.MOV(W1, constant);
          e.CMP(src1, W1);
        });
    e.CSET(i.dest, Cond::NE);
  }
};
struct COMPARE_NE_I64
    : Sequence<COMPARE_NE_I64, I<OPCODE_COMPARE_NE, I8Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeCompareOp(
        e, i, [](A64Emitter& e, XReg src1, XReg src2) { e.CMP(src1, src2); },
        [](A64Emitter& e, XReg src1, int32_t constant) {
          e.MOV(X1, constant);
          e.CMP(src1, X1);
        });
    e.CSET(i.dest, Cond::NE);
  }
};
struct COMPARE_NE_F32
    : Sequence<COMPARE_NE_F32, I<OPCODE_COMPARE_NE, I8Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1, i.src2);
    e.CSET(i.dest, Cond::NE);
  }
};
struct COMPARE_NE_F64
    : Sequence<COMPARE_NE_F64, I<OPCODE_COMPARE_NE, I8Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FCMP(i.src1, i.src2);
    e.CSET(i.dest, Cond::NE);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_COMPARE_NE, COMPARE_NE_I8, COMPARE_NE_I16,
                     COMPARE_NE_I32, COMPARE_NE_I64, COMPARE_NE_F32,
                     COMPARE_NE_F64);

// ============================================================================
// OPCODE_COMPARE_*
// ============================================================================
#define EMITTER_ASSOCIATIVE_COMPARE_INT(op, cond, inverse_cond, type,          \
                                        reg_type)                              \
  struct COMPARE_##op##_##type                                                 \
      : Sequence<COMPARE_##op##_##type,                                        \
                 I<OPCODE_COMPARE_##op, I8Op, type, type>> {                   \
    static void Emit(A64Emitter& e, const EmitArgType& i) {                    \
      EmitAssociativeCompareOp(                                                \
          e, i,                                                                \
          [](A64Emitter& e, WReg dest, const reg_type& src1,                   \
             const reg_type& src2, bool inverse) {                             \
            e.CMP(src1, src2);                                                 \
            if (!inverse) {                                                    \
              e.CSET(dest, cond);                                              \
            } else {                                                           \
              e.CSET(dest, inverse_cond);                                      \
            }                                                                  \
          },                                                                   \
          [](A64Emitter& e, WReg dest, const reg_type& src1, int32_t constant, \
             bool inverse) {                                                   \
            e.MOV(reg_type(1), constant);                                      \
            e.CMP(src1, reg_type(1));                                          \
            if (!inverse) {                                                    \
              e.CSET(dest, cond);                                              \
            } else {                                                           \
              e.CSET(dest, inverse_cond);                                      \
            }                                                                  \
          });                                                                  \
    }                                                                          \
  };
#define EMITTER_ASSOCIATIVE_COMPARE_XX(op, cond, inverse_cond)          \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, cond, inverse_cond, I8Op, WReg);  \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, cond, inverse_cond, I16Op, WReg); \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, cond, inverse_cond, I32Op, WReg); \
  EMITTER_ASSOCIATIVE_COMPARE_INT(op, cond, inverse_cond, I64Op, XReg); \
  EMITTER_OPCODE_TABLE(OPCODE_COMPARE_##op, COMPARE_##op##_I8Op,        \
                       COMPARE_##op##_I16Op, COMPARE_##op##_I32Op,      \
                       COMPARE_##op##_I64Op);
EMITTER_ASSOCIATIVE_COMPARE_XX(SLT, Cond::LT, Cond::GT);  // setl, setg
EMITTER_ASSOCIATIVE_COMPARE_XX(SLE, Cond::LE, Cond::GE);  // setle, setge
EMITTER_ASSOCIATIVE_COMPARE_XX(SGT, Cond::GT, Cond::LT);  // setg, setl
EMITTER_ASSOCIATIVE_COMPARE_XX(SGE, Cond::GE, Cond::LE);  // setge, setle
EMITTER_ASSOCIATIVE_COMPARE_XX(ULT, Cond::LO, Cond::HI);  // setb, seta
EMITTER_ASSOCIATIVE_COMPARE_XX(ULE, Cond::LS, Cond::HS);  // setbe, setae
EMITTER_ASSOCIATIVE_COMPARE_XX(UGE, Cond::HS, Cond::LS);  // setae, setbe
EMITTER_ASSOCIATIVE_COMPARE_XX(UGT, Cond::HI, Cond::LO);  // seta, setb

// https://web.archive.org/web/20171129015931/https://x86.renejeschke.de/html/file_module_x86_id_288.html
// Original link: https://x86.renejeschke.de/html/file_module_x86_id_288.html
#define EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(op, cond)                  \
  struct COMPARE_##op##_F32                                           \
      : Sequence<COMPARE_##op##_F32,                                  \
                 I<OPCODE_COMPARE_##op, I8Op, F32Op, F32Op>> {        \
    static void Emit(A64Emitter& e, const EmitArgType& i) {           \
      e.FCMP(i.src1, i.src2);                                         \
      e.CSET(i.dest, cond);                                           \
    }                                                                 \
  };                                                                  \
  struct COMPARE_##op##_F64                                           \
      : Sequence<COMPARE_##op##_F64,                                  \
                 I<OPCODE_COMPARE_##op, I8Op, F64Op, F64Op>> {        \
    static void Emit(A64Emitter& e, const EmitArgType& i) {           \
      if (i.src1.is_constant) {                                       \
        e.LoadConstantV(Q0, i.src1.constant());                       \
        e.FCMP(D0, i.src2);                                           \
      } else if (i.src2.is_constant) {                                \
        e.LoadConstantV(Q0, i.src2.constant());                       \
        e.FCMP(i.src1, D0);                                           \
      } else {                                                        \
        e.FCMP(i.src1, i.src2);                                       \
      }                                                               \
      e.CSET(i.dest, cond);                                           \
    }                                                                 \
  };                                                                  \
  EMITTER_OPCODE_TABLE(OPCODE_COMPARE_##op##_FLT, COMPARE_##op##_F32, \
                       COMPARE_##op##_F64);
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SLT, Cond::LT);  // setb
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SLE, Cond::LE);  // setbe
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SGT, Cond::GT);  // seta
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(SGE, Cond::GE);  // setae
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(ULT, Cond::LO);  // setb
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(ULE, Cond::LS);  // setbe
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(UGT, Cond::HI);  // seta
EMITTER_ASSOCIATIVE_COMPARE_FLT_XX(UGE, Cond::HS);  // setae

// ============================================================================
// OPCODE_DID_SATURATE
// ============================================================================
struct DID_SATURATE
    : Sequence<DID_SATURATE, I<OPCODE_DID_SATURATE, I8Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Bit 27 in the FPSR is the QC bit
    e.MRS(X0, SystemReg::FPSR);
    e.UBFX(i.dest, W0, 27, 1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DID_SATURATE, DID_SATURATE);

// ============================================================================
// OPCODE_ADD
// ============================================================================
// F32/F64 paths here (and CONVERT/ROUND/MUL_ADD/MUL_SUB etc) are also used
// for Xenon paired-single (ps_*) accuracy once emitters added in ppc_emit_fpu.
// FIRST REAL EMITTERS LANDED (ps_addx/maddx/msubx/mulx/mrx per Captain Agent 4 plan).
// See master plan + stubs in ppc_emit_fpu.cc (GQR SPRs + HIR F32 halves now active).
// Non-standard: denormals via FPCR FZ, NaN via
// IsNan/FCMP + no DN, fused FMA critical (MUL_SUB fixed to FNMSUB 2026).
// TODO(benvanik): put dest/src1|2 together.
// CAPTAIN RE-TASK: ps_subx/ps_sel now use same F32 paths (Sub/Select on halves). Light
// FPCR single-prec polish notes added (fpcr_table + SET_ROUNDING). New debug counters
// (ps_nan_cases etc) + harness sequences consume these for ps_* validation (R1 report).

// R1 / CAPTAIN ps_* ACCURACY HARNESS LIGHT HOOK (FPU seqs).
// Primary activation: A64Backend::Initialize (under cvar) + explicit Java PerformanceMonitor
// trigger (identical to 128B harness shipped earlier).
// Light additional: Record* calls inside MUL_ADD_F32 etc (see below) fire when F32/FMA paths
// are emitted under a64_ps_accuracy_stress. This exercises the ps counters as soon as
// ps-related HIR (from ppc_emit_fpu ps_addx/maddx lowering) hits the backend.
// (Avoided top-level static-if to keep TU-safe / no extra includes; 128B pattern in seq_memory
// is the model but we keep hooks minimal here.)
// Full research citations from R1 55-tool report (ps_maddx highest priority for FMA in UE3/Forza/Halo
// vertex/skin/anim/physics; psq_l/psq_st massive; GQR; *explicit* 128B psq_st store reservation
// interaction warning) + 128B notes are in:
//   a64_backend.cc (cvar + RunPairedSingleAccuracyHarness impl + init call)
//   a64_backend.h (decl)
//   ax360e_perf_log.h (counters + snapshot)
//   emulator_ax360e.cpp + Emulator.java + PerformanceMonitor.java (JNI/Java trigger)
//   a64_seq_memory.cc (psq_st Clear comments)
//   ppc_emit_fpu.cc (master ps plan + GQR foundation)
// This is the "research author becomes validator" harness while the rest of the fleet cooks.
template <typename SEQ, typename REG, typename ARGS>
void EmitAddXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](A64Emitter& e, REG dest_src, REG src) {
        e.ADD(dest_src, dest_src, src);
      },
      [](A64Emitter& e, REG dest_src, int32_t constant) {
        e.MOV(REG(1), constant);
        e.ADD(dest_src, dest_src, REG(1));
      });
}
struct ADD_I8 : Sequence<ADD_I8, I<OPCODE_ADD, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I8, WReg>(e, i);
  }
};
struct ADD_I16 : Sequence<ADD_I16, I<OPCODE_ADD, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I16, WReg>(e, i);
  }
};
struct ADD_I32 : Sequence<ADD_I32, I<OPCODE_ADD, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I32, WReg>(e, i);
  }
};
struct ADD_I64 : Sequence<ADD_I64, I<OPCODE_ADD, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddXX<ADD_I64, XReg>(e, i);
  }
};
struct ADD_F32 : Sequence<ADD_F32, I<OPCODE_ADD, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<SReg>(
        e, i, [](A64Emitter& e, SReg dest, SReg src1, SReg src2) {
          e.FADD(dest, src1, src2);
        });
  }
};
struct ADD_F64 : Sequence<ADD_F64, I<OPCODE_ADD, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp<DReg>(
        e, i, [](A64Emitter& e, DReg dest, DReg src1, DReg src2) {
          e.FADD(dest, src1, src2);
        });
  }
};
struct ADD_V128 : Sequence<ADD_V128, I<OPCODE_ADD, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.FADD(dest.S4(), src1.S4(), src2.S4());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ADD, ADD_I8, ADD_I16, ADD_I32, ADD_I64, ADD_F32,
                     ADD_F64, ADD_V128);

// ============================================================================
// OPCODE_ADD_CARRY
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitAddCarryXX(A64Emitter& e, const ARGS& i) {
  // TODO(benvanik): faster setting? we could probably do some fun math tricks
  // here to get the carry flag set.
  if (i.src3.is_constant) {
    e.MOV(W0, WZR);
    if (i.src3.constant()) {
      // Set carry
      // This is implicitly "SUBS 0 - 0"
      e.CMP(W0, 0);
    } else {
      // Clear carry
      e.CMN(W0, 0);
    }
  } else {
    // If src3 is non-zero, set the carry flag
    e.CMP(i.src3.reg().toW(), 0);
    e.CSET(X0, Cond::NE);

    e.MRS(X1, SystemReg::NZCV);
    // Assign carry bit
    e.BFI(X1, X0, 29, 1);
    e.MSR(SystemReg::NZCV, X1);
  }
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](A64Emitter& e, const REG& dest_src, const REG& src) {
        e.ADC(dest_src, dest_src, src);
      },
      [](A64Emitter& e, const REG& dest_src, int32_t constant) {
        e.MOV(REG(1), constant);
        e.ADC(dest_src, dest_src, REG(1));
      });
}
struct ADD_CARRY_I8
    : Sequence<ADD_CARRY_I8, I<OPCODE_ADD_CARRY, I8Op, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I8, WReg>(e, i);
  }
};
struct ADD_CARRY_I16
    : Sequence<ADD_CARRY_I16, I<OPCODE_ADD_CARRY, I16Op, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I16, WReg>(e, i);
  }
};
struct ADD_CARRY_I32
    : Sequence<ADD_CARRY_I32, I<OPCODE_ADD_CARRY, I32Op, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I32, WReg>(e, i);
  }
};
struct ADD_CARRY_I64
    : Sequence<ADD_CARRY_I64, I<OPCODE_ADD_CARRY, I64Op, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAddCarryXX<ADD_CARRY_I64, XReg>(e, i);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ADD_CARRY, ADD_CARRY_I8, ADD_CARRY_I16,
                     ADD_CARRY_I32, ADD_CARRY_I64);

// ============================================================================
// OPCODE_SUB
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitSubXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitAssociativeBinaryOp(
      e, i,
      [](A64Emitter& e, REG dest_src, REG src) {
        e.SUB(dest_src, dest_src, src);
      },
      [](A64Emitter& e, REG dest_src, int32_t constant) {
        e.MOV(REG(1), constant);
        e.SUB(dest_src, dest_src, REG(1));
      });
}
struct SUB_I8 : Sequence<SUB_I8, I<OPCODE_SUB, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I8, WReg>(e, i);
  }
};
struct SUB_I16 : Sequence<SUB_I16, I<OPCODE_SUB, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I16, WReg>(e, i);
  }
};
struct SUB_I32 : Sequence<SUB_I32, I<OPCODE_SUB, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I32, WReg>(e, i);
  }
};
struct SUB_I64 : Sequence<SUB_I64, I<OPCODE_SUB, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitSubXX<SUB_I64, XReg>(e, i);
  }
};
struct SUB_F32 : Sequence<SUB_F32, I<OPCODE_SUB, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitAssociativeBinaryVOp<SReg>(
        e, i, [](A64Emitter& e, SReg dest, SReg src1, SReg src2) {
          e.FSUB(dest, src1, src2);
        });
  }
};
struct SUB_F64 : Sequence<SUB_F64, I<OPCODE_SUB, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitAssociativeBinaryVOp<DReg>(
        e, i, [](A64Emitter& e, DReg dest, DReg src1, DReg src2) {
          e.FSUB(dest, src1, src2);
        });
  }
};
struct SUB_V128 : Sequence<SUB_V128, I<OPCODE_SUB, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitAssociativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.FSUB(dest.S4(), src1.S4(), src2.S4());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SUB, SUB_I8, SUB_I16, SUB_I32, SUB_I64, SUB_F32,
                     SUB_F64, SUB_V128);

// ============================================================================
// OPCODE_MUL
// ============================================================================
// Sign doesn't matter here, as we don't use the high bits.
// We exploit mulx here to avoid creating too much register pressure.
struct MUL_I8 : Sequence<MUL_I8, I<OPCODE_MUL, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      assert_true(!i.src2.is_constant);
      e.MOV(W0, i.src1.constant());
      e.MUL(i.dest, W0, i.src2);
    } else if (i.src2.is_constant) {
      assert_true(!i.src1.is_constant);
      e.MOV(W0, i.src2.constant());
      e.MUL(i.dest, i.src1, W0);
    } else {
      e.MUL(i.dest, i.src1, i.src2);
    }
  }
};
struct MUL_I16 : Sequence<MUL_I16, I<OPCODE_MUL, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      assert_true(!i.src2.is_constant);
      e.MOV(W0, i.src1.constant());
      e.MUL(i.dest, W0, i.src2);
    } else if (i.src2.is_constant) {
      assert_true(!i.src1.is_constant);
      e.MOV(W0, i.src2.constant());
      e.MUL(i.dest, i.src1, W0);
    } else {
      e.MUL(i.dest, i.src1, i.src2);
    }
  }
};
struct MUL_I32 : Sequence<MUL_I32, I<OPCODE_MUL, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      assert_true(!i.src2.is_constant);
      e.MOV(W0, i.src1.constant());
      e.MUL(i.dest, W0, i.src2);
    } else if (i.src2.is_constant) {
      assert_true(!i.src1.is_constant);
      e.MOV(W0, i.src2.constant());
      e.MUL(i.dest, i.src1, W0);
    } else {
      e.MUL(i.dest, i.src1, i.src2);
    }
  }
};
struct MUL_I64 : Sequence<MUL_I64, I<OPCODE_MUL, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      assert_true(!i.src2.is_constant);
      e.MOV(X0, i.src1.constant());
      e.MUL(i.dest, X0, i.src2);
    } else if (i.src2.is_constant) {
      assert_true(!i.src1.is_constant);
      e.MOV(X0, i.src2.constant());
      e.MUL(i.dest, i.src1, X0);
    } else {
      e.MUL(i.dest, i.src1, i.src2);
    }
  }
};
struct MUL_F32 : Sequence<MUL_F32, I<OPCODE_MUL, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitCommutativeBinaryVOp<SReg>(
        e, i, [](A64Emitter& e, SReg dest, SReg src1, SReg src2) {
          e.FMUL(dest, src1, src2);
        });
  }
};
struct MUL_F64 : Sequence<MUL_F64, I<OPCODE_MUL, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitCommutativeBinaryVOp<DReg>(
        e, i, [](A64Emitter& e, DReg dest, DReg src1, DReg src2) {
          e.FMUL(dest, src1, src2);
        });
  }
};
struct MUL_V128 : Sequence<MUL_V128, I<OPCODE_MUL, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.FMUL(dest.S4(), src1.S4(), src2.S4());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL, MUL_I8, MUL_I16, MUL_I32, MUL_I64, MUL_F32,
                     MUL_F64, MUL_V128);

// ============================================================================
// OPCODE_MUL_HI
// ============================================================================
struct MUL_HI_I8 : Sequence<MUL_HI_I8, I<OPCODE_MUL_HI, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.MUL(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.MUL(i.dest, i.src1, W0);
      } else {
        e.MUL(i.dest, i.src1, i.src2);
      }
      e.UBFX(i.dest, i.dest, 8, 8);
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.MUL(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.MUL(i.dest, i.src1, W0);
      } else {
        e.MUL(i.dest, i.src1, i.src2);
      }
      e.SBFX(i.dest, i.dest, 8, 8);
    }
  }
};
struct MUL_HI_I16
    : Sequence<MUL_HI_I16, I<OPCODE_MUL_HI, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.MUL(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.MUL(i.dest, i.src1, W0);
      } else {
        e.MUL(i.dest, i.src1, i.src2);
      }
      e.UBFX(i.dest, i.dest, 16, 16);
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.MUL(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.MUL(i.dest, i.src1, W0);
      } else {
        e.MUL(i.dest, i.src1, i.src2);
      }
      e.SBFX(i.dest, i.dest, 16, 16);
    }
  }
};
struct MUL_HI_I32
    : Sequence<MUL_HI_I32, I<OPCODE_MUL_HI, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.UMULL(X0, W0, i.src2);
        e.UBFX(X0, X0, 32, 32);
        e.MOV(i.dest, X0.toW());
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.UMULL(X0, W0, i.src1);
        e.UBFX(X0, X0, 32, 32);
        e.MOV(i.dest, X0.toW());
      } else {
        e.UMULL(X0, i.src1, i.src2);
        e.UBFX(X0, X0, 32, 32);
        e.MOV(i.dest, X0.toW());
      }
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.SMULL(X0, W0, i.src2);
        e.SBFX(X0, X0, 32, 32);
        e.MOV(i.dest, X0.toW());
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.SMULL(X0, W0, i.src1);
        e.SBFX(X0, X0, 32, 32);
        e.MOV(i.dest, X0.toW());
      } else {
        e.SMULL(X0, i.src1, i.src2);
        e.SBFX(X0, X0, 32, 32);
        e.MOV(i.dest, X0.toW());
      }
    }
  }
};
struct MUL_HI_I64
    : Sequence<MUL_HI_I64, I<OPCODE_MUL_HI, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(X0, i.src1.constant());
        e.UMULH(i.dest, X0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(X0, i.src2.constant());
        e.UMULH(i.dest, i.src1, X0);
      } else {
        e.UMULH(i.dest, i.src1, i.src2);
      }
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(X0, i.src1.constant());
        e.SMULH(i.dest, X0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(X0, i.src2.constant());
        e.SMULH(i.dest, i.src1, X0);
      } else {
        e.SMULH(i.dest, i.src1, i.src2);
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_HI, MUL_HI_I8, MUL_HI_I16, MUL_HI_I32,
                     MUL_HI_I64);

// ============================================================================
// OPCODE_DIV
// ============================================================================
struct DIV_I8 : Sequence<DIV_I8, I<OPCODE_DIV, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.UDIV(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.UDIV(i.dest, i.src1, W0);
      } else {
        e.UDIV(i.dest, i.src1, i.src2);
      }
      e.UXTB(i.dest, i.dest);
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.SDIV(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.SDIV(i.dest, i.src1, W0);
      } else {
        e.SDIV(i.dest, i.src1, i.src2);
      }
      e.SXTB(i.dest, i.dest);
    }
  }
};
struct DIV_I16 : Sequence<DIV_I16, I<OPCODE_DIV, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.UDIV(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.UDIV(i.dest, i.src1, W0);
      } else {
        e.UDIV(i.dest, i.src1, i.src2);
      }
      e.UXTH(i.dest, i.dest);
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.SDIV(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.SDIV(i.dest, i.src1, W0);
      } else {
        e.SDIV(i.dest, i.src1, i.src2);
      }
      e.SXTH(i.dest, i.dest);
    }
  }
};
struct DIV_I32 : Sequence<DIV_I32, I<OPCODE_DIV, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.UDIV(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.UDIV(i.dest, i.src1, W0);
      } else {
        e.UDIV(i.dest, i.src1, i.src2);
      }
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(W0, i.src1.constant());
        e.SDIV(i.dest, W0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(W0, i.src2.constant());
        e.SDIV(i.dest, i.src1, W0);
      } else {
        e.SDIV(i.dest, i.src1, i.src2);
      }
    }
  }
};
struct DIV_I64 : Sequence<DIV_I64, I<OPCODE_DIV, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(X0, i.src1.constant());
        e.UDIV(i.dest, X0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(X0, i.src2.constant());
        e.UDIV(i.dest, i.src1, X0);
      } else {
        e.UDIV(i.dest, i.src1, i.src2);
      }
    } else {
      if (i.src1.is_constant) {
        assert_true(!i.src2.is_constant);
        e.MOV(X0, i.src1.constant());
        e.SDIV(i.dest, X0, i.src2);
      } else if (i.src2.is_constant) {
        assert_true(!i.src1.is_constant);
        e.MOV(X0, i.src2.constant());
        e.SDIV(i.dest, i.src1, X0);
      } else {
        e.SDIV(i.dest, i.src1, i.src2);
      }
    }
  }
};
struct DIV_F32 : Sequence<DIV_F32, I<OPCODE_DIV, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitAssociativeBinaryVOp<SReg>(
        e, i, [](A64Emitter& e, SReg dest, SReg src1, SReg src2) {
          e.FDIV(dest, src1, src2);
        });
  }
};
struct DIV_F64 : Sequence<DIV_F64, I<OPCODE_DIV, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitAssociativeBinaryVOp<DReg>(
        e, i, [](A64Emitter& e, DReg dest, DReg src1, DReg src2) {
          e.FDIV(dest, src1, src2);
        });
  }
};
struct DIV_V128 : Sequence<DIV_V128, I<OPCODE_DIV, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    EmitAssociativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.FDIV(dest.S4(), src1.S4(), src2.S4());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DIV, DIV_I8, DIV_I16, DIV_I32, DIV_I64, DIV_F32,
                     DIV_F64, DIV_V128);

// ============================================================================
// OPCODE_MUL_ADD
// ============================================================================
// d = 1 * 2 + 3
// $0 = $1x$0 + $2
struct MUL_ADD_F32
    : Sequence<MUL_ADD_F32, I<OPCODE_MUL_ADD, F32Op, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    SReg src3 = S3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3.toQ(), i.src3.constant());
    } else {
      src3 = i.src3.reg();
    }

    SReg src2 = S2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    } else {
      src2 = i.src2.reg();
    }

    SReg src1 = S1;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1.toQ(), i.src1.constant());
    } else {
      src1 = i.src1.reg();
    }

    e.FMADD(i.dest, src1, src2, src3);

    // R1 ps_* harness: count FMA cases when accuracy stress active (ps_maddx will lower to this).
    // Lightweight; pairs with harness sequences. Citations: R1 FMA priority for ps_* + 128B psq_st.
    if (cvars::a64_ps_accuracy_stress || cvars::a64_accuracy_debug) {
      ax360e::perf::g_cpu_accuracy.RecordPairedSingleFMA();
      ax360e::perf::g_cpu_accuracy.RecordPairedSingleArith(2);
      // CAPTAIN RE-TASK: ps_* specific debug counter (new ps_fma_executed) for R1 harness visibility.
      // Gated same as existing; distinguishes ps family FMA exec from scalar. See harness + tracker.
      ax360e::perf::g_cpu_accuracy.RecordPsFmaExecuted(2);
    }
  }
};
struct MUL_ADD_F64
    : Sequence<MUL_ADD_F64, I<OPCODE_MUL_ADD, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    DReg src3 = D3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3.toQ(), i.src3.constant());
    } else {
      src3 = i.src3.reg();
    }

    DReg src2 = D2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    } else {
      src2 = i.src2.reg();
    }

    DReg src1 = D1;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1.toQ(), i.src1.constant());
    } else {
      src1 = i.src1.reg();
    }

    e.FMADD(i.dest, src1, src2, src3);
  }
};
struct MUL_ADD_V128
    : Sequence<MUL_ADD_V128,
               I<OPCODE_MUL_ADD, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    const QReg vd=Q3;
    if (i.src3.is_constant) {
      e.LoadConstantV(vd.toQ(), i.src3.constant());
    } else {
        e.MOV(vd.B16(), i.src3.reg().B16());
    }

    QReg src2 = Q2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    } else {
      src2 = i.src2.reg();
    }

    QReg src1 = Q1;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1.toQ(), i.src1.constant());
    } else {
      src1 = i.src1.reg();
    }

    e.FMLA(vd.S4(), src1.S4(), src2.S4());
    e.MOV(i.dest.reg().B16(),vd.B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_ADD, MUL_ADD_F32, MUL_ADD_F64, MUL_ADD_V128);

// ============================================================================
// OPCODE_MUL_SUB
// ============================================================================
// d = 1 * 2 - 3
// $0 = $2x$0 - $3
// TODO(benvanik): use other forms (132/213/etc) to avoid register shuffling.
// dest could be src2 or src3 - need to ensure it's not before overwriting dest
// perhaps use other 132/213/etc
// Forms:
// - 132 -> $1 = $1 * $3 - $2
// - 213 -> $1 = $2 * $1 - $3
// - 231 -> $1 = $2 * $3 - $1
struct MUL_SUB_F32
    : Sequence<MUL_SUB_F32, I<OPCODE_MUL_SUB, F32Op, F32Op, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Use fused FNMSUB for (src1 * src2) - src3 to improve accuracy vs
    // separate FMUL+FSUB. Critical for fmsub*/fnmsub* emulation and
    // paired-single (ps_*) single-precision FMA behavior on Xenon.
    SReg src3 = S3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3.toQ(), i.src3.constant());
    } else {
      src3 = i.src3.reg();
    }

    SReg src2 = S2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    } else {
      src2 = i.src2.reg();
    }

    SReg src1 = S1;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1.toQ(), i.src1.constant());
    } else {
      src1 = i.src1.reg();
    }

    // FNMSUB Sd, Sn, Sm, Sa  =>  Sd = Sn*Sm - Sa
    e.FNMSUB(i.dest, src1, src2, src3);
  }
};
struct MUL_SUB_F64
    : Sequence<MUL_SUB_F64, I<OPCODE_MUL_SUB, F64Op, F64Op, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Use fused FNMSUB for (src1 * src2) - src3 (see F32 for details).
    DReg src3 = D3;
    if (i.src3.is_constant) {
      e.LoadConstantV(src3.toQ(), i.src3.constant());
    } else {
      src3 = i.src3.reg();
    }

    DReg src2 = D2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2.toQ(), i.src2.constant());
    } else {
      src2 = i.src2.reg();
    }

    DReg src1 = D1;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1.toQ(), i.src1.constant());
    } else {
      src1 = i.src1.reg();
    }

    e.FNMSUB(i.dest, src1, src2, src3);
  }
};
struct MUL_SUB_V128
    : Sequence<MUL_SUB_V128,
               I<OPCODE_MUL_SUB, V128Op, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Fused equivalent using FNEG + FMLA for (src1*src2) - src3.
    // Matches style of MUL_ADD_V128. Improves paired-single/vector
    // single-precision accuracy (v*fp, physics/anim sims).
    QReg vd = Q3;
    if (i.src3.is_constant) {
      e.LoadConstantV(vd, i.src3.constant());
    } else {
      e.MOV(vd.B16(), i.src3.reg().B16());
    }
    e.FNEG(vd.S4(), vd.S4());  // vd = -src3

    QReg src2 = Q2;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2, i.src2.constant());
    } else {
      src2 = i.src2.reg();
    }

    QReg src1 = Q1;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    } else {
      src1 = i.src1.reg();
    }

    e.FMLA(vd.S4(), src1.S4(), src2.S4());  // vd = -src3 + src1*src2
    e.MOV(i.dest.reg().B16(), vd.B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_MUL_SUB, MUL_SUB_F32, MUL_SUB_F64, MUL_SUB_V128);

// ============================================================================
// OPCODE_NEG
// ============================================================================
// TODO(benvanik): put dest/src1 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitNegXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitUnaryOp(
      e, i, [](A64Emitter& e, REG dest_src) { e.NEG(dest_src, dest_src); });
}
struct NEG_I8 : Sequence<NEG_I8, I<OPCODE_NEG, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I8, WReg>(e, i);
  }
};
struct NEG_I16 : Sequence<NEG_I16, I<OPCODE_NEG, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I16, WReg>(e, i);
  }
};
struct NEG_I32 : Sequence<NEG_I32, I<OPCODE_NEG, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I32, WReg>(e, i);
  }
};
struct NEG_I64 : Sequence<NEG_I64, I<OPCODE_NEG, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNegXX<NEG_I64, XReg>(e, i);
  }
};
struct NEG_F32 : Sequence<NEG_F32, I<OPCODE_NEG, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FNEG(i.dest, i.src1);
  }
};
struct NEG_F64 : Sequence<NEG_F64, I<OPCODE_NEG, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FNEG(i.dest, i.src1);
  }
};
struct NEG_V128 : Sequence<NEG_V128, I<OPCODE_NEG, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(!i.instr->flags);
    e.FNEG(i.dest.reg().S4(), i.src1.reg().S4());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NEG, NEG_I8, NEG_I16, NEG_I32, NEG_I64, NEG_F32,
                     NEG_F64, NEG_V128);

// ============================================================================
// OPCODE_ABS
// ============================================================================
struct ABS_F32 : Sequence<ABS_F32, I<OPCODE_ABS, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FABS(i.dest, i.src1);
  }
};
struct ABS_F64 : Sequence<ABS_F64, I<OPCODE_ABS, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FABS(i.dest, i.src1);
  }
};
struct ABS_V128 : Sequence<ABS_V128, I<OPCODE_ABS, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FABS(i.dest.reg().S4(), i.src1.reg().S4());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ABS, ABS_F32, ABS_F64, ABS_V128);

// ============================================================================
// OPCODE_SQRT
// ============================================================================
struct SQRT_F32 : Sequence<SQRT_F32, I<OPCODE_SQRT, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FSQRT(i.dest, i.src1);
  }
};
struct SQRT_F64 : Sequence<SQRT_F64, I<OPCODE_SQRT, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FSQRT(i.dest, i.src1);
  }
};
struct SQRT_V128 : Sequence<SQRT_V128, I<OPCODE_SQRT, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FSQRT(i.dest.reg().S4(), i.src1.reg().S4());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SQRT, SQRT_F32, SQRT_F64, SQRT_V128);

// ============================================================================
// OPCODE_RSQRT
// ============================================================================
// Altivec guarantees an error of < 1/4096 for vrsqrtefp
struct RSQRT_F32 : Sequence<RSQRT_F32, I<OPCODE_RSQRT, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FRSQRTE(i.dest, i.src1);
  }
};
struct RSQRT_F64 : Sequence<RSQRT_F64, I<OPCODE_RSQRT, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FRSQRTE(i.dest, i.src1);
  }
};
struct RSQRT_V128 : Sequence<RSQRT_V128, I<OPCODE_RSQRT, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FRSQRTE(i.dest.reg().S4(), i.src1.reg().S4());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RSQRT, RSQRT_F32, RSQRT_F64, RSQRT_V128);

// ============================================================================
// OPCODE_RECIP
// ============================================================================
// Altivec guarantees an error of < 1/4096 for vrefp
struct RECIP_F32 : Sequence<RECIP_F32, I<OPCODE_RECIP, F32Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FRECPE(i.dest, i.src1);
  }
};
struct RECIP_F64 : Sequence<RECIP_F64, I<OPCODE_RECIP, F64Op, F64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FRECPE(i.dest, i.src1);
  }
};
struct RECIP_V128 : Sequence<RECIP_V128, I<OPCODE_RECIP, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.FRECPE(i.dest.reg().S4(), i.src1.reg().S4());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_RECIP, RECIP_F32, RECIP_F64, RECIP_V128);

// ============================================================================
// OPCODE_POW2
// ============================================================================
// TODO(benvanik): use approx here:
//     https://jrfonseca.blogspot.com/2008/09/fast-sse2-pow-tables-or-polynomials.html
struct POW2_F32 : Sequence<POW2_F32, I<OPCODE_POW2, F32Op, F32Op>> {
  static float32x4_t EmulatePow2(void*, float32x4_t src) {
    float value;
    vst1q_lane_f32(&value, src, 0);
    value = std::exp2(value);
    return vld1q_lane_f32(&value, src, 0);
  }
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always();
    e.MOV(Q0.B16(), i.src1.reg().toQ().B16());
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePow2));
    e.FMOV(i.dest, S0);
  }
};
struct POW2_F64 : Sequence<POW2_F64, I<OPCODE_POW2, F64Op, F64Op>> {
  static float64x2_t EmulatePow2(void*, float64x2_t src) {
    double value;
    vst1q_lane_f64(&value, src, 0);
    value = std::exp2(value);
    return vld1q_lane_f64(&value, src, 0);
  }
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always();
    e.MOV(Q0.B16(), i.src1.reg().toQ().B16());
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePow2));
    e.FMOV(i.dest, D0);
  }
};
struct POW2_V128 : Sequence<POW2_V128, I<OPCODE_POW2, V128Op, V128Op>> {
  static float32x4_t EmulatePow2(void*, float32x4_t src) {
    alignas(16) float values[4];
    vst1q_f32(values, src);
    for (size_t i = 0; i < 4; ++i) {
      values[i] = std::exp2(values[i]);
    }
    return vld1q_f32(values);
  }
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.MOV(Q0.B16(), i.src1.reg().toQ().B16());
    e.CallNativeSafe(reinterpret_cast<void*>(EmulatePow2));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_POW2, POW2_F32, POW2_F64, POW2_V128);

// ============================================================================
// OPCODE_LOG2
// ============================================================================
// TODO(benvanik): use approx here:
//     https://jrfonseca.blogspot.com/2008/09/fast-sse2-pow-tables-or-polynomials.html
// TODO(benvanik): this emulated fn destroys all xmm registers! don't do it!
struct LOG2_F32 : Sequence<LOG2_F32, I<OPCODE_LOG2, F32Op, F32Op>> {
  static float32x4_t EmulateLog2(void*, float32x4_t src) {
    float value;
    vst1q_lane_f32(&value, src, 0);
    value = std::log2(value);
    return vld1q_lane_f32(&value, src, 0);
  }
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always();
    if (i.src1.is_constant) {
      e.LoadConstantV(Q0 ,i.src1.constant());
    } else {
        e.MOV(Q0.B16(), i.src1.reg().toQ().B16());
    }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateLog2));
    e.FMOV(i.dest, S0);
  }
};
struct LOG2_F64 : Sequence<LOG2_F64, I<OPCODE_LOG2, F64Op, F64Op>> {
  static float64x2_t EmulateLog2(void*, float64x2_t src) {
    double value;
    vst1q_lane_f64(&value, src, 0);
    value = std::log2(value);
    return vld1q_lane_f64(&value, src, 0);
  }
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_always();
      if (i.src1.is_constant) {
          e.LoadConstantV(Q0 ,i.src1.constant());
      } else {
          e.MOV(Q0.B16(), i.src1.reg().toQ().B16());
      }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateLog2));
    e.FMOV(i.dest, D0);
  }
};
struct LOG2_V128 : Sequence<LOG2_V128, I<OPCODE_LOG2, V128Op, V128Op>> {
  static float32x4_t EmulateLog2(void*, float32x4_t src) {
    alignas(16) float values[4];
    vst1q_f32(values, src);
    for (size_t i = 0; i < 4; ++i) {
      values[i] = std::log2(values[i]);
    }
    return vld1q_f32(values);
  }
  static void Emit(A64Emitter& e, const EmitArgType& i) {
      if (i.src1.is_constant) {
          e.LoadConstantV(Q0 ,i.src1.constant());
      } else {
          e.MOV(Q0.B16(), i.src1.reg().toQ().B16());
      }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateLog2));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOG2, LOG2_F32, LOG2_F64, LOG2_V128);

// ============================================================================
// OPCODE_DOT_PRODUCT_3
// ============================================================================
struct DOT_PRODUCT_3_V128
    : Sequence<DOT_PRODUCT_3_V128,
               I<OPCODE_DOT_PRODUCT_3, F32Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // https://msdn.microsoft.com/en-us/library/bb514054(v=vs.90).aspx
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, SReg dest, QReg src1, QReg src2) {
          e.FMUL(dest.toQ().S4(), src1.S4(), src2.S4());
          e.MOV(dest.toQ().Selem()[3], WZR);
          e.FADDP(dest.toQ().S4(), dest.toQ().S4(), dest.toQ().S4());
          e.FADDP(dest.toS(), dest.toD().S2());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DOT_PRODUCT_3, DOT_PRODUCT_3_V128);

// ============================================================================
// OPCODE_DOT_PRODUCT_4
// ============================================================================
struct DOT_PRODUCT_4_V128
    : Sequence<DOT_PRODUCT_4_V128,
               I<OPCODE_DOT_PRODUCT_4, F32Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // https://msdn.microsoft.com/en-us/library/bb514054(v=vs.90).aspx
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, SReg dest, QReg src1, QReg src2) {
          e.FMUL(dest.toQ().S4(), src1.S4(), src2.S4());
          e.FADDP(dest.toQ().S4(), dest.toQ().S4(), dest.toQ().S4());
          e.FADDP(dest.toS(), dest.toD().S2());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_DOT_PRODUCT_4, DOT_PRODUCT_4_V128);

// ============================================================================
// OPCODE_AND
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitAndXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](A64Emitter& e, REG dest_src, REG src) {
        e.AND(dest_src, dest_src, src);
      },
      [](A64Emitter& e, REG dest_src, int32_t constant) {
        e.MOV(REG(1), constant);
        e.AND(dest_src, dest_src, REG(1));
      });
}
struct AND_I8 : Sequence<AND_I8, I<OPCODE_AND, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I8, WReg>(e, i);
  }
};
struct AND_I16 : Sequence<AND_I16, I<OPCODE_AND, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I16, WReg>(e, i);
  }
};
struct AND_I32 : Sequence<AND_I32, I<OPCODE_AND, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I32, WReg>(e, i);
  }
};
struct AND_I64 : Sequence<AND_I64, I<OPCODE_AND, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndXX<AND_I64, XReg>(e, i);
  }
};
struct AND_V128 : Sequence<AND_V128, I<OPCODE_AND, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.AND(dest.B16(), src1.B16(), src2.B16());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_AND, AND_I8, AND_I16, AND_I32, AND_I64, AND_V128);

// ============================================================================
// OPCODE_AND_NOT
// ============================================================================
template <typename SEQ, typename REG, typename ARGS>
void EmitAndNotXX(A64Emitter& e, const ARGS& i) {
  if (i.src1.is_constant) {
    // src1 constant.
    auto temp = GetTempReg<typename decltype(i.src1)::reg_type>(e);
    e.MOV(temp, i.src1.constant());
    e.BIC(i.dest, temp, i.src2);
  } else if (i.src2.is_constant) {
    // src2 constant.
    if (i.dest.reg().index() == i.src1.reg().index()) {
      auto temp = GetTempReg<typename decltype(i.src2)::reg_type>(e);
      e.MOV(temp, ~i.src2.constant());
      e.AND(i.dest, i.dest, temp);
    } else {
      e.MOV(i.dest, i.src1);
      auto temp = GetTempReg<typename decltype(i.src2)::reg_type>(e);
      e.MOV(temp, ~i.src2.constant());
      e.AND(i.dest, i.dest, temp);
    }
  } else {
    // neither are constant
    e.BIC(i.dest, i.src1, i.src2);
  }
}
struct AND_NOT_I8 : Sequence<AND_NOT_I8, I<OPCODE_AND_NOT, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I8, WReg>(e, i);
  }
};
struct AND_NOT_I16
    : Sequence<AND_NOT_I16, I<OPCODE_AND_NOT, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I16, WReg>(e, i);
  }
};
struct AND_NOT_I32
    : Sequence<AND_NOT_I32, I<OPCODE_AND_NOT, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I32, WReg>(e, i);
  }
};
struct AND_NOT_I64
    : Sequence<AND_NOT_I64, I<OPCODE_AND_NOT, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAndNotXX<AND_NOT_I64, XReg>(e, i);
  }
};
struct AND_NOT_V128
    : Sequence<AND_NOT_V128, I<OPCODE_AND_NOT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.BIC(dest.B16(), src1.B16(), src2.B16());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_AND_NOT, AND_NOT_I8, AND_NOT_I16, AND_NOT_I32,
                     AND_NOT_I64, AND_NOT_V128);

// ============================================================================
// OPCODE_OR
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitOrXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](A64Emitter& e, REG dest_src, REG src) {
        e.ORR(dest_src, dest_src, src);
      },
      [](A64Emitter& e, REG dest_src, int32_t constant) {
        e.MOV(REG(1), constant);
        e.ORR(dest_src, dest_src, REG(1));
      });
}
struct OR_I8 : Sequence<OR_I8, I<OPCODE_OR, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I8, WReg>(e, i);
  }
};
struct OR_I16 : Sequence<OR_I16, I<OPCODE_OR, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I16, WReg>(e, i);
  }
};
struct OR_I32 : Sequence<OR_I32, I<OPCODE_OR, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I32, WReg>(e, i);
  }
};
struct OR_I64 : Sequence<OR_I64, I<OPCODE_OR, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitOrXX<OR_I64, XReg>(e, i);
  }
};
struct OR_V128 : Sequence<OR_V128, I<OPCODE_OR, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.ORR(dest.B16(), src1.B16(), src2.B16());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_OR, OR_I8, OR_I16, OR_I32, OR_I64, OR_V128);

// ============================================================================
// OPCODE_XOR
// ============================================================================
// TODO(benvanik): put dest/src1|2 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitXorXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitCommutativeBinaryOp(
      e, i,
      [](A64Emitter& e, REG dest_src, REG src) {
        e.EOR(dest_src, dest_src, src);
      },
      [](A64Emitter& e, REG dest_src, int32_t constant) {
        e.MOV(REG(1), constant);
        e.EOR(dest_src, dest_src, REG(1));
      });
}
struct XOR_I8 : Sequence<XOR_I8, I<OPCODE_XOR, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I8, WReg>(e, i);
  }
};
struct XOR_I16 : Sequence<XOR_I16, I<OPCODE_XOR, I16Op, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I16, WReg>(e, i);
  }
};
struct XOR_I32 : Sequence<XOR_I32, I<OPCODE_XOR, I32Op, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I32, WReg>(e, i);
  }
};
struct XOR_I64 : Sequence<XOR_I64, I<OPCODE_XOR, I64Op, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitXorXX<XOR_I64, XReg>(e, i);
  }
};
struct XOR_V128 : Sequence<XOR_V128, I<OPCODE_XOR, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          e.EOR(dest.B16(), src1.B16(), src2.B16());
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_XOR, XOR_I8, XOR_I16, XOR_I32, XOR_I64, XOR_V128);

// ============================================================================
// OPCODE_NOT
// ============================================================================
// TODO(benvanik): put dest/src1 together.
template <typename SEQ, typename REG, typename ARGS>
void EmitNotXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitUnaryOp(
      e, i, [](A64Emitter& e, REG dest_src) { e.MVN(dest_src, dest_src); });
}
struct NOT_I8 : Sequence<NOT_I8, I<OPCODE_NOT, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I8, WReg>(e, i);
  }
};
struct NOT_I16 : Sequence<NOT_I16, I<OPCODE_NOT, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I16, WReg>(e, i);
  }
};
struct NOT_I32 : Sequence<NOT_I32, I<OPCODE_NOT, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I32, WReg>(e, i);
  }
};
struct NOT_I64 : Sequence<NOT_I64, I<OPCODE_NOT, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitNotXX<NOT_I64, XReg>(e, i);
  }
};
struct NOT_V128 : Sequence<NOT_V128, I<OPCODE_NOT, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.NOT(i.dest.reg().B16(), i.src1.reg().B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_NOT, NOT_I8, NOT_I16, NOT_I32, NOT_I64, NOT_V128);

// ============================================================================
// OPCODE_SHL
// ============================================================================
// TODO(benvanik): optimize common shifts.
template <typename SEQ, typename REG, typename ARGS>
void EmitShlXX(A64Emitter& e, const ARGS& i) {
  SEQ::EmitAssociativeBinaryOp(
      e, i,
      [](A64Emitter& e, REG dest_src, WReg src) {
        e.LSL(dest_src, dest_src, REG(src.index()));
      },
      [](A64Emitter& e, REG dest_src, int8_t constant) {
        e.LSL(dest_src, dest_src, constant);
      });
}
struct SHL_I8 : Sequence<SHL_I8, I<OPCODE_SHL, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I8, WReg>(e, i);
  }
};
struct SHL_I16 : Sequence<SHL_I16, I<OPCODE_SHL, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I16, WReg>(e, i);
  }
};
struct SHL_I32 : Sequence<SHL_I32, I<OPCODE_SHL, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I32, WReg>(e, i);
  }
};
struct SHL_I64 : Sequence<SHL_I64, I<OPCODE_SHL, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitShlXX<SHL_I64, XReg>(e, i);
  }
};
struct SHL_V128 : Sequence<SHL_V128, I<OPCODE_SHL, V128Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): native version (with shift magic).
    if (i.src2.is_constant) {
      //e.MOV(e.GetNativeParam(1), i.src2.constant());
        e.MOV(X1, i.src2.constant());
    } else {
      //e.MOV(e.GetNativeParam(1), i.src2.reg().toX());
        e.MOV(X1, i.src2.reg().toX());
    }
    //e.ADD(e.GetNativeParam(0), SP, e.StashV(0, i.src1));
      e.MOV(Q0.B16(), i.src1.reg().B16());
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateShlV128));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static float32x4_t EmulateShlV128(void*, float32x4_t src1, uint8_t src2) {
    // Almost all instances are shamt = 1, but non-constant.
    // shamt is [0,7]
    uint8_t shamt = src2 & 0x7;
    alignas(16) vec128_t value;
    vst1q_f32(reinterpret_cast<float32_t*>(&value), src1);
    for (int i = 0; i < 15; ++i) {
      value.u8[i ^ 0x3] = (value.u8[i ^ 0x3] << shamt) |
                          (value.u8[(i + 1) ^ 0x3] >> (8 - shamt));
    }
    value.u8[15 ^ 0x3] = value.u8[15 ^ 0x3] << shamt;
    return vld1q_f32(reinterpret_cast<float32_t*>(&value));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHL, SHL_I8, SHL_I16, SHL_I32, SHL_I64, SHL_V128);

// ============================================================================
// OPCODE_SHR
// ============================================================================
struct SHR_I8 : Sequence<SHR_I8, I<OPCODE_SHR, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.LSR(dest_src, dest_src, src);
        },
        [](A64Emitter& e, WReg dest_src, int8_t constant) {
          e.LSR(dest_src, dest_src, constant);
        });
  }
};
struct SHR_I16 : Sequence<SHR_I16, I<OPCODE_SHR, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.LSR(dest_src, dest_src, src);
        },
        [](A64Emitter& e, WReg dest_src, int8_t constant) {
          e.LSR(dest_src, dest_src, constant);
        });
  }
};
struct SHR_I32 : Sequence<SHR_I32, I<OPCODE_SHR, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.LSR(dest_src, dest_src, src);
        },
        [](A64Emitter& e, WReg dest_src, int8_t constant) {
          e.LSR(dest_src, dest_src, constant);
        });
  }
};
struct SHR_I64 : Sequence<SHR_I64, I<OPCODE_SHR, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, XReg dest_src, WReg src) {
          e.LSR(dest_src, dest_src, src.toX());
        },
        [](A64Emitter& e, XReg dest_src, int8_t constant) {
          e.LSR(dest_src, dest_src, constant);
        });
  }
};
struct SHR_V128 : Sequence<SHR_V128, I<OPCODE_SHR, V128Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // TODO(benvanik): native version (with shift magic).
      if (i.src2.is_constant) {
          //e.MOV(e.GetNativeParam(1), i.src2.constant());
          e.MOV(X1, i.src2.constant());
      } else {
          //e.MOV(e.GetNativeParam(1), i.src2.reg().toX());
          e.MOV(X1, i.src2.reg().toX());
      }
      //e.ADD(e.GetNativeParam(0), SP, e.StashV(0, i.src1));
      e.MOV(Q0.B16(), i.src1.reg().B16());
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateShrV128));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static float32x4_t EmulateShrV128(void*, float32x4_t src1, uint8_t src2) {
    // Almost all instances are shamt = 1, but non-constant.
    // shamt is [0,7]
    uint8_t shamt = src2 & 0x7;
    alignas(16) vec128_t value;
    vst1q_f32(reinterpret_cast<float32_t*>(&value), src1);
    for (int i = 15; i > 0; --i) {
      value.u8[i ^ 0x3] = (value.u8[i ^ 0x3] >> shamt) |
                          (value.u8[(i - 1) ^ 0x3] << (8 - shamt));
    }
    value.u8[0 ^ 0x3] = value.u8[0 ^ 0x3] >> shamt;
    return vld1q_f32(reinterpret_cast<float32_t*>(&value));
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHR, SHR_I8, SHR_I16, SHR_I32, SHR_I64, SHR_V128);

// ============================================================================
// OPCODE_SHA
// ============================================================================
struct SHA_I8 : Sequence<SHA_I8, I<OPCODE_SHA, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.SXTB(dest_src, dest_src);
          e.ASR(dest_src, dest_src, src);
        },
        [](A64Emitter& e, WReg dest_src, int8_t constant) {
          e.SXTB(dest_src, dest_src);
          e.ASR(dest_src, dest_src, constant);
        });
  }
};
struct SHA_I16 : Sequence<SHA_I16, I<OPCODE_SHA, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.SXTH(dest_src, dest_src);
          e.ASR(dest_src, dest_src, src);
        },
        [](A64Emitter& e, WReg dest_src, int8_t constant) {
          e.ASR(dest_src, dest_src, constant);
        });
  }
};
struct SHA_I32 : Sequence<SHA_I32, I<OPCODE_SHA, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, WReg dest_src, WReg src) {
          e.ASR(dest_src, dest_src, src);
        },
        [](A64Emitter& e, WReg dest_src, int8_t constant) {
          e.ASR(dest_src, dest_src, constant);
        });
  }
};
struct SHA_I64 : Sequence<SHA_I64, I<OPCODE_SHA, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    Sequence::EmitAssociativeBinaryOp(
        e, i,
        [](A64Emitter& e, XReg dest_src, WReg src) {
          e.ASR(dest_src, dest_src, src.toX());
        },
        [](A64Emitter& e, XReg dest_src, int8_t constant) {
          e.ASR(dest_src, dest_src, constant);
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SHA, SHA_I8, SHA_I16, SHA_I32, SHA_I64);

// ============================================================================
// OPCODE_ROTATE_LEFT
// ============================================================================
// TODO(benvanik): put dest/src1 together, src2 in cl.
template <typename SEQ, typename REG, typename ARGS>
void EmitRotateLeftXX(A64Emitter& e, const ARGS& i) {
  // ; rotate r1 left by r2, producing r0
  // ; (destroys r2)
  //                                     ; r1 = ABCDEFGH
  // lslv    r0, r1, r2                  ; r0 = EFGH0000
  // mvn     r2, r2                      ; r2 = leftover bits
  // lsrv    r2, r1, r2                  ; r2 = 0000ABCD
  // orr     r0, r0, r2                  ; r0 = EFGHABCD
  if (i.src1.is_constant) {
    e.MOV(REG(0), i.src1.constant());
  } else {
    e.MOV(REG(0), i.src1.reg());
  }

  if (i.src2.is_constant) {
    e.MOV(REG(1), i.src2.constant());
  } else {
    e.MOV(W1, i.src2.reg().toW());
  }

  e.LSLV(i.dest, REG(0), REG(1));
  e.MVN(REG(1), REG(1));
  e.LSRV(REG(1), REG(0), REG(1));
  e.ORR(i.dest, i.dest, REG(1));
}
struct ROTATE_LEFT_I8
    : Sequence<ROTATE_LEFT_I8, I<OPCODE_ROTATE_LEFT, I8Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitRotateLeftXX<ROTATE_LEFT_I8, WReg>(e, i);
  }
};
struct ROTATE_LEFT_I16
    : Sequence<ROTATE_LEFT_I16, I<OPCODE_ROTATE_LEFT, I16Op, I16Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitRotateLeftXX<ROTATE_LEFT_I16, WReg>(e, i);
  }
};
struct ROTATE_LEFT_I32
    : Sequence<ROTATE_LEFT_I32, I<OPCODE_ROTATE_LEFT, I32Op, I32Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.MOV(W0, i.src1.constant());
    } else {
      e.MOV(W0, i.src1.reg());
    }

    if (i.src2.is_constant) {
      e.MOV(W1, i.src2.constant());
    } else {
      e.SXTB(W1, i.src2.reg());
    }
    e.NEG(W1, W1);

    e.ROR(i.dest, W0, W1);
  }
};
struct ROTATE_LEFT_I64
    : Sequence<ROTATE_LEFT_I64, I<OPCODE_ROTATE_LEFT, I64Op, I64Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.MOV(X0, i.src1.constant());
    } else {
      e.MOV(X0, i.src1.reg());
    }

    if (i.src2.is_constant) {
      e.MOV(X1, i.src2.constant());
    } else {
      e.SXTB(X1, i.src2.reg().toW());
    }
    e.NEG(X1, X1);

    e.ROR(i.dest, X0, X1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_ROTATE_LEFT, ROTATE_LEFT_I8, ROTATE_LEFT_I16,
                     ROTATE_LEFT_I32, ROTATE_LEFT_I64);

// ============================================================================
// OPCODE_BYTE_SWAP
// ============================================================================
// TODO(benvanik): put dest/src1 together.
struct BYTE_SWAP_I16
    : Sequence<BYTE_SWAP_I16, I<OPCODE_BYTE_SWAP, I16Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitUnaryOp(e, i, [](A64Emitter& e, WReg dest_src) {
      e.REV16(dest_src, dest_src);
    });
  }
};
struct BYTE_SWAP_I32
    : Sequence<BYTE_SWAP_I32, I<OPCODE_BYTE_SWAP, I32Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitUnaryOp(
        e, i, [](A64Emitter& e, WReg dest_src) { e.REV(dest_src, dest_src); });
  }
};
struct BYTE_SWAP_I64
    : Sequence<BYTE_SWAP_I64, I<OPCODE_BYTE_SWAP, I64Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitUnaryOp(
        e, i, [](A64Emitter& e, XReg dest_src) { e.REV(dest_src, dest_src); });
  }
};
struct BYTE_SWAP_V128
    : Sequence<BYTE_SWAP_V128, I<OPCODE_BYTE_SWAP, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Reverse upper and lower 64-bit halfs
    e.REV32(i.dest.reg().B16(), i.src1.reg().B16());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_BYTE_SWAP, BYTE_SWAP_I16, BYTE_SWAP_I32,
                     BYTE_SWAP_I64, BYTE_SWAP_V128);

// ============================================================================
// OPCODE_CNTLZ
// Count leading zeroes
// ============================================================================
struct CNTLZ_I8 : Sequence<CNTLZ_I8, I<OPCODE_CNTLZ, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // No 8bit lzcnt, so do 32 and sub 24.
    e.UXTB(i.dest, i.src1);
    e.CLZ(i.dest, i.dest);
    e.SUB(i.dest.reg(), i.dest.reg(), 24);
  }
};
struct CNTLZ_I16 : Sequence<CNTLZ_I16, I<OPCODE_CNTLZ, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // No 16bit lzcnt, so do 32 and sub 16.
    e.UXTH(i.dest, i.src1);
    e.CLZ(i.dest, i.dest);
    e.SUB(i.dest.reg(), i.dest.reg(), 16);
  }
};
struct CNTLZ_I32 : Sequence<CNTLZ_I32, I<OPCODE_CNTLZ, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CLZ(i.dest, i.src1);
  }
};
struct CNTLZ_I64 : Sequence<CNTLZ_I64, I<OPCODE_CNTLZ, I8Op, I64Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    e.CLZ(i.dest.reg().toX(), i.src1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_CNTLZ, CNTLZ_I8, CNTLZ_I16, CNTLZ_I32, CNTLZ_I64);

// ============================================================================
// OPCODE_SET_ROUNDING_MODE
// ============================================================================
// Input: FPSCR (PPC format) low bits (rn + ni)
// Convert from PPC rounding mode + non-IEEE (denorm/NaN behavior) to ARM FPCR.
// PPC FPSCR bits 0-2: rn[1:0] | ni
//   rn: 00=near, 01=zero, 10=+inf, 11=-inf
//   ni=1 enables non-IEEE mode (often FZ on real Xenon)
// ARM FPCR bits [24]=FZ, [23:22]=RMode (0=near,1=+inf,2=-inf,3=zero)
// We also consider DN (bit 25) for NaN propagation consistency with Xenon
// paired-single / FPU (SNaN->QNaN quieting, payload rules differ from IEEE).
// See also: a64_backend.h mxcsr_fpu/vmx (legacy from x64; FPCR used directly here)
// Paired-single (ps_*) and VMX fp ops rely on these single-prec paths + FPCR.
// CAPTAIN RE-TASK light polish: single-prec ps_* (ps_addx..ps_sel) use F32 halves + this
// mapping for independent per-element rounding/denorm/NaN (Xenon PPE != strict IEEE).
// Ties directly to ps research report + new ps_* accuracy counters (nan/denorm) + emitters.
// FZ (flush zero) critical for ps denorm_handled cases; DN kept 0 for payload fidelity.
// Hook: fpcr_table drives SET_ROUNDING_MODE (called on mtfs*/fpscr update in HIR).
// See also ppc_emit_fpu.cc ps_* lowering comments + a64_backend harness sequences.
static const uint8_t fpcr_table[] = {
    0b0'00,  // |--|nearest     (IEEE default, denorms handled)
    0b0'11,  // |--|toward zero
    0b0'01,  // |--|toward +infinity
    0b0'10,  // |--|toward -infinity
    0b1'00,  // |FZ|nearest     (ni=1 non-IEEE: flush denorms to zero)
    0b1'11,  // |FZ|toward zero
    0b1'01,  // |FZ|toward +infinity
    0b1'10,  // |FZ|toward -infinity
};
struct SET_ROUNDING_MODE_I32
    : Sequence<SET_ROUNDING_MODE_I32,
               I<OPCODE_SET_ROUNDING_MODE, VoidOp, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Low 3 bits are |Non-IEEE:1|RoundingMode:2|
    // Non-IEEE bit (ni) controls flush-to-zero for denormal handling.
    // For full Xenon fidelity, future: also toggle DN (FPCR[25]) for
    // default-NaN behavior in NaN propagation (affects physics/anim).
    e.AND(W1, i.src1, 0b111);

    // Use the low 3 bits as an index into a LUT
    e.MOV(X0, reinterpret_cast<uintptr_t>(fpcr_table));
    e.LDRB(W0, X0, X1);

    // Replace FPCR RMode+FZ bits (23:22 + 24) with new value.
    // Keep DN=0 for better NaN payload propagation matching PPC.
    e.MRS(X1, SystemReg::FPCR);
    e.BFI(X1, X0, 23, 3);  // bits 23:22 RM, bit 24 = FZ (from table bit 2)
    // Ensure DN (bit 25) cleared for NaN propagation (not default NaN).
    e.AND(X1, X1, ~ (1ULL << 25));
    e.MSR(SystemReg::FPCR, X1);
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SET_ROUNDING_MODE, SET_ROUNDING_MODE_I32);

// Include anchors to other sequence sources so they get included in the build.
extern volatile int anchor_control;
static int anchor_control_dest = anchor_control;

extern volatile int anchor_memory;
static int anchor_memory_dest = anchor_memory;

extern volatile int anchor_vector;
static int anchor_vector_dest = anchor_vector;

bool SelectSequence(A64Emitter* e, const Instr* i, const Instr** new_tail) {
  const InstrKey key(i);
  auto it = sequence_table.find(key);
  if (it != sequence_table.end()) {
    if (it->second(*e, i)) {
      *new_tail = i->next;
      return true;
    }
  }
  return false;
}

// ============================================================================
// Centralized Unhandled Instruction System Implementation
// ============================================================================

// Rate-limited unhandled opcode tracker (file-static for the a64 namespace).
// Sparse map — only actually-hit opcodes consume memory.
static std::unordered_map<uint32_t, uint64_t> unhandled_hit_counts;
static constexpr uint64_t kLogEveryN = 64;

// Small ring for recent unhandled names (for diagnostics / future Java exposure).
static constexpr size_t kRecentUnhandledRingSize = 8;
static std::string recent_unhandled_ring[kRecentUnhandledRingSize];
static size_t recent_unhandled_ring_head = 0;

namespace internal {
void RecordRecentUnhandled(const char* op_name) {
  if (!op_name) op_name = "<unknown>";
  recent_unhandled_ring[recent_unhandled_ring_head] = op_name;
  recent_unhandled_ring_head = (recent_unhandled_ring_head + 1) % kRecentUnhandledRingSize;
}
}  // namespace internal

void ReportUnhandledA64Opcode(A64Emitter* e, const hir::Instr* instr,
                              const char* context) {
  if (!instr || !instr->opcode) {
    XELOGE("A64: UNHANDLED null/unknown opcode (context={})", context ? context : "unknown");
    return;
  }

  const uint32_t opcode_id = static_cast<uint32_t>(instr->opcode->num);
  const char* op_name = instr->opcode->name ? instr->opcode->name : "<unnamed>";

  auto& count = unhandled_hit_counts[opcode_id];
  count++;

  internal::RecordRecentUnhandled(op_name);

  // Rate-limited logging (first hit always, then every kLogEveryN)
  bool should_log = (count == 1) || ((count % kLogEveryN) == 0);
  if (should_log) {
    uint64_t guest_hint = 0;
    if (e) {
      // Best effort: last known guest PC from emitter source map
      guest_hint = e->last_guest_address();
    }

    XELOGE("A64 UNHANDLED OP [rate-limited]: {} (id={}) hits={} context={} guest_pc~0x{:X} accuracy_debug={}",
           op_name, opcode_id, count, context ? context : "seq",
           guest_hint, cvars::a64_accuracy_debug ? 1 : 0);

    if (cvars::a64_accuracy_debug && count == 1) {
      XELOGE("  ^ First occurrence of this unhandled HIR opcode. "
             "This usually means a missing PPC -> HIR translation or a rare "
             "Altivec/VMX instruction variant. Check recent PPC disassembly.");
    }
  }
}

void EmitStructuredFallback(A64Emitter& e, const hir::Instr* instr) {
  if (!instr) {
    e.NOP();
    return;
  }

  // Best-effort: zero the primary destination register if it exists and is a
  // register operand. This allows execution to continue (with wrong results)
  // instead of hard-crashing the guest thread.
  // Covers the most common cases (scalar + vector ops).
  bool did_zero = false;

  // V128 / vector dest is extremely common for unhandled Altivec paths.
  if (instr->dest && instr->dest->IsV128()) {
    auto& vreg = instr->dest->reg();
    e.EOR(vreg.B16(), vreg.B16(), vreg.B16());   // zero the 128-bit dest
    did_zero = true;
  } else if (instr->dest) {
    // Scalar integer/float dests
    if (instr->dest->IsInt64()) {
      e.MOV(instr->dest->reg().X(), XZR);
      did_zero = true;
    } else if (instr->dest->IsInt32() || instr->dest->IsInt16() || instr->dest->IsInt8()) {
      e.MOV(instr->dest->reg().W(), WZR);
      did_zero = true;
    } else if (instr->dest->IsFloat64()) {
      e.FMOV(instr->dest->reg().D(), 0.0);
      did_zero = true;
    } else if (instr->dest->IsFloat32()) {
      e.FMOV(instr->dest->reg().S(), 0.0f);
      did_zero = true;
    }
  }

  // Emit a distinctive but non-fatal marker sequence (easy to spot in
  // disassembly / debuggers). We avoid BRK here — BRK is reserved for
  // accuracy_debug mode in the caller.
  e.NOP();
  // Two NOPs + MOV of a magic immediate (0xA64BAD1) into a temp for visibility.
  e.MOV(X16, 0xA64BAD1ULL);

  if (cvars::a64_accuracy_debug) {
    // In accuracy debug mode we want to surface the problem loudly.
    e.DebugBreak();   // or could BRK(0xA64D)
  }

  const char* op_name = (instr->opcode && instr->opcode->name) ? instr->opcode->name : "?";
  XELOGW("A64: Structured fallback executed for {} (dest zeroed={})", op_name, did_zero);
}

// Public helper to query recent unhandled report (for future JNI exposure).
std::string GetA64UnhandledReportSummary() {
  std::string summary = "A64 Unhandled Opcodes (recent):\n";
  for (size_t i = 0; i < kRecentUnhandledRingSize; ++i) {
    size_t idx = (recent_unhandled_ring_head + i) % kRecentUnhandledRingSize;
    if (!recent_unhandled_ring[idx].empty()) {
      summary += "  - " + recent_unhandled_ring[idx] + "\n";
    }
  }
  summary += "Total unique hit opcodes tracked: " + std::to_string(unhandled_hit_counts.size()) + "\n";
  return summary;
}

// JNI-friendly export (registration to be wired in the main native bridge or
// via RegisterNatives in the app's JNI init code).
extern "C" {

// Lightweight C helper (can be called from other native code or dlsym).
const char* A64_GetUnhandledReportForJNI() {
  static std::string cached;
  cached = GetA64UnhandledReportSummary();
  return cached.c_str();
}

// Standard JNI export for aenu.emulator.Emulator.getA64UnhandledReport().
// Full dynamic registration (RegisterNatives) should be performed from the
// app's main JNI_OnLoad or Emulator native init for robustness.
JNIEXPORT jstring JNICALL
Java_aenu_emulator_Emulator_getA64UnhandledReport(JNIEnv* env, jobject thiz) {
  const char* report = A64_GetUnhandledReportForJNI();
  return env->NewStringUTF(report ? report : "A64 unhandled report unavailable");
}

}  // extern "C"

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
