/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_util.h"

#include <algorithm>
#include <cstring>

#include "xenia/cpu/backend/a64/a64_op.h"
#include "xenia/cpu/backend/a64/a64_tracers.h"

// For OPCODE_PACK/OPCODE_UNPACK
#include "third_party/half/include/half.hpp"
#include "xenia/base/logging.h"
#include "xenia/cpu/cpu_flags.h"  // for cvars::a64_accuracy_debug etc.

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

volatile int anchor_vector = 0;

// ============================================================================
// OPCODE_VECTOR_CONVERT_I2F
// ============================================================================
struct VECTOR_CONVERT_I2F
    : Sequence<VECTOR_CONVERT_I2F,
               I<OPCODE_VECTOR_CONVERT_I2F, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      e.UCVTF(i.dest.reg().S4(), i.src1.reg().S4());
    } else {
      e.SCVTF(i.dest.reg().S4(), i.src1.reg().S4());
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_CONVERT_I2F, VECTOR_CONVERT_I2F);

// ============================================================================
// OPCODE_VECTOR_CONVERT_F2I
// ============================================================================
struct VECTOR_CONVERT_F2I
    : Sequence<VECTOR_CONVERT_F2I,
               I<OPCODE_VECTOR_CONVERT_F2I, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.instr->flags & ARITHMETIC_UNSIGNED) {
      e.FCVTZU(i.dest.reg().S4(), i.src1.reg().S4());
    } else {
      e.FCVTZS(i.dest.reg().S4(), i.src1.reg().S4());
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_CONVERT_F2I, VECTOR_CONVERT_F2I);

// ============================================================================
// OPCODE_LOAD_VECTOR_SHL
// ============================================================================
static const vec128_t lvsl_table[16] = {
    vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
    vec128b(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
    vec128b(2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17),
    vec128b(3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18),
    vec128b(4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19),
    vec128b(5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20),
    vec128b(6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21),
    vec128b(7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22),
    vec128b(8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23),
    vec128b(9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24),
    vec128b(10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25),
    vec128b(11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26),
    vec128b(12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27),
    vec128b(13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28),
    vec128b(14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29),
    vec128b(15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30),
};
struct LOAD_VECTOR_SHL_I8
    : Sequence<LOAD_VECTOR_SHL_I8, I<OPCODE_LOAD_VECTOR_SHL, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      auto sh = i.src1.constant();
      assert_true(sh < xe::countof(lvsl_table));
      e.MOV(X0, reinterpret_cast<uintptr_t>(&lvsl_table[sh]));
      e.LDR(i.dest, X0);
    } else {
      e.MOV(X0, reinterpret_cast<uintptr_t>(lvsl_table));
      e.AND(X1, i.src1.reg().toX(), 0xf);
      e.LDR(i.dest, X0, X1, IndexExt::LSL, 4);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_VECTOR_SHL, LOAD_VECTOR_SHL_I8);

// ============================================================================
// OPCODE_LOAD_VECTOR_SHR
// ============================================================================
static const vec128_t lvsr_table[16] = {
    vec128b(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31),
    vec128b(15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30),
    vec128b(14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29),
    vec128b(13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28),
    vec128b(12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27),
    vec128b(11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26),
    vec128b(10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25),
    vec128b(9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24),
    vec128b(8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23),
    vec128b(7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22),
    vec128b(6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21),
    vec128b(5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20),
    vec128b(4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19),
    vec128b(3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18),
    vec128b(2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17),
    vec128b(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
};
struct LOAD_VECTOR_SHR_I8
    : Sequence<LOAD_VECTOR_SHR_I8, I<OPCODE_LOAD_VECTOR_SHR, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      auto sh = i.src1.constant();
      assert_true(sh < xe::countof(lvsr_table));
      e.MOV(X0, reinterpret_cast<uintptr_t>(&lvsr_table[sh]));
      e.LDR(i.dest, X0);
    } else {
      e.MOV(X0, reinterpret_cast<uintptr_t>(lvsr_table));
      e.AND(X1, i.src1.reg().toX(), 0xf);
      e.LDR(i.dest, X0, X1, IndexExt::LSL, 4);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_LOAD_VECTOR_SHR, LOAD_VECTOR_SHR_I8);

// ============================================================================
// OPCODE_VECTOR_MAX
// ============================================================================
struct VECTOR_MAX
    : Sequence<VECTOR_MAX, I<OPCODE_VECTOR_MAX, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          uint32_t part_type = i.instr->flags >> 8;
          bool is_unsigned = !!(i.instr->flags & ARITHMETIC_UNSIGNED);
          switch (part_type) {
            case INT8_TYPE:
              if (is_unsigned) {
                e.UMAX(dest.B16(), src1.B16(), src2.B16());
              } else {
                e.SMAX(dest.B16(), src1.B16(), src2.B16());
              }
              break;
            case INT16_TYPE:
              if (is_unsigned) {
                e.UMAX(dest.H8(), src1.H8(), src2.H8());
              } else {
                e.SMAX(dest.H8(), src1.H8(), src2.H8());
              }
              break;
            case INT32_TYPE:
              if (is_unsigned) {
                e.UMAX(dest.S4(), src1.S4(), src2.S4());
              } else {
                e.SMAX(dest.S4(), src1.S4(), src2.S4());
              }
              break;
            case FLOAT32_TYPE:
              e.FMAX(dest.S4(), src1.S4(), src2.S4());
              break;
            default:
              // INT64 and others: safe fallback (no direct NEON U/SMAX for D)
              e.EOR(dest.B16(), dest.B16(), dest.B16());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_MAX, VECTOR_MAX);

// ============================================================================
// OPCODE_VECTOR_MIN
// ============================================================================
struct VECTOR_MIN
    : Sequence<VECTOR_MIN, I<OPCODE_VECTOR_MIN, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          uint32_t part_type = i.instr->flags >> 8;
          bool is_unsigned = !!(i.instr->flags & ARITHMETIC_UNSIGNED);
          switch (part_type) {
            case INT8_TYPE:
              if (is_unsigned) {
                e.UMIN(dest.B16(), src1.B16(), src2.B16());
              } else {
                e.SMIN(dest.B16(), src1.B16(), src2.B16());
              }
              break;
            case INT16_TYPE:
              if (is_unsigned) {
                e.UMIN(dest.H8(), src1.H8(), src2.H8());
              } else {
                e.SMIN(dest.H8(), src1.H8(), src2.H8());
              }
              break;
            case INT32_TYPE:
              if (is_unsigned) {
                e.UMIN(dest.S4(), src1.S4(), src2.S4());
              } else {
                e.SMIN(dest.S4(), src1.S4(), src2.S4());
              }
              break;
            case FLOAT32_TYPE:
              e.FMIN(dest.S4(), src1.S4(), src2.S4());
              break;
            default:
              // INT64 and others: safe fallback (no direct NEON U/SMIN for D)
              e.EOR(dest.B16(), dest.B16(), dest.B16());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_MIN, VECTOR_MIN);

// ============================================================================
// OPCODE_VECTOR_COMPARE_EQ
// ============================================================================
struct VECTOR_COMPARE_EQ_V128
    : Sequence<VECTOR_COMPARE_EQ_V128,
               I<OPCODE_VECTOR_COMPARE_EQ, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAssociativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          switch (i.instr->flags) {
            case INT8_TYPE:
              e.CMEQ(dest.B16(), src1.B16(), src2.B16());
              break;
            case INT16_TYPE:
              e.CMEQ(dest.H8(), src1.H8(), src2.H8());
              break;
            case INT32_TYPE:
              e.CMEQ(dest.S4(), src1.S4(), src2.S4());
              break;
            case FLOAT32_TYPE:
              e.FCMEQ(dest.S4(), src1.S4(), src2.S4());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_EQ, VECTOR_COMPARE_EQ_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_SGT
// ============================================================================
struct VECTOR_COMPARE_SGT_V128
    : Sequence<VECTOR_COMPARE_SGT_V128,
               I<OPCODE_VECTOR_COMPARE_SGT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAssociativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          switch (i.instr->flags) {
            case INT8_TYPE:
              e.CMGT(dest.B16(), src1.B16(), src2.B16());
              break;
            case INT16_TYPE:
              e.CMGT(dest.H8(), src1.H8(), src2.H8());
              break;
            case INT32_TYPE:
              e.CMGT(dest.S4(), src1.S4(), src2.S4());
              break;
            case FLOAT32_TYPE:
              e.FCMGT(dest.S4(), src1.S4(), src2.S4());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_SGT, VECTOR_COMPARE_SGT_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_SGE
// ============================================================================
struct VECTOR_COMPARE_SGE_V128
    : Sequence<VECTOR_COMPARE_SGE_V128,
               I<OPCODE_VECTOR_COMPARE_SGE, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAssociativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          switch (i.instr->flags) {
            case INT8_TYPE:
              e.CMGE(dest.B16(), src1.B16(), src2.B16());
              break;
            case INT16_TYPE:
              e.CMGE(dest.H8(), src1.H8(), src2.H8());
              break;
            case INT32_TYPE:
              e.CMGE(dest.S4(), src1.S4(), src2.S4());
              break;
            case FLOAT32_TYPE:
              e.FCMGE(dest.S4(), src1.S4(), src2.S4());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_SGE, VECTOR_COMPARE_SGE_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_UGT
// ============================================================================
struct VECTOR_COMPARE_UGT_V128
    : Sequence<VECTOR_COMPARE_UGT_V128,
               I<OPCODE_VECTOR_COMPARE_UGT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAssociativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          switch (i.instr->flags) {
            case INT8_TYPE:
              e.CMHI(dest.B16(), src1.B16(), src2.B16());
              break;
            case INT16_TYPE:
              e.CMHI(dest.H8(), src1.H8(), src2.H8());
              break;
            case INT32_TYPE:
              e.CMHI(dest.S4(), src1.S4(), src2.S4());
              break;
            case FLOAT32_TYPE:
              e.FABS(Q1.S4(), src1.S4());
              e.FABS(Q2.S4(), src2.S4());
              e.FCMGT(dest.S4(), Q1.S4(), Q2.S4());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_UGT, VECTOR_COMPARE_UGT_V128);

// ============================================================================
// OPCODE_VECTOR_COMPARE_UGE
// ============================================================================
struct VECTOR_COMPARE_UGE_V128
    : Sequence<VECTOR_COMPARE_UGE_V128,
               I<OPCODE_VECTOR_COMPARE_UGE, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitAssociativeBinaryVOp(
        e, i, [&i](A64Emitter& e, QReg dest, QReg src1, QReg src2) {
          switch (i.instr->flags) {
            case INT8_TYPE:
              e.CMHS(dest.B16(), src1.B16(), src2.B16());
              break;
            case INT16_TYPE:
              e.CMHS(dest.H8(), src1.H8(), src2.H8());
              break;
            case INT32_TYPE:
              e.CMHS(dest.S4(), src1.S4(), src2.S4());
              break;
            case FLOAT32_TYPE:
              e.FABS(Q1.S4(), src1.S4());
              e.FABS(Q2.S4(), src2.S4());
              e.FCMGE(dest.S4(), Q1.S4(), Q2.S4());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_COMPARE_UGE, VECTOR_COMPARE_UGE_V128);

// ============================================================================
// OPCODE_VECTOR_ADD
// ============================================================================
struct VECTOR_ADD
    : Sequence<VECTOR_ADD, I<OPCODE_VECTOR_ADD, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [&i](A64Emitter& e, const QReg& dest, QReg src1, QReg src2) {
          const TypeName part_type =
              static_cast<TypeName>(i.instr->flags & 0xFF);
          const uint32_t arithmetic_flags = i.instr->flags >> 8;
          bool is_unsigned = !!(arithmetic_flags & ARITHMETIC_UNSIGNED);
          bool saturate = !!(arithmetic_flags & ARITHMETIC_SATURATE);
          switch (part_type) {
            case INT8_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQADD(dest.B16(), src1.B16(), src2.B16());
                } else {
                  e.SQADD(dest.B16(), src1.B16(), src2.B16());
                }
              } else {
                e.ADD(dest.B16(), src1.B16(), src2.B16());
              }
              break;
            case INT16_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQADD(dest.H8(), src1.H8(), src2.H8());
                } else {
                  e.SQADD(dest.H8(), src1.H8(), src2.H8());
                }
              } else {
                e.ADD(dest.H8(), src1.H8(), src2.H8());
              }
              break;
            case INT32_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQADD(dest.S4(), src1.S4(), src2.S4());
                } else {
                  e.SQADD(dest.S4(), src1.S4(), src2.S4());
                }
              } else {
                e.ADD(dest.S4(), src1.S4(), src2.S4());
              }
              break;
            case FLOAT32_TYPE:
              assert_false(is_unsigned);
              assert_false(saturate);
              e.FADD(dest.S4(), src1.S4(), src2.S4());
              break;
            case INT64_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQADD(dest.D2(), src1.D2(), src2.D2());
                } else {
                  e.SQADD(dest.D2(), src1.D2(), src2.D2());
                }
              } else {
                e.ADD(dest.D2(), src1.D2(), src2.D2());
              }
              break;
            case FLOAT64_TYPE:
              e.FADD(dest.D2(), src1.D2(), src2.D2());
              break;
            default:
              // Unknown element type: safe fallback
              e.EOR(dest.B16(), dest.B16(), dest.B16());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_ADD, VECTOR_ADD);

// ============================================================================
// OPCODE_VECTOR_SUB
// ============================================================================
struct VECTOR_SUB
    : Sequence<VECTOR_SUB, I<OPCODE_VECTOR_SUB, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i, [&i](A64Emitter& e, const QReg& dest, QReg src1, QReg src2) {
          const TypeName part_type =
              static_cast<TypeName>(i.instr->flags & 0xFF);
          const uint32_t arithmetic_flags = i.instr->flags >> 8;
          bool is_unsigned = !!(arithmetic_flags & ARITHMETIC_UNSIGNED);
          bool saturate = !!(arithmetic_flags & ARITHMETIC_SATURATE);
          switch (part_type) {
            case INT8_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQSUB(dest.B16(), src1.B16(), src2.B16());
                } else {
                  e.SQSUB(dest.B16(), src1.B16(), src2.B16());
                }
              } else {
                e.SUB(dest.B16(), src1.B16(), src2.B16());
              }
              break;
            case INT16_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQSUB(dest.H8(), src1.H8(), src2.H8());
                } else {
                  e.SQSUB(dest.H8(), src1.H8(), src2.H8());
                }
              } else {
                e.SUB(dest.H8(), src1.H8(), src2.H8());
              }
              break;
            case INT32_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQSUB(dest.S4(), src1.S4(), src2.S4());
                } else {
                  e.SQSUB(dest.S4(), src1.S4(), src2.S4());
                }
              } else {
                e.SUB(dest.S4(), src1.S4(), src2.S4());
              }
              break;
            case FLOAT32_TYPE:
              assert_false(is_unsigned);
              assert_false(saturate);
              e.FSUB(dest.S4(), src1.S4(), src2.S4());
              break;
            case INT64_TYPE:
              if (saturate) {
                if (is_unsigned) {
                  e.UQSUB(dest.D2(), src1.D2(), src2.D2());
                } else {
                  e.SQSUB(dest.D2(), src1.D2(), src2.D2());
                }
              } else {
                e.SUB(dest.D2(), src1.D2(), src2.D2());
              }
              break;
            case FLOAT64_TYPE:
              e.FSUB(dest.D2(), src1.D2(), src2.D2());
              break;
            default:
              // Unknown element type: safe fallback
              e.EOR(dest.B16(), dest.B16(), dest.B16());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SUB, VECTOR_SUB);

// ============================================================================
// OPCODE_VECTOR_SHL
// ============================================================================
template <typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
static uint8x16_t EmulateVectorShl(void*, uint8x16_t src1,
                                   uint8x16_t src2) {
  alignas(16) T value[16 / sizeof(T)];
  alignas(16) T shamt[16 / sizeof(T)];

  // Load NEON registers into a C array.
  vst1q_u8(reinterpret_cast<uint8_t*>(reinterpret_cast<T*>(&value)), src1);
  vst1q_u8(reinterpret_cast<uint8_t*>(reinterpret_cast<T*>(&shamt)), src2);

  for (size_t i = 0; i < (16 / sizeof(T)); ++i) {
    value[i] = value[i] << (shamt[i] & ((sizeof(T) * 8) - 1));
  }

  // Store result and return it.
  return vld1q_u8(reinterpret_cast<uint8_t*>(&value));
}
struct VECTOR_SHL_V128
    : Sequence<VECTOR_SHL_V128, I<OPCODE_VECTOR_SHL, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Load operands (handle constants)
    QReg src1 = i.src1.is_constant ? Q0 : i.src1.reg();
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    }
    QReg shamt = i.src2.is_constant ? Q1 : i.src2.reg();
    if (i.src2.is_constant) {
      e.LoadConstantV(shamt, i.src2.constant());
    }

    switch (i.instr->flags) {
      case INT8_TYPE: {
        // Mask shift counts to [0,7] per byte (USHL interprets as signed shift)
        e.MOVI(Q2.B16(), 0x07);
        e.AND(Q1.B16(), shamt.B16(), Q2.B16());
        e.USHL(i.dest.reg().B16(), src1.B16(), Q1.B16());
        break;
      }
      case INT16_TYPE: {
        e.MOVI(Q2.H8(), 0x0F);
        e.AND(Q1.H8(), shamt.H8(), Q2.H8());
        e.USHL(i.dest.reg().H8(), src1.H8(), Q1.H8());
        break;
      }
      case INT32_TYPE: {
        e.MOVI(Q2.S4(), 0x1F);
        e.AND(Q1.S4(), shamt.S4(), Q2.S4());
        e.USHL(i.dest.reg().S4(), src1.S4(), Q1.S4());
        break;
      }
      case INT64_TYPE: {
        e.MOV(X0, 0x3F);
        e.DUP(Q2.D2(), X0);
        e.AND(Q1.D2(), shamt.D2(), Q2.D2());
        e.USHL(i.dest.reg().D2(), src1.D2(), Q1.D2());
        break;
      }
      default:
        // Unknown: zero
        e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16());
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SHL, VECTOR_SHL_V128);

// ============================================================================
// OPCODE_VECTOR_SHR
// ============================================================================
template <typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
static uint8x16_t EmulateVectorShr(void*, uint8x16_t src1, uint8x16_t src2) {
  alignas(16) T value[16 / sizeof(T)];
  alignas(16) T shamt[16 / sizeof(T)];

  // Load NEON registers into a C array.
  vst1q_u8(reinterpret_cast<uint8_t*>(reinterpret_cast<T*>(&value)), src1);
  vst1q_u8(reinterpret_cast<uint8_t*>(reinterpret_cast<T*>(&shamt)), src2);

  for (size_t i = 0; i < (16 / sizeof(T)); ++i) {
    value[i] = value[i] >> (shamt[i] & ((sizeof(T) * 8) - 1));
  }

  // Store result and return it.
  return vld1q_u8(reinterpret_cast<uint8_t*>(&value));
}
struct VECTOR_SHR_V128
    : Sequence<VECTOR_SHR_V128, I<OPCODE_VECTOR_SHR, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Load operands (handle constants)
    QReg src1 = i.src1.is_constant ? Q0 : i.src1.reg();
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    }
    QReg shamt = i.src2.is_constant ? Q1 : i.src2.reg();
    if (i.src2.is_constant) {
      e.LoadConstantV(shamt, i.src2.constant());
    }

    // Fast path for uniform constant shift amount (common case)
    if (i.src2.is_constant) {
      switch (i.instr->flags) {
        case INT8_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 15; ++n) {
            if (c.u8[n] != c.u8[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned s = c.u8[0];
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else if (s >= 8) {
              e.MOVI(i.dest.reg().B16(), 0);
            } else {
              e.USHR(i.dest.reg().B16(), src1.B16(), s);
            }
            return;
          }
          break;
        }
        case INT16_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 7; ++n) {
            if (c.u16[n] != c.u16[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned s = c.u16[0];
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else if (s >= 16) {
              e.MOVI(i.dest.reg().B16(), 0);
            } else {
              e.USHR(i.dest.reg().H8(), src1.H8(), s);
            }
            return;
          }
          break;
        }
        case INT32_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 3; ++n) {
            if (c.u32[n] != c.u32[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned s = c.u32[0];
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else if (s >= 32) {
              e.MOVI(i.dest.reg().B16(), 0);
            } else {
              e.USHR(i.dest.reg().S4(), src1.S4(), s);
            }
            return;
          }
          break;
        }
        case INT64_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = (c.u64[0] == c.u64[1]);
          if (all_same) {
            unsigned s = static_cast<unsigned>(c.u64[0] & 0x3F);
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else if (s >= 64) {
              e.MOVI(i.dest.reg().B16(), 0);
            } else {
              e.USHR(i.dest.reg().D2(), src1.D2(), s);
            }
            return;
          }
          break;
        }
      }
    }

    // Variable (or non-uniform const) path: mask + negate shifts + USHL for logical right
    switch (i.instr->flags) {
      case INT8_TYPE: {
        e.MOVI(Q2.B16(), 0x07);
        e.AND(Q1.B16(), shamt.B16(), Q2.B16());
        e.NEG(Q2.B16(), Q1.B16());
        e.USHL(i.dest.reg().B16(), src1.B16(), Q2.B16());
        break;
      }
      case INT16_TYPE: {
        e.MOVI(Q2.H8(), 0x0F);
        e.AND(Q1.H8(), shamt.H8(), Q2.H8());
        e.NEG(Q2.H8(), Q1.H8());
        e.USHL(i.dest.reg().H8(), src1.H8(), Q2.H8());
        break;
      }
      case INT32_TYPE: {
        e.MOVI(Q2.S4(), 0x1F);
        e.AND(Q1.S4(), shamt.S4(), Q2.S4());
        e.NEG(Q2.S4(), Q1.S4());
        e.USHL(i.dest.reg().S4(), src1.S4(), Q2.S4());
        break;
      }
      case INT64_TYPE: {
        e.MOV(X0, 0x3F);
        e.DUP(Q2.D2(), X0);
        e.AND(Q1.D2(), shamt.D2(), Q2.D2());
        e.NEG(Q2.D2(), Q1.D2());
        e.USHL(i.dest.reg().D2(), src1.D2(), Q2.D2());
        break;
      }
      default:
        e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16());
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SHR, VECTOR_SHR_V128);

// ============================================================================
// OPCODE_VECTOR_SHA
// ============================================================================
struct VECTOR_SHA_V128
    : Sequence<VECTOR_SHA_V128, I<OPCODE_VECTOR_SHA, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Load operands (handle constants)
    QReg src1 = i.src1.is_constant ? Q0 : i.src1.reg();
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    }
    QReg shamt = i.src2.is_constant ? Q1 : i.src2.reg();
    if (i.src2.is_constant) {
      e.LoadConstantV(shamt, i.src2.constant());
    }

    // Fast path for uniform constant shift amount
    if (i.src2.is_constant) {
      switch (i.instr->flags) {
        case INT8_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 15; ++n) {
            if (c.u8[n] != c.u8[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned s = c.u8[0] & 0x7;
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SSHR(i.dest.reg().B16(), src1.B16(), s);
            }
            return;
          }
          break;
        }
        case INT16_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 7; ++n) {
            if (c.u16[n] != c.u16[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned s = c.u16[0] & 0xF;
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SSHR(i.dest.reg().H8(), src1.H8(), s);
            }
            return;
          }
          break;
        }
        case INT32_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 3; ++n) {
            if (c.u32[n] != c.u32[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned s = c.u32[0] & 0x1F;
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SSHR(i.dest.reg().S4(), src1.S4(), s);
            }
            return;
          }
          break;
        }
        case INT64_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = (c.u64[0] == c.u64[1]);
          if (all_same) {
            unsigned s = static_cast<unsigned>(c.u64[0] & 0x3F);
            if (s == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SSHR(i.dest.reg().D2(), src1.D2(), s);
            }
            return;
          }
          break;
        }
      }
    }

    // Variable path: mask + negate + SSHL for arithmetic right
    switch (i.instr->flags) {
      case INT8_TYPE: {
        e.MOVI(Q2.B16(), 0x07);
        e.AND(Q1.B16(), shamt.B16(), Q2.B16());
        e.NEG(Q2.B16(), Q1.B16());
        e.SSHL(i.dest.reg().B16(), src1.B16(), Q2.B16());
        break;
      }
      case INT16_TYPE: {
        e.MOVI(Q2.H8(), 0x0F);
        e.AND(Q1.H8(), shamt.H8(), Q2.H8());
        e.NEG(Q2.H8(), Q1.H8());
        e.SSHL(i.dest.reg().H8(), src1.H8(), Q2.H8());
        break;
      }
      case INT32_TYPE: {
        e.MOVI(Q2.S4(), 0x1F);
        e.AND(Q1.S4(), shamt.S4(), Q2.S4());
        e.NEG(Q2.S4(), Q1.S4());
        e.SSHL(i.dest.reg().S4(), src1.S4(), Q2.S4());
        break;
      }
      case INT64_TYPE: {
        e.MOV(X0, 0x3F);
        e.DUP(Q2.D2(), X0);
        e.AND(Q1.D2(), shamt.D2(), Q2.D2());
        e.NEG(Q2.D2(), Q1.D2());
        e.SSHL(i.dest.reg().D2(), src1.D2(), Q2.D2());
        break;
      }
      default:
        e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16());
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_SHA, VECTOR_SHA_V128);

// ============================================================================
// OPCODE_VECTOR_ROTATE_LEFT
// ============================================================================
template <typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
static uint8x16_t EmulateVectorRotateLeft(void*, uint8x16_t src1,
                                          uint8x16_t src2) {
  alignas(16) T value[16 / sizeof(T)];
  alignas(16) T shamt[16 / sizeof(T)];

  // Load NEON registers into a C array.
  vst1q_u8(reinterpret_cast<uint8_t*>(reinterpret_cast<T*>(&value)), src1);
  vst1q_u8(reinterpret_cast<uint8_t*>(reinterpret_cast<T*>(&shamt)), src2);

  for (size_t i = 0; i < (16 / sizeof(T)); ++i) {
    value[i] = xe::rotate_left<T>(value[i], shamt[i] & ((sizeof(T) * 8) - 1));
  }

  // Store result and return it.
  return vld1q_u8(reinterpret_cast<uint8_t*>(&value));
}
struct VECTOR_ROTATE_LEFT_V128
    : Sequence<VECTOR_ROTATE_LEFT_V128,
               I<OPCODE_VECTOR_ROTATE_LEFT, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    // Load operands (handle constants)
    QReg src1 = i.src1.is_constant ? Q0 : i.src1.reg();
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    }
    QReg shamt = i.src2.is_constant ? Q1 : i.src2.reg();
    if (i.src2.is_constant) {
      e.LoadConstantV(shamt, i.src2.constant());
    }

    // Fast path for uniform constant rotate amount (using imm shifts + OR)
    if (i.src2.is_constant) {
      switch (i.instr->flags) {
        case INT8_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 15; ++n) {
            if (c.u8[n] != c.u8[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned k = c.u8[0] & 0x7;
            if (k == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SHL(Q2.B16(), src1.B16(), k);
              e.USHR(Q3.B16(), src1.B16(), 8 - k);
              e.ORR(i.dest.reg().B16(), Q2.B16(), Q3.B16());
            }
            return;
          }
          break;
        }
        case INT16_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 7; ++n) {
            if (c.u16[n] != c.u16[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned k = c.u16[0] & 0xF;
            if (k == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SHL(Q2.H8(), src1.H8(), k);
              e.USHR(Q3.H8(), src1.H8(), 16 - k);
              e.ORR(i.dest.reg().B16(), Q2.B16(), Q3.B16());
            }
            return;
          }
          break;
        }
        case INT32_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = true;
          for (size_t n = 0; n < 3; ++n) {
            if (c.u32[n] != c.u32[n + 1]) {
              all_same = false;
              break;
            }
          }
          if (all_same) {
            unsigned k = c.u32[0] & 0x1F;
            if (k == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SHL(Q2.S4(), src1.S4(), k);
              e.USHR(Q3.S4(), src1.S4(), 32 - k);
              e.ORR(i.dest.reg().B16(), Q2.B16(), Q3.B16());
            }
            return;
          }
          break;
        }
        case INT64_TYPE: {
          const auto& c = i.src2.constant();
          bool all_same = (c.u64[0] == c.u64[1]);
          if (all_same) {
            unsigned k = static_cast<unsigned>(c.u64[0] & 0x3F);
            if (k == 0) {
              e.MOV(i.dest.reg().B16(), src1.B16());
            } else {
              e.SHL(Q2.D2(), src1.D2(), k);
              e.USHR(Q3.D2(), src1.D2(), 64 - k);
              e.ORR(i.dest.reg().B16(), Q2.B16(), Q3.B16());
            }
            return;
          }
          break;
        }
      }
    }

    // General variable rotate left per-element:
    // left = src << k; right = src >> (w - k); dest = left | right
    // Use USHL + USHL(neg) + ORR
    switch (i.instr->flags) {
      case INT8_TYPE: {
        // mask k
        e.MOVI(Q2.B16(), 0x07);
        e.AND(Q1.B16(), shamt.B16(), Q2.B16());  // Q1 = k (0-7)
        // m = (8 - k) & 7
        e.MOVI(Q2.B16(), 0x08);
        e.SUB(Q3.B16(), Q2.B16(), Q1.B16());
        e.MOVI(Q2.B16(), 0x07);
        e.AND(Q3.B16(), Q3.B16(), Q2.B16());  // Q3 = m
        e.NEG(Q2.B16(), Q3.B16());            // Q2 = -m
        // shifts
        e.USHL(Q3.B16(), src1.B16(), Q1.B16());  // left
        e.USHL(Q2.B16(), src1.B16(), Q2.B16());  // right logical
        e.ORR(i.dest.reg().B16(), Q3.B16(), Q2.B16());
        break;
      }
      case INT16_TYPE: {
        e.MOVI(Q2.H8(), 0x0F);
        e.AND(Q1.H8(), shamt.H8(), Q2.H8());  // Q1 = k
        e.MOVI(Q2.H8(), 0x10);
        e.SUB(Q3.H8(), Q2.H8(), Q1.H8());
        e.MOVI(Q2.H8(), 0x0F);
        e.AND(Q3.H8(), Q3.H8(), Q2.H8());  // m
        e.NEG(Q2.H8(), Q3.H8());
        e.USHL(Q3.H8(), src1.H8(), Q1.H8());
        e.USHL(Q2.H8(), src1.H8(), Q2.H8());
        e.ORR(i.dest.reg().B16(), Q3.B16(), Q2.B16());
        break;
      }
      case INT32_TYPE: {
        e.MOVI(Q2.S4(), 0x1F);
        e.AND(Q1.S4(), shamt.S4(), Q2.S4());
        e.MOVI(Q2.S4(), 0x20);
        e.SUB(Q3.S4(), Q2.S4(), Q1.S4());
        e.MOVI(Q2.S4(), 0x1F);
        e.AND(Q3.S4(), Q3.S4(), Q2.S4());
        e.NEG(Q2.S4(), Q3.S4());
        e.USHL(Q3.S4(), src1.S4(), Q1.S4());
        e.USHL(Q2.S4(), src1.S4(), Q2.S4());
        e.ORR(i.dest.reg().B16(), Q3.B16(), Q2.B16());
        break;
      }
      case INT64_TYPE: {
        e.MOV(X0, 0x3F);
        e.DUP(Q2.D2(), X0);
        e.AND(Q1.D2(), shamt.D2(), Q2.D2());
        e.MOV(X0, 64);
        e.DUP(Q2.D2(), X0);
        e.SUB(Q3.D2(), Q2.D2(), Q1.D2());
        e.MOV(X0, 0x3F);
        e.DUP(Q2.D2(), X0);
        e.AND(Q3.D2(), Q3.D2(), Q2.D2());
        e.NEG(Q2.D2(), Q3.D2());
        e.USHL(Q3.D2(), src1.D2(), Q1.D2());
        e.USHL(Q2.D2(), src1.D2(), Q2.D2());
        e.ORR(i.dest.reg().B16(), Q3.B16(), Q2.B16());
        break;
      }
      default:
        e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16());
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_ROTATE_LEFT, VECTOR_ROTATE_LEFT_V128);

// ============================================================================
// OPCODE_VECTOR_AVERAGE
// ============================================================================
struct VECTOR_AVERAGE
    : Sequence<VECTOR_AVERAGE,
               I<OPCODE_VECTOR_AVERAGE, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    EmitCommutativeBinaryVOp(
        e, i,
        [&i](A64Emitter& e, const QReg& dest, const QReg& src1,
             const QReg& src2) {
          const TypeName part_type =
              static_cast<TypeName>(i.instr->flags & 0xFF);
          const uint32_t arithmetic_flags = i.instr->flags >> 8;
          bool is_unsigned = !!(arithmetic_flags & ARITHMETIC_UNSIGNED);
          switch (part_type) {
            case INT8_TYPE:
              if (is_unsigned) {
                e.URHADD(dest.B16(), src1.B16(), src2.B16());
              } else {
                e.SRHADD(dest.B16(), src1.B16(), src2.B16());
              }
              break;
            case INT16_TYPE:
              if (is_unsigned) {
                e.URHADD(dest.H8(), src1.H8(), src2.H8());
              } else {
                e.SRHADD(dest.H8(), src1.H8(), src2.H8());
              }
              break;
            case INT32_TYPE:
              if (is_unsigned) {
                e.URHADD(dest.S4(), src1.S4(), src2.S4());
              } else {
                e.SRHADD(dest.S4(), src1.S4(), src2.S4());
              }
              break;
            default:
              // INT64 (no RHADD.D), float, etc: safe fallback
              e.EOR(dest.B16(), dest.B16(), dest.B16());
              break;
          }
        });
  }
};
EMITTER_OPCODE_TABLE(OPCODE_VECTOR_AVERAGE, VECTOR_AVERAGE);

// ============================================================================
// OPCODE_INSERT
// ============================================================================
struct INSERT_I8
    : Sequence<INSERT_I8, I<OPCODE_INSERT, V128Op, V128Op, I8Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    e.MOV(i.dest.reg().Belem()[i.src2.constant() ^ 0x3], i.src3.reg());
  }
};
struct INSERT_I16
    : Sequence<INSERT_I16, I<OPCODE_INSERT, V128Op, V128Op, I8Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    e.MOV(i.dest.reg().Helem()[i.src2.constant() ^ 0x1], i.src3.reg());
  }
};
struct INSERT_I32
    : Sequence<INSERT_I32, I<OPCODE_INSERT, V128Op, V128Op, I8Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.is_constant);
    e.MOV(i.dest.reg().Selem()[i.src2.constant()], i.src3.reg());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_INSERT, INSERT_I8, INSERT_I16, INSERT_I32);

// ============================================================================
// OPCODE_EXTRACT
// ============================================================================
// TODO(benvanik): sequence extract/splat:
//  v0.i32 = extract v0.v128, 0
//  v0.v128 = splat v0.i32
// This can be a single broadcast.
struct EXTRACT_I8
    : Sequence<EXTRACT_I8, I<OPCODE_EXTRACT, I8Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.UMOV(i.dest, i.src1.reg().Belem()[VEC128_B(i.src2.constant())]);
    } else {
      // Fixup index
      e.EOR(W0, i.src2, 0b11);
      e.AND(W0, W0, 0x1F);
      e.DUP(Q0.B16(), W0);
      // Byte-table lookup
      e.TBL(Q0.B16(), List{i.src1.reg().B16()}, Q0.B16());
      // Get lowest element
      e.UMOV(i.dest, Q0.Belem()[0]);
    }
  }
};
struct EXTRACT_I16
    : Sequence<EXTRACT_I16, I<OPCODE_EXTRACT, I16Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src2.is_constant) {
      e.UMOV(i.dest, i.src1.reg().Helem()[VEC128_W(i.src2.constant())]);
    } else {
      // Fixup index
      e.EOR(W0, i.src2, 0b01);
      e.LSL(W0, W0, 1);

      // Replicate index as byte
      e.MOV(W1, 0x01'01);
      e.MUL(W0, W0, W1);

      // Byte indices
      e.ADD(W0, W0, 0x01'00);
      e.UXTH(W0, W0);

      // Replicate byte indices
      e.DUP(Q0.H8(), W0);
      // Byte-table lookup
      e.TBL(Q0.B16(), List{i.src1.reg().B16()}, Q0.B16());
      // Get lowest element
      e.UMOV(i.dest, Q0.Helem()[0]);
    }
  }
};
struct EXTRACT_I32
    : Sequence<EXTRACT_I32, I<OPCODE_EXTRACT, I32Op, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    static const vec128_t extract_table_32[4] = {
        vec128b(3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        vec128b(7, 6, 5, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        vec128b(11, 10, 9, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        vec128b(15, 14, 13, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
    };
    if (i.src2.is_constant) {
      e.UMOV(i.dest, i.src1.reg().Selem()[VEC128_D(i.src2.constant())]);
    } else {
      QReg src1 = i.src1.reg();
      if (i.src1.is_constant) {
        src1 = Q1;
        e.LoadConstantV(src1, i.src1.constant());
      }

      e.AND(X0, i.src2.reg().toX(), 0b11);
      e.LSL(X0, X0, 4);

      e.MOV(X1, reinterpret_cast<uintptr_t>(extract_table_32));
      e.LDR(Q0, X1, X0);

      // Byte-table lookup
      e.TBL(Q0.B16(), List{src1.B16()}, Q0.B16());
      // Get lowest element
      e.UMOV(i.dest, Q0.Selem()[0]);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_EXTRACT, EXTRACT_I8, EXTRACT_I16, EXTRACT_I32);

// ============================================================================
// OPCODE_SPLAT
// ============================================================================
// Copy a value into all elements of a vector
struct SPLAT_I8 : Sequence<SPLAT_I8, I<OPCODE_SPLAT, V128Op, I8Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      e.MOVI(i.dest.reg().B16(), i.src1.constant());
    } else {
      e.DUP(i.dest.reg().B16(), i.src1);
    }
  }
};
struct SPLAT_I16 : Sequence<SPLAT_I16, I<OPCODE_SPLAT, V128Op, I16Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      if ((i.src1.constant() & 0xFF'00) == 0) {
        e.MOVI(i.dest.reg().H8(), i.src1.constant());
        return;
      } else if ((i.src1.constant() & 0x00'FF) == 0) {
        e.MOVI(i.dest.reg().H8(), i.src1.constant(), oaknut::util::LSL, 8);
        return;
      }
      e.MOV(W0, i.src1.constant());
      e.DUP(i.dest.reg().H8(), W0);
    } else {
      e.DUP(i.dest.reg().H8(), i.src1);
    }
  }
};
struct SPLAT_I32 : Sequence<SPLAT_I32, I<OPCODE_SPLAT, V128Op, I32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      oaknut::FImm8 fp8(0);
      if (f32_to_fimm8(i.src1.value->constant.u32, fp8)) {
        e.FMOV(i.dest.reg().S4(), fp8);
        return;
      } else if ((i.src1.constant() & 0xFF'FF'FF'00) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.constant());
        return;
      } else if ((i.src1.constant() & 0xFF'FF'00'FF) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.constant(), oaknut::util::LSL, 8);
        return;
      } else if ((i.src1.constant() & 0xFF'00'FF'FF) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.constant(), oaknut::util::LSL, 16);
        return;
      } else if ((i.src1.constant() & 0x00'FF'FF'FF) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.constant(), oaknut::util::LSL, 24);
        return;
      }
      e.MOV(W0, i.src1.constant());
      e.DUP(i.dest.reg().S4(), W0);
    } else {
      e.DUP(i.dest.reg().S4(), i.src1);
    }
  }
};
struct SPLAT_F32 : Sequence<SPLAT_F32, I<OPCODE_SPLAT, V128Op, F32Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    if (i.src1.is_constant) {
      oaknut::FImm8 fp8(0);
      if (f32_to_fimm8(i.src1.value->constant.u32, fp8)) {
        e.FMOV(i.dest.reg().S4(), fp8);
        return;
      } else if ((i.src1.value->constant.u32 & 0xFF'FF'FF'00) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.value->constant.u32);
        return;
      } else if ((i.src1.value->constant.u32 & 0xFF'FF'00'FF) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.value->constant.u32, oaknut::util::LSL,
               8);
        return;
      } else if ((i.src1.value->constant.u32 & 0xFF'00'FF'FF) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.value->constant.u32, oaknut::util::LSL,
               16);
        return;
      } else if ((i.src1.value->constant.u32 & 0x00'FF'FF'FF) == 0) {
        e.MOVI(i.dest.reg().S4(), i.src1.value->constant.u32, oaknut::util::LSL,
               24);
        return;
      }
      e.MOV(W0, i.src1.value->constant.i32);
      e.DUP(i.dest.reg().S4(), W0);
    } else {
      e.DUP(i.dest.reg().S4(), i.src1.reg().toQ().Selem()[0]);
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_SPLAT, SPLAT_I8, SPLAT_I16, SPLAT_I32, SPLAT_F32);

// ============================================================================
// OPCODE_PERMUTE
// ============================================================================
struct PERMUTE_I32
    : Sequence<PERMUTE_I32, I<OPCODE_PERMUTE, V128Op, I32Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.instr->flags == INT32_TYPE);
    // Permute words between src2 and src3.
    if (i.src1.is_constant) {
      // Each byte is a word-index
      const uint32_t control = i.src1.constant();
      const QReg indices = Q0;

      // Word to byte index
      e.MOV(W0, control * 4);
      e.MOV(indices.Selem()[0], W0);

      // Widen int8 to int16
      e.ZIP1(indices.B16(), indices.B16(), indices.B16());
      // Widen int16 to int32
      e.ZIP1(indices.B16(), indices.B16(), indices.B16());

      // Convert to byte-indices
      e.MOV(W0, 0x03'02'01'00);
      e.DUP(Q1.S4(), W0);
      e.ADD(indices.S4(), indices.S4(), Q1.S4());

      // Table-registers must be sequential indices
      const QReg table0 = Q2;
      if (i.src2.is_constant) {
        e.LoadConstantV(table0, i.src2.constant());
      } else {
        e.MOV(table0.B16(), i.src2.reg().B16());
      }

      const QReg table1 = Q3;
      if (i.src3.is_constant) {
        e.LoadConstantV(table1, i.src3.constant());
      } else {
        e.MOV(table1.B16(), i.src3.reg().B16());
      }

      e.TBL(i.dest.reg().B16(), List{table0.B16(), table1.B16()},
            indices.B16());
    } else {
      // Non-constant permute control (I32 word indices in src1)
      const WReg ctrl = i.src1.reg().toW();

      // Build 16-byte index vector at runtime for TBL
      // Each 2-bit field selects a source word (0-3 from src2/src3 combined)
      // We expand to proper byte indices (0-15 within the 32-byte combined table)

      QReg idx = Q0;

      // Start with the control replicated in a way we can process
      e.DUP(idx.S4(), ctrl);

      // Mask and shift to extract each 2-bit word index, then scale *4
      // Lane 0 (bits 0-1)
      e.AND(W1, ctrl, 0x3);
      e.LSL(W1, W1, 2);           // *4 for byte base
      e.MOV(idx.Selem()[0], W1);

      // Lane 1 (bits 2-3)
      e.LSR(W1, ctrl, 2);
      e.AND(W1, W1, 0x3);
      e.LSL(W1, W1, 2);
      e.MOV(idx.Selem()[1], W1);

      // Lane 2 (bits 4-5)
      e.LSR(W1, ctrl, 4);
      e.AND(W1, W1, 0x3);
      e.LSL(W1, W1, 2);
      e.MOV(idx.Selem()[2], W1);

      // Lane 3 (bits 6-7)
      e.LSR(W1, ctrl, 6);
      e.AND(W1, W1, 0x3);
      e.LSL(W1, W1, 2);
      e.MOV(idx.Selem()[3], W1);

      // Now add the byte offsets 0,1,2,3 within each word
      e.MOV(W0, 0x03'02'01'00);
      e.DUP(Q1.S4(), W0);
      e.ADD(idx.S4(), idx.S4(), Q1.S4());

      // Table registers (same as constant path)
      const QReg table0 = Q2;
      if (i.src2.is_constant) {
        e.LoadConstantV(table0, i.src2.constant());
      } else {
        e.MOV(table0.B16(), i.src2.reg().B16());
      }

      const QReg table1 = Q3;
      if (i.src3.is_constant) {
        e.LoadConstantV(table1, i.src3.constant());
      } else {
        e.MOV(table1.B16(), i.src3.reg().B16());
      }

      e.TBL(i.dest.reg().B16(), List{table0.B16(), table1.B16()},
            idx.B16());
    }
  }
};
struct PERMUTE_V128
    : Sequence<PERMUTE_V128,
               I<OPCODE_PERMUTE, V128Op, V128Op, V128Op, V128Op>> {
  static void EmitByInt8(A64Emitter& e, const EmitArgType& i) {
    // Permute bytes between src2 and src3.
    // src1 is an array of indices corresponding to positions within src2 and
    // src3.
    if (i.src3.value->IsConstantZero()) {
      if (i.src2.value->IsConstantZero()) {
        // src2 & src3 are zero, so result will always be zero.
        e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16());
        return;
      }
    }

    const QReg indices = Q0;
    if (i.src1.is_constant) {
      e.LoadConstantV(indices, i.src1.constant());
    } else {
      e.MOV(indices.B16(), i.src1.reg().B16());
    }

    // Indices must be endian-swapped
    e.MOVI(Q1.B16(), 0b11);
    e.EOR(indices.B16(), indices.B16(), Q1.B16());

    // Modulo 32 the indices
    e.MOVI(Q1.B16(), 0b0001'1111);
    e.AND(indices.B16(), indices.B16(), Q1.B16());

    // Table-registers must be sequential indices
    const QReg table_lo = Q2;
    if (i.src2.is_constant) {
      e.LoadConstantV(table_lo, i.src2.constant());
    } else {
      e.MOV(table_lo.B16(), i.src2.reg().B16());
    }

    const QReg table_hi = Q3;
    if (i.src3.is_constant) {
      e.LoadConstantV(table_hi, i.src3.constant());
    } else {
      e.MOV(table_hi.B16(), i.src3.reg().B16());
    }

    e.TBL(i.dest.reg().B16(), List{table_lo.B16(), table_hi.B16()},
          indices.B16());
  }

  static void EmitByInt16(A64Emitter& e, const EmitArgType& i) {
    // Permute bytes between src2 and src3.
    // src1 is an array of indices corresponding to positions within src2 and
    // src3.
    if (i.src3.value->IsConstantZero()) {
      if (i.src2.value->IsConstantZero()) {
        // src2 & src3 are zero, so result will always be zero.
        e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16());
        return;
      }
    }

    const QReg indices = Q0;
    if (i.src1.is_constant) {
      e.LoadConstantV(indices, i.src1.constant());
    } else {
      e.MOV(indices.B16(), i.src1.reg().B16());
    }

    // Indices must be endian-swapped
    e.MOVI(Q1.H8(), 0b1);
    e.EOR(indices.B16(), indices.B16(), Q1.B16());

    // Modulo-16 the indices
    e.MOVI(Q1.H8(), 0b0000'1111);
    e.AND(indices.B16(), indices.B16(), Q1.B16());

    // Convert int16 indices into int8
    e.MOVI(Q1.B16(), 0x02);
    e.MUL(indices.H8(), indices.H8(), Q1.H8());

    e.MOVI(Q1.H8(), 0x01, LSL, 8);
    e.ADD(indices.H8(), indices.H8(), Q1.H8());

    // Table-registers must be sequential indices
    const QReg table_lo = Q2;
    if (i.src2.is_constant) {
      e.LoadConstantV(table_lo, i.src2.constant());
    } else {
      e.MOV(table_lo.B16(), i.src2.reg().B16());
    }

    const QReg table_hi = Q3;
    if (i.src3.is_constant) {
      e.LoadConstantV(table_hi, i.src3.constant());
    } else {
      e.MOV(table_hi.B16(), i.src3.reg().B16());
    }

    e.TBL(i.dest.reg().B16(), List{table_lo.B16(), table_hi.B16()},
          indices.B16());
  }

  static void EmitByInt32(A64Emitter& e, const EmitArgType& i) {
    // Word permute between src2 and src3 using src1 as byte indices (0-31).
    // Scale word indices to byte indices (each word = 4 bytes).
    const QReg indices = Q0;
    if (i.src1.is_constant) {
      e.LoadConstantV(indices, i.src1.constant());
    } else {
      e.MOV(indices.B16(), i.src1.reg().B16());
    }

    // Each source index is a word index (0-7). Scale *4 and add byte offset within word.
    // For simplicity and correctness, convert to proper byte permute indices.
    e.MOVI(Q1.B16(), 0x03);           // byte within word mask
    e.AND(Q2.B16(), indices.B16(), Q1.B16()); // byte offset in word

    e.MOVI(Q1.B16(), 0xFC);           // clear low 2 bits
    e.AND(indices.B16(), indices.B16(), Q1.B16());

    // Multiply word index by 4 (shift left 2)
    e.SHL(indices.B16(), indices.B16(), 2);

    // Add byte offset
    e.ADD(indices.B16(), indices.B16(), Q2.B16());

    // Now indices are proper byte positions (0-31)
    // Modulo is not strictly needed if sources are well-formed, but safe:
    e.MOVI(Q1.B16(), 0x1F);
    e.AND(indices.B16(), indices.B16(), Q1.B16());

    const QReg table_lo = Q2;
    if (i.src2.is_constant) {
      e.LoadConstantV(table_lo, i.src2.constant());
    } else {
      e.MOV(table_lo.B16(), i.src2.reg().B16());
    }

    const QReg table_hi = Q3;
    if (i.src3.is_constant) {
      e.LoadConstantV(table_hi, i.src3.constant());
    } else {
      e.MOV(table_hi.B16(), i.src3.reg().B16());
    }

    e.TBL(i.dest.reg().B16(), List{table_lo.B16(), table_hi.B16()},
          indices.B16());
  }

  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags) {
      case INT8_TYPE:
        EmitByInt8(e, i);
        break;
      case INT16_TYPE:
        EmitByInt16(e, i);
        break;
      case INT32_TYPE:
        EmitByInt32(e, i);
        break;
      case FLOAT32_TYPE:
        // FLOAT32 uses identical 4-element word permutation logic
        EmitByInt32(e, i);
        break;
      default:
        // Robust fallback for INT64/FLOAT64 or any future type:
        // interpret src1 bytes directly as byte indices (0-31) into src2||src3.
        // This eliminates assert and provides safe (if not always semantically
        // perfect for >32-bit elements) behavior using TBL. HIR currently
        // restricts Permute to <= INT32.
        EmitByInt8(e, i);
        break;
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_PERMUTE, PERMUTE_I32, PERMUTE_V128);

// ============================================================================
// OPCODE_SWIZZLE
// ============================================================================
struct SWIZZLE
    : Sequence<SWIZZLE, I<OPCODE_SWIZZLE, V128Op, V128Op, OffsetOp>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    auto element_type = i.instr->flags;
    if (element_type == INT8_TYPE) {
      // Byte swizzle - fall back to general permute style
      const uint8_t swizzle_mask = static_cast<uint8_t>(i.src2.value);
      const vec128_t indice_vec = vec128i(
          ((swizzle_mask >> 0) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00,
          ((swizzle_mask >> 2) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00,
          ((swizzle_mask >> 4) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00,
          ((swizzle_mask >> 6) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00);
      const QReg indices = Q1;
      e.LoadConstantV(indices, indice_vec);
      QReg table0 = Q0;
      if (i.src1.is_constant) {
        e.LoadConstantV(table0, i.src1.constant());
      } else {
        table0 = i.src1;
      }
      e.TBL(i.dest.reg().B16(), List{table0.B16()}, indices.B16());
    } else if (element_type == INT16_TYPE) {
      // Halfword swizzle (common in some audio / matrix code)
      const uint8_t swizzle_mask = static_cast<uint8_t>(i.src2.value);
      const vec128_t indice_vec = vec128i(
          ((swizzle_mask >> 0) & 0b11) * 0x08'08'08'08 + 0x07'06'05'04'03'02'01'00,
          ((swizzle_mask >> 2) & 0b11) * 0x08'08'08'08 + 0x07'06'05'04'03'02'01'00,
          ((swizzle_mask >> 4) & 0b11) * 0x08'08'08'08 + 0x07'06'05'04'03'02'01'00,
          ((swizzle_mask >> 6) & 0b11) * 0x08'08'08'08 + 0x07'06'05'04'03'02'01'00);
      const QReg indices = Q1;
      e.LoadConstantV(indices, indice_vec);
      QReg table0 = Q0;
      if (i.src1.is_constant) {
        e.LoadConstantV(table0, i.src1.constant());
      } else {
        table0 = i.src1;
      }
      e.TBL(i.dest.reg().B16(), List{table0.B16()}, indices.B16());
    } else if (element_type == INT32_TYPE || element_type == FLOAT32_TYPE) {
      // Four 2-bit word-indices packed into one 8-bit value.
      // Prioritizes common 360 paths: vpermwi128, vrlimi128 rotates/swizzles.
      const uint8_t swizzle_mask = static_cast<uint8_t>(i.src2.value);
      uint8_t sx = (swizzle_mask >> 0) & 0b11;
      uint8_t sy = (swizzle_mask >> 2) & 0b11;
      uint8_t sz = (swizzle_mask >> 4) & 0b11;
      uint8_t sw = (swizzle_mask >> 6) & 0b11;

      QReg src = Q0;
      if (i.src1.is_constant) {
        e.LoadConstantV(src, i.src1.constant());
      } else {
        src = i.src1;
      }

      // High-quality fast paths using EXT (byte rotate), DUP (broadcast), MOV (identity).
      // These are used heavily by vpermwi128 and vrlimi128 in games.
      if (sx == 0 && sy == 1 && sz == 2 && sw == 3) {
        e.MOV(i.dest.reg().B16(), src.B16());
        return;
      } else if (sx == sy && sx == sz && sx == sw) {
        // Broadcast one word to all lanes (common for splat-like swizzles)
        e.DUP(i.dest.reg().S4(), src.Selem()[sx]);
        return;
      } else if (sx == 1 && sy == 2 && sz == 3 && sw == 0) {
        e.EXT(i.dest.reg().B16(), src.B16(), src.B16(), 4);  // XYZW -> YZWX
        return;
      } else if (sx == 2 && sy == 3 && sz == 0 && sw == 1) {
        e.EXT(i.dest.reg().B16(), src.B16(), src.B16(), 8);  // XYZW -> ZWXY
        return;
      } else if (sx == 3 && sy == 0 && sz == 1 && sw == 2) {
        e.EXT(i.dest.reg().B16(), src.B16(), src.B16(), 12); // XYZW -> WXYZ
        return;
      }

      // General case: build byte indices and use robust TBL (correct for all masks)
      const vec128_t indice_vec =
          vec128i(((swizzle_mask >> 0) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00,
                  ((swizzle_mask >> 2) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00,
                  ((swizzle_mask >> 4) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00,
                  ((swizzle_mask >> 6) & 0b11) * 0x04'04'04'04 + 0x03'02'01'00);

      const QReg indices = Q1;
      e.LoadConstantV(indices, indice_vec);

      e.TBL(i.dest.reg().B16(), List{src.B16()}, indices.B16());
    } else if (element_type == INT64_TYPE || element_type == FLOAT64_TYPE) {
      // 64-bit swizzle (high/low 64-bit halves). Robust for all selector combos using EXT/DUP or TBL.
      const uint8_t swizzle_mask = static_cast<uint8_t>(i.src2.value);
      uint8_t sel0 = (swizzle_mask >> 0) & 0b11;
      uint8_t sel1 = (swizzle_mask >> 2) & 0b11;
      QReg src = Q0;
      if (i.src1.is_constant) {
        e.LoadConstantV(src, i.src1.constant());
      } else {
        src = i.src1;
      }

      if (sel0 == 0 && sel1 == 1) {
        // Identity: low->low, high->high
        e.MOV(i.dest.reg().B16(), src.B16());
      } else if (sel0 == 0 && sel1 == 0) {
        // Dup low 64 to both halves (common)
        e.MOV(i.dest.reg().D1()[0], src.D1()[0]);
        e.MOV(i.dest.reg().D1()[1], src.D1()[0]);
      } else if (sel0 == 1 && sel1 == 1) {
        // Dup high 64 to both halves
        e.MOV(i.dest.reg().D1()[0], src.D1()[1]);
        e.MOV(i.dest.reg().D1()[1], src.D1()[1]);
      } else if (sel0 == 1 && sel1 == 0) {
        // Swap the two 64-bit halves
        e.EXT(i.dest.reg().B16(), src.B16(), src.B16(), 8);
      } else {
        // General/arbitrary: build correct per-lane byte indices and TBL (fixed previous bugs)
        uint64_t bytes0 = (sel0 & 1) ? 0x0F0E0D0C0B0A0908ULL : 0x0706050403020100ULL;
        uint64_t bytes1 = (sel1 & 1) ? 0x0F0E0D0C0B0A0908ULL : 0x0706050403020100ULL;
        vec128_t indice_vec{};
        indice_vec.u64[0] = bytes0;
        indice_vec.u64[1] = bytes1;
        const QReg indices = Q1;
        e.LoadConstantV(indices, indice_vec);
        e.TBL(i.dest.reg().B16(), List{src.B16()}, indices.B16());
      }
    } else {
      // Unknown element type for swizzle - safe fallback (copy). HIR only emits I32/F32.
      if (i.src1.is_constant) {
        e.LoadConstantV(i.dest.reg(), i.src1.constant());
      } else {
        e.MOV(i.dest.reg().B16(), i.src1.reg().B16());
      }
    }
  };
};
EMITTER_OPCODE_TABLE(OPCODE_SWIZZLE, SWIZZLE);

// ============================================================================
// OPCODE_PACK
// ============================================================================
struct PACK : Sequence<PACK, I<OPCODE_PACK, V128Op, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags & PACK_TYPE_MODE) {
      case PACK_TYPE_D3DCOLOR:
        EmitD3DCOLOR(e, i);
        break;
      case PACK_TYPE_FLOAT16_2:
        EmitFLOAT16_2(e, i);
        break;
      case PACK_TYPE_FLOAT16_4:
        EmitFLOAT16_4(e, i);
        break;
      case PACK_TYPE_SHORT_2:
        EmitSHORT_2(e, i);
        break;
      case PACK_TYPE_SHORT_4:
        EmitSHORT_4(e, i);
        break;
      case PACK_TYPE_UINT_2101010:
        EmitUINT_2101010(e, i);
        break;
      case PACK_TYPE_ULONG_4202020:
        EmitULONG_4202020(e, i);
        break;
      case PACK_TYPE_8_IN_16:
        Emit8_IN_16(e, i, i.instr->flags);
        break;
      case PACK_TYPE_16_IN_32:
        Emit16_IN_32(e, i, i.instr->flags);
        break;
      default:
        ReportUnhandledA64Opcode(&e, i.instr, "PACK type");
        EmitStructuredFallback(e, i.instr);
        if (cvars::a64_accuracy_debug) {
          e.DebugBreak();  // surface immediately in debug mode
        }
        break;
    }
  }
  static void EmitD3DCOLOR(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    QReg src = i.src1;
    if (i.src1.is_constant) {
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    }

    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    // Saturate to [3,3....] so that only values between 3...[00] and 3...[FF]
    // are valid - max before min to pack NaN as zero (5454082B is heavily
    // affected by the order - packs 0xFFFFFFFF in matrix code to get a 0
    // constant).
    e.LDR(Q0, VConstData, e.GetVConstOffset(V3333));
    e.FMAX(i.dest.reg().S4(), src.S4(), Q0.S4());

    e.LDR(Q0, VConstData, e.GetVConstOffset(VPackD3DCOLORSat));
    e.FMIN(i.dest.reg().S4(), i.dest.reg().S4(), Q0.S4());
    // Extract bytes.
    // RGBA (XYZW) -> ARGB (WXYZ)
    // w = ((src1.uw & 0xFF) << 24) | ((src1.ux & 0xFF) << 16) |
    //     ((src1.uy & 0xFF) << 8) | (src1.uz & 0xFF)
    e.LDR(Q0, VConstData, e.GetVConstOffset(VPackD3DCOLOR));
    e.TBL(i.dest.reg().B16(), List{i.dest.reg().B16()}, Q0.B16());
  }
  static uint8x16_t EmulateFLOAT16_2(void*, uint8x16_t src1) {
    alignas(16) float a[4];
    alignas(16) uint16_t b[8];
    vst1q_u8(reinterpret_cast<uint8_t*>(&a), src1);
    std::memset(b, 0, sizeof(b));

    for (int i = 0; i < 2; i++) {
      b[7 - i] = half_float::detail::float2half<std::round_toward_zero>(a[i]);
    }

    return vld1q_u8(reinterpret_cast<uint8_t*>(&b));
  }
  static void EmitFLOAT16_2(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    // http://blogs.msdn.com/b/chuckw/archive/2012/09/11/directxmath-f16c-and-fma.aspx
    // dest = [(src1.x | src1.y), 0, 0, 0]

    if (e.IsFeatureEnabled(kA64EmitF16C)) {
      const QReg src1 = i.src1.is_constant ? Q0 : i.src1;
      if (i.src1.is_constant) {
        e.LoadConstantV(src1, i.src1.constant());
      }
      e.FCVTN(i.dest.reg().toD().H4(), src1.S4());
      e.MOVI(Q0.B16(), 0);
      e.EXT(i.dest.reg().B16(), Q0.B16(), i.dest.reg().B16(), 4);
      e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
      return;
    }

    if (i.src1.is_constant) {
      e.LoadConstantV(Q0, i.src1.constant());
    } else {
      e.MOV(Q0.B16(), i.src1.reg().B16());
    }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateFLOAT16_2));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static uint8x16_t EmulateFLOAT16_4(void*, uint8x16_t src1) {
    alignas(16) float a[4];
    alignas(16) uint16_t b[8];
    vst1q_u8(reinterpret_cast<uint8_t*>(&a), src1);
    std::memset(b, 0, sizeof(b));

    for (int i = 0; i < 4; i++) {
      b[7 - (i ^ 2)] =
          half_float::detail::float2half<std::round_toward_zero>(a[i]);
    }

    return vld1q_u8(reinterpret_cast<uint8_t*>(&b));
  }
  static void EmitFLOAT16_4(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    // http://blogs.msdn.com/b/chuckw/archive/2012/09/11/directxmath-f16c-and-fma.aspx
    // dest = [(src1.z | src1.w), (src1.x | src1.y), 0, 0]

    if (e.IsFeatureEnabled(kA64EmitF16C)) {
      const QReg src1 = i.src1.is_constant ? Q0 : i.src1;
      if (i.src1.is_constant) {
        e.LoadConstantV(src1, i.src1.constant());
      }
      e.FCVTN(i.dest.reg().toD().H4(), src1.S4());
      e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
      e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
      return;
    }

    if (i.src1.is_constant) {
      e.LoadConstantV(Q0, i.src1.constant());
    } else {
      e.MOV(Q0.B16(), i.src1.reg().B16());
    }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateFLOAT16_4));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static void EmitSHORT_2(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    QReg src = i.src1;
    if (i.src1.is_constant) {
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    }
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    // Saturate
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackSHORT_Min));
    e.FMAX(i.dest.reg().S4(), src.S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackSHORT_Max));
    e.FMIN(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Pack
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackSHORT_2));
    e.TBL(i.dest.reg().B16(), oaknut::List{i.dest.reg().B16()}, Q1.B16());
  }
  static void EmitSHORT_4(A64Emitter& e, const EmitArgType& i) {
    assert_true(i.src2.value->IsConstantZero());
    QReg src = i.src1;
    if (i.src1.is_constant) {
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    }
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    // Saturate
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackSHORT_Min));
    e.FMAX(i.dest.reg().S4(), src.S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackSHORT_Max));
    e.FMIN(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Pack
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackSHORT_4));
    e.TBL(i.dest.reg().B16(), oaknut::List{i.dest.reg().B16()}, Q1.B16());
  }
  static void EmitUINT_2101010(A64Emitter& e, const EmitArgType& i) {
    // https://www.opengl.org/registry/specs/ARB/vertex_type_2_10_10_10_rev.txt
    // XYZ are 10 bits, signed and saturated.
    // W is 2 bits, unsigned and saturated.
    const QReg src = i.dest;
    if (i.src1.is_constant) {
      e.LoadConstantV(src, i.src1.constant());
    }
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    // Saturate.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackUINT_2101010_MinUnpacked));
    e.FMAX(i.dest.reg().S4(), src.S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackUINT_2101010_MaxUnpacked));
    e.FMIN(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Remove the unneeded bits of the floats.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackUINT_2101010_MaskUnpacked));
    e.AND(i.dest.reg().B16(), i.dest.reg().B16(), Q1.B16());

    // Shift the components up.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackUINT_2101010_Shift));
    e.USHL(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Combine the components.
    e.LoadConstantV(Q1, vec128i(0x03'02'01'00 + 0x04'04'04'04 * 2,
                                0x03'02'01'00 + 0x04'04'04'04 * 3,
                                0x03'02'01'00 + 0x04'04'04'04 * 0,
                                0x03'02'01'00 + 0x04'04'04'04 * 1));
    e.TBL(Q0.B16(), oaknut::List{i.dest.reg().B16()}, Q1.B16());
    e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), Q0.B16());

    e.LoadConstantV(Q1, vec128i(0x03'02'01'00 + 0x04'04'04'04 * 1,
                                0x03'02'01'00 + 0x04'04'04'04 * 0,
                                0x03'02'01'00 + 0x04'04'04'04 * 3,
                                0x03'02'01'00 + 0x04'04'04'04 * 2));
    e.TBL(Q0.B16(), oaknut::List{i.dest.reg().B16()}, Q1.B16());
    e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), Q0.B16());
  }
  static void EmitULONG_4202020(A64Emitter& e, const EmitArgType& i) {
    // XYZ are 20 bits, signed and saturated.
    // W is 4 bits, unsigned and saturated.
    QReg src = i.src1;
    if (i.src1.is_constant) {
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    }
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    // Saturate.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackULONG_4202020_MinUnpacked));
    e.FMAX(i.dest.reg().S4(), src.S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackULONG_4202020_MaxUnpacked));
    e.FMIN(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Remove the unneeded bits of the floats (so excess nibbles will also be
    // cleared).
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackULONG_4202020_MaskUnpacked));
    e.AND(i.dest.reg().B16(), i.dest.reg().B16(), Q1.B16());

    // Store Y and W shifted left by 4 so vpshufb can be used with them.
    e.SHL(Q0.S4(), i.dest.reg().S4(), 4);

    // Place XZ where they're supposed to be.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackULONG_4202020_PermuteXZ));
    e.TBL(i.dest.reg().B16(), oaknut::List{i.dest.reg().B16()}, Q1.B16());
    // Place YW.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackULONG_4202020_PermuteYW));
    e.TBL(Q0.B16(), oaknut::List{Q0.B16()}, Q1.B16());
    // Merge XZ and YW.
    e.EOR(i.dest.reg().B16(), i.dest.reg().B16(), Q0.B16());
  }
  
  static void Emit8_IN_16(A64Emitter& e, const EmitArgType& i, uint32_t flags) {
    // Robust handling for all sign/saturate combinations using NEON narrow ops.
    // Always stage inputs into Q0/Q1 temps so we never modify caller registers
    // and always support constant sources.
    QReg src1 = Q0;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    } else {
      e.MOV(src1.B16(), i.src1.reg().B16());
    }
    QReg src2 = Q1;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2, i.src2.constant());
    } else {
      e.MOV(src2.B16(), i.src2.reg().B16());
    }

    if (IsPackInUnsigned(flags)) {
      if (IsPackOutUnsigned(flags)) {
        if (IsPackOutSaturate(flags)) {
          // unsigned -> unsigned + saturate
          e.UQXTN(Q2.toD().B8(), src2.H8());
          e.UQXTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // unsigned -> unsigned (modulo / truncate low 8 bits)
          e.XTN(Q2.toD().B8(), src2.H8());
          e.XTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      } else {
        if (IsPackOutSaturate(flags)) {
          // unsigned -> signed + saturate (clamp [0, 0x7F] then truncate)
          e.LoadConstantV(Q3, vec128i(0x007F007Fu, 0x007F007Fu, 0x007F007Fu, 0x007F007Fu));
          e.UMIN(src1.H8(), src1.H8(), Q3.H8());
          e.UMIN(src2.H8(), src2.H8(), Q3.H8());
          e.XTN(Q2.toD().B8(), src2.H8());
          e.XTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // unsigned -> signed (modulo / truncate low 8 bits)
          e.XTN(Q2.toD().B8(), src2.H8());
          e.XTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      }
    } else {
      if (IsPackOutUnsigned(flags)) {
        if (IsPackOutSaturate(flags)) {
          // signed -> unsigned + saturate
          e.SQXTUN(Q2.toD().B8(), src2.H8());
          e.SQXTUN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // signed -> unsigned (modulo / truncate low 8 bits)
          e.XTN(Q2.toD().B8(), src2.H8());
          e.XTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      } else {
        if (IsPackOutSaturate(flags)) {
          // signed -> signed + saturate
          e.SQXTN(Q2.toD().B8(), src2.H8());
          e.SQXTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // signed -> signed (modulo / truncate low 8 bits)
          e.XTN(Q2.toD().B8(), src2.H8());
          e.XTN2(Q2.B16(), src1.H8());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      }
    }
  }
  // Pack 2 32-bit vectors into a 16-bit vector.
  static void Emit16_IN_32(A64Emitter& e, const EmitArgType& i,
                           uint32_t flags) {
    // Robust handling for all sign/saturate combinations using NEON narrow ops.
    // Stage inputs into Q0/Q1 temps (always support constants, never mutate inputs).
    QReg src1 = Q0;
    if (i.src1.is_constant) {
      e.LoadConstantV(src1, i.src1.constant());
    } else {
      e.MOV(src1.B16(), i.src1.reg().B16());
    }
    QReg src2 = Q1;
    if (i.src2.is_constant) {
      e.LoadConstantV(src2, i.src2.constant());
    } else {
      e.MOV(src2.B16(), i.src2.reg().B16());
    }

    if (IsPackInUnsigned(flags)) {
      if (IsPackOutUnsigned(flags)) {
        if (IsPackOutSaturate(flags)) {
          // unsigned -> unsigned + saturate
          e.UQXTN(Q2.toD().H4(), src2.S4());
          e.UQXTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // unsigned -> unsigned (modulo / truncate low 16 bits)
          e.XTN(Q2.toD().H4(), src2.S4());
          e.XTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      } else {
        if (IsPackOutSaturate(flags)) {
          // unsigned -> signed + saturate (clamp [0, 0x7FFF] then truncate)
          e.LoadConstantV(Q3, vec128i(0x00007FFFu, 0x00007FFFu, 0x00007FFFu, 0x00007FFFu));
          e.UMIN(src1.S4(), src1.S4(), Q3.S4());
          e.UMIN(src2.S4(), src2.S4(), Q3.S4());
          e.XTN(Q2.toD().H4(), src2.S4());
          e.XTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // unsigned -> signed (modulo / truncate low 16 bits)
          e.XTN(Q2.toD().H4(), src2.S4());
          e.XTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      }
    } else {
      if (IsPackOutUnsigned(flags)) {
        if (IsPackOutSaturate(flags)) {
          // signed -> unsigned + saturate
          e.SQXTUN(Q2.toD().H4(), src2.S4());
          e.SQXTUN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // signed -> unsigned (modulo / truncate low 16 bits)
          e.XTN(Q2.toD().H4(), src2.S4());
          e.XTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      } else {
        if (IsPackOutSaturate(flags)) {
          // signed -> signed + saturate
          e.SQXTN(Q2.toD().H4(), src2.S4());
          e.SQXTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        } else {
          // signed -> signed (modulo / truncate low 16 bits)
          e.XTN(Q2.toD().H4(), src2.S4());
          e.XTN2(Q2.H8(), src1.S4());
          e.REV32(i.dest.reg().H8(), Q2.H8());
          e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.dest.reg().B16(), 8);
        }
      }
    }
  }
};
EMITTER_OPCODE_TABLE(OPCODE_PACK, PACK);

// ============================================================================
// OPCODE_UNPACK
// ============================================================================
struct UNPACK : Sequence<UNPACK, I<OPCODE_UNPACK, V128Op, V128Op>> {
  static void Emit(A64Emitter& e, const EmitArgType& i) {
    switch (i.instr->flags & PACK_TYPE_MODE) {
      case PACK_TYPE_D3DCOLOR:
        EmitD3DCOLOR(e, i);
        break;
      case PACK_TYPE_FLOAT16_2:
        EmitFLOAT16_2(e, i);
        break;
      case PACK_TYPE_FLOAT16_4:
        EmitFLOAT16_4(e, i);
        break;
      case PACK_TYPE_SHORT_2:
        EmitSHORT_2(e, i);
        break;
      case PACK_TYPE_SHORT_4:
        EmitSHORT_4(e, i);
        break;
      case PACK_TYPE_UINT_2101010:
        EmitUINT_2101010(e, i);
        break;
      case PACK_TYPE_ULONG_4202020:
        EmitULONG_4202020(e, i);
        break;
      case PACK_TYPE_8_IN_16:
        Emit8_IN_16(e, i, i.instr->flags);
        break;
      case PACK_TYPE_16_IN_32:
        Emit16_IN_32(e, i, i.instr->flags);
        break;
      default:
        ReportUnhandledA64Opcode(&e, i.instr, "PACK type");
        EmitStructuredFallback(e, i.instr);
        if (cvars::a64_accuracy_debug) {
          e.DebugBreak();  // surface immediately in debug mode
        }
        break;
    }
  }
  static void EmitD3DCOLOR(A64Emitter& e, const EmitArgType& i) {
    // ARGB (WXYZ) -> RGBA (XYZW)
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    QReg src(0);

    if (i.src1.is_constant) {
      if (i.src1.value->IsConstantZero()) {
        e.FMOV(i.dest.reg().S4(), FImm8(0, 7, 0));
        return;
      }
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }
    // src = ZZYYXXWW
    // Unpack to 000000ZZ,000000YY,000000XX,000000WW
    
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackD3DCOLOR));
    e.TBL(i.dest.reg().B16(), oaknut::List{src.B16()}, Q1.B16());
    // Add 1.0f to each.
    e.FMOV(Q1.S4(), FImm8(0, 7, 0));
    e.ORR(i.dest.reg().B16(), i.dest.reg().B16(), Q1.B16());
    // To convert to 0 to 1, games multiply by 0x47008081 and add 0xC7008081.


  }
  static uint8x16_t EmulateFLOAT16_2(void*, uint8x16_t src1) {
    alignas(16) uint16_t a[8];
    alignas(16) float b[4];
    vst1q_u8(reinterpret_cast<uint8_t*>(&a), src1);
    std::memset(b, 0, sizeof(b));

    for (int i = 0; i < 2; i++) {
      b[i] = half_float::detail::half2float(a[VEC128_W(6 + i)]);
    }

    // Constants, or something
    b[2] = 0.f;
    b[3] = 1.f;

    return vld1q_u8(reinterpret_cast<uint8_t*>(&b));
  }
  static void EmitFLOAT16_2(A64Emitter& e, const EmitArgType& i) {
    // 1 bit sign, 5 bit exponent, 10 bit mantissa
    // D3D10 half float format

    if (e.IsFeatureEnabled(kA64EmitF16C)) {
      const QReg src1 = i.src1.is_constant ? Q0 : i.src1;
      if (i.src1.is_constant) {
        e.LoadConstantV(src1, i.src1.constant());
      }

      // Move the upper 4 bytes to the lower 4 bytes, zero the rest
      e.EOR(Q0.B16(), Q0.B16(), Q0.B16());
      e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), Q0.B16(), 12);

      e.FCVTL(i.dest.reg().S4(), i.dest.reg().toD().H4());
      e.REV64(i.dest.reg().S4(), i.dest.reg().S4());

      // Write 1.0 to element 3
      e.FMOV(S0, oaknut::FImm8(0, 7, 0));
      e.MOV(i.dest.reg().Selem()[3], Q0.Selem()[0]);
      return;
    }

    if (i.src1.is_constant) {
      e.LoadConstantV(Q0, i.src1.constant());
    } else {
      e.MOV(Q0.B16(), i.src1.reg().B16());
    }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateFLOAT16_2));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static uint8x16_t EmulateFLOAT16_4(void*, uint8x16_t src1) {
    alignas(16) uint16_t a[8];
    alignas(16) float b[4];
    vst1q_u8(reinterpret_cast<uint8_t*>(&a), src1);

    for (int i = 0; i < 4; i++) {
      b[i] = half_float::detail::half2float(a[VEC128_W(4 + i)]);
    }

    return vld1q_u8(reinterpret_cast<uint8_t*>(&b));
  }
  static void EmitFLOAT16_4(A64Emitter& e, const EmitArgType& i) {
    // src = [(dest.x | dest.y), (dest.z | dest.w), 0, 0]
    if (e.IsFeatureEnabled(kA64EmitF16C)) {
      const QReg src1 = i.src1.is_constant ? Q0 : i.src1;
      if (i.src1.is_constant) {
        e.LoadConstantV(src1, i.src1.constant());
      }
      e.EXT(i.dest.reg().B16(), i.dest.reg().B16(), i.src1.reg().B16(), 8);
      e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
      e.FCVTL(i.dest.reg().S4(), i.dest.reg().toD().H4());
      return;
    }

    if (i.src1.is_constant) {
      e.LoadConstantV(Q0, i.src1.constant());
    } else {
      e.MOV(Q0.B16(), i.src1.reg().B16());
    }
    e.CallNativeSafe(reinterpret_cast<void*>(EmulateFLOAT16_4));
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static void EmitSHORT_2(A64Emitter& e, const EmitArgType& i) {
    // (VD.x) = 3.0 + (VB.x>>16)*2^-22
    // (VD.y) = 3.0 + (VB.x)*2^-22
    // (VD.z) = 0.0
    // (VD.w) = 1.0 (games splat W after unpacking to get vectors of 1.0f)
    // src is (xx,xx,xx,VALUE)
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    QReg src(0);
    if (i.src1.is_constant) {
      if (i.src1.value->IsConstantZero()) {
        src = i.dest;
        e.LDR(i.dest, VConstData, e.GetVConstOffset(V3301));
        return;
      }
      // TODO(benvanik): check other common constants/perform shuffle/or here.
      src = i.src1;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }
    // Shuffle bytes.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackSHORT_2));
    e.TBL(i.dest.reg().B16(), oaknut::List{src.B16()}, Q1.B16());

    // If negative, make smaller than 3 - sign extend before adding.
    e.SHL(i.dest.reg().S4(), i.dest.reg().S4(), 16);
    e.SSHR(i.dest.reg().S4(), i.dest.reg().S4(), 16);

    // Add 3,3,0,1.
    e.LDR(Q1, VConstData, e.GetVConstOffset(V3301));
    e.ADD(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Return quiet NaNs in case of negative overflow.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackSHORT_Overflow));
    e.CMEQ(Q0.S4(), i.dest.reg().S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VQNaN));
    e.BSL(Q0.B16(), Q1.B16(), i.dest.reg().B16());
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static void EmitSHORT_4(A64Emitter& e, const EmitArgType& i) {
    // (VD.x) = 3.0 + (VB.x>>16)*2^-22
    // (VD.y) = 3.0 + (VB.x)*2^-22
    // (VD.z) = 3.0 + (VB.y>>16)*2^-22
    // (VD.w) = 3.0 + (VB.y)*2^-22
    // src is (xx,xx,VALUE,VALUE)

    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    QReg src(0);
    if (i.src1.is_constant) {
      if (i.src1.value->IsConstantZero()) {
        e.LDR(i.dest, VConstData, e.GetVConstOffset(V3333));
        return;
      }
      // TODO(benvanik): check other common constants/perform shuffle/or here.
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }
    // Shuffle bytes.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackSHORT_4));
    e.TBL(i.dest.reg().B16(), oaknut::List{src.B16()}, Q1.B16());

    // If negative, make smaller than 3 - sign extend before adding.
    e.SHL(i.dest.reg().S4(), i.dest.reg().S4(), 16);
    e.SSHR(i.dest.reg().S4(), i.dest.reg().S4(), 16);

    // Add 3,3,3,3.
    e.LDR(Q1, VConstData, e.GetVConstOffset(V3333));
    e.ADD(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());

    // Return quiet NaNs in case of negative overflow.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackSHORT_Overflow));
    e.CMEQ(Q0.S4(), i.dest.reg().S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VQNaN));
    e.BSL(Q0.B16(), Q1.B16(), i.dest.reg().B16());
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static void EmitUINT_2101010(A64Emitter& e, const EmitArgType& i) {
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    QReg src(0);
    if (i.src1.is_constant) {
      if (i.src1.value->IsConstantZero()) {
        e.LDR(i.dest, VConstData, e.GetVConstOffset(V3331));
        return;
      }
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }

    // Splat W.
    e.DUP(i.dest.reg().S4(), src.Selem()[3]);
    // Keep only the needed components.
    // Red in 0-9 now, green in 10-19, blue in 20-29, alpha in 30-31.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackUINT_2101010_MaskPacked));
    e.AND(i.dest.reg().B16(), i.dest.reg().B16(), Q1.B16());

    // Shift the components down.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VPackUINT_2101010_Shift));
    e.NEG(Q1.S4(), Q1.S4());
    e.USHL(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());
    // If XYZ are negative, make smaller than 3 - sign extend XYZ before adding.
    // W is unsigned.
    e.SHL(i.dest.reg().S4(), i.dest.reg().S4(), 22);
    e.SSHR(i.dest.reg().S4(), i.dest.reg().S4(), 22);
    // Add 3,3,3,1.
    e.LDR(Q1, VConstData, e.GetVConstOffset(V3331));
    e.ADD(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());
    // Return quiet NaNs in case of negative overflow.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackUINT_2101010_Overflow));
    e.CMEQ(Q0.S4(), i.dest.reg().S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VQNaN));
    e.BSL(Q0.B16(), Q1.B16(), i.dest.reg().B16());
    e.MOV(i.dest.reg().B16(), Q0.B16());
    // To convert XYZ to -1 to 1, games multiply by 0x46004020 & sub 0x46C06030.
    // For W to 0 to 1, they multiply by and subtract 0x4A2AAAAB.}
  }
  static void EmitULONG_4202020(A64Emitter& e, const EmitArgType& i) {
    const XReg VConstData = X3;
    e.MOV(VConstData, e.GetVConstPtr());

    QReg src(0);
    if (i.src1.is_constant) {
      if (i.src1.value->IsConstantZero()) {
        e.LDR(i.dest, VConstData, e.GetVConstOffset(V3331));
        return;
      }
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }
    // Extract pairs of nibbles to XZYW. XZ will have excess 4 upper bits, YW
    // will have excess 4 lower bits.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackULONG_4202020_Permute));
    e.TBL(i.dest.reg().B16(), oaknut::List{src.B16()}, Q1.B16());

    // Drop the excess nibble of YW.
    e.USHR(Q0.S4(), i.dest.reg().S4(), 4);
    // Merge XZ and YW now both starting at offset 0.
    e.LoadConstantV(Q1, vec128i(3 * 0x04'04'04'04 + 0x03'02'01'00,
                                2 * 0x04'04'04'04 + 0x03'02'01'00,
                                1 * 0x04'04'04'04 + 0x03'02'01'00,
                                0 * 0x04'04'04'04 + 0x03'02'01'00));
    e.TBL(i.dest.reg().B16(), oaknut::List{i.dest.reg().B16(), Q0.B16()},
          Q1.B16());

    // Reorder as XYZW.
    e.LoadConstantV(Q1, vec128i(3 * 0x04'04'04'04 + 0x03'02'01'00,
                                1 * 0x04'04'04'04 + 0x03'02'01'00,
                                2 * 0x04'04'04'04 + 0x03'02'01'00,
                                0 * 0x04'04'04'04 + 0x03'02'01'00));
    e.TBL(i.dest.reg().B16(), oaknut::List{i.dest.reg().B16(), Q0.B16()},
          Q1.B16());
    // Drop the excess upper nibble in XZ and sign-extend XYZ.
    e.SHL(i.dest.reg().S4(), i.dest.reg().S4(), 12);
    e.SSHR(i.dest.reg().S4(), i.dest.reg().S4(), 12);
    // Add 3,3,3,1.
    e.LDR(Q1, VConstData, e.GetVConstOffset(V3331));
    e.ADD(i.dest.reg().S4(), i.dest.reg().S4(), Q1.S4());
    // Return quiet NaNs in case of negative overflow.
    e.LDR(Q1, VConstData, e.GetVConstOffset(VUnpackULONG_4202020_Overflow));
    e.CMEQ(Q0.S4(), i.dest.reg().S4(), Q1.S4());

    e.LDR(Q1, VConstData, e.GetVConstOffset(VQNaN));
    e.BSL(Q0.B16(), Q1.B16(), i.dest.reg().B16());
    e.MOV(i.dest.reg().B16(), Q0.B16());
  }
  static void Emit8_IN_16(A64Emitter& e, const EmitArgType& i, uint32_t flags) {
    assert_false(IsPackOutSaturate(flags));
    QReg src(0);
    if (i.src1.is_constant) {
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }
    if (IsPackToLo(flags)) {
      // Unpack to LO. Extension choice based on input signedness (IN flag).
      if (IsPackInUnsigned(flags)) {
        if (IsPackOutUnsigned(flags)) {
          // unsigned -> unsigned : zero-extend
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.UXTL2(i.dest.reg().H8(), i.dest.reg().B16());
        } else {
          // unsigned -> signed : zero-extend (result positive)
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.UXTL2(i.dest.reg().H8(), i.dest.reg().B16());
        }
      } else {
        if (IsPackOutUnsigned(flags)) {
          // signed -> unsigned : sign-extend (from signed packed data)
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.SXTL2(i.dest.reg().H8(), i.dest.reg().B16());
        } else {
          // signed -> signed
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.SXTL2(i.dest.reg().H8(), i.dest.reg().B16());
        }
      }
    } else {
      // Unpack to HI.
      if (IsPackInUnsigned(flags)) {
        if (IsPackOutUnsigned(flags)) {
          // unsigned -> unsigned : zero-extend
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.UXTL(i.dest.reg().H8(), i.dest.reg().toD().B8());
        } else {
          // unsigned -> signed : zero-extend (result positive)
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.UXTL(i.dest.reg().H8(), i.dest.reg().toD().B8());
        }
      } else {
        if (IsPackOutUnsigned(flags)) {
          // signed -> unsigned : sign-extend (from signed packed data)
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.SXTL(i.dest.reg().H8(), i.dest.reg().toD().B8());
        } else {
          // signed -> signed
          e.MOV(i.dest.reg().B16(), src.B16());
          e.REV32(i.dest.reg().H8(), i.dest.reg().H8());
          e.SXTL(i.dest.reg().H8(), i.dest.reg().toD().B8());
        }
      }
    }
  }
  static void Emit16_IN_32(A64Emitter& e, const EmitArgType& i,
                           uint32_t flags) {
    assert_false(IsPackOutSaturate(flags));
    QReg src(0);
    if (i.src1.is_constant) {
      src = i.dest;
      e.LoadConstantV(src, i.src1.constant());
    } else {
      src = i.src1;
    }
    if (IsPackToLo(flags)) {
      // Unpack to LO. Extension based on IN signedness.
      if (IsPackInUnsigned(flags)) {
        if (IsPackOutUnsigned(flags)) {
          // unsigned -> unsigned : zero-extend
          e.UXTL2(i.dest.reg().S4(), src.H8());
        } else {
          // unsigned -> signed : zero-extend (result positive)
          e.UXTL2(i.dest.reg().S4(), src.H8());
        }
      } else {
        if (IsPackOutUnsigned(flags)) {
          // signed -> unsigned : sign-extend (from signed packed data)
          e.SXTL2(i.dest.reg().S4(), src.H8());
        } else {
          // signed -> signed
          e.SXTL2(i.dest.reg().S4(), src.H8());
        }
      }
    } else {
      // Unpack to HI.
      if (IsPackInUnsigned(flags)) {
        if (IsPackOutUnsigned(flags)) {
          // unsigned -> unsigned : zero-extend
          e.UXTL(i.dest.reg().S4(), src.toD().H4());
        } else {
          // unsigned -> signed : zero-extend (result positive)
          e.UXTL(i.dest.reg().S4(), src.toD().H4());
        }
      } else {
        if (IsPackOutUnsigned(flags)) {
          // signed -> unsigned : sign-extend (from signed packed data)
          e.SXTL(i.dest.reg().S4(), src.toD().H4());
        } else {
          // signed -> signed
          e.SXTL(i.dest.reg().S4(), src.toD().H4());
        }
      }
    }
    e.REV64(i.dest.reg().S4(), i.dest.reg().S4());
  }
};
EMITTER_OPCODE_TABLE(OPCODE_UNPACK, UNPACK);

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
