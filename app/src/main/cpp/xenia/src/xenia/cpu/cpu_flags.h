/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_CPU_FLAGS_H_
#define XENIA_CPU_CPU_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_string(cpu);

DECLARE_string(load_module_map);

DECLARE_bool(disassemble_functions);

DECLARE_bool(trace_functions);
DECLARE_bool(trace_function_coverage);
DECLARE_bool(trace_function_references);
DECLARE_bool(trace_function_data);

DECLARE_bool(disable_global_lock);

DECLARE_bool(validate_hir);

DECLARE_uint64(pvr);

// Breakpoints:
DECLARE_uint64(break_on_instruction);
DECLARE_int32(break_condition_gpr);
DECLARE_uint64(break_condition_value);
DECLARE_string(break_condition_op);
DECLARE_bool(break_condition_truncate);

DECLARE_bool(break_on_debugbreak);

// A64 CPU Accuracy Debug cvar (DEFINE in a64_backend.cc).
// Exposed via DECLARE here for cross-module use in central PPC emit paths
// (TLB/ERAT NOP hardening) and base exception handler (ESR/DSISR/FSR synth
// improvements on data aborts). Wire everything under existing cvar per
// minimal hardening package from R2 TLB research (low title impact on flat
// addrspace + HLE model).
// TLB owner re-task: a64_accuracy_debug now also activates TLB-aware debug seqs
// (psq_st/res migration, TLB+psq prot faults, TLB+FPU 128B) inside ps harness.
// psq_l load skeleton re-task: also covers psq_l + lwarx 128B EA/res tracking seqs (symmetric).
// R1 validator-in-chief expansion: even richer title-derived + full-stack sequences
// (FMA/sub/sel, GQR VBO fidelity, psq+lockfree audio/physics+barriers, TLB shootdown,
// crown psq_l+sub/sel+psq_st+pairing+TLB cases) + 3 new R1-gap counters now exercised.
DECLARE_bool(a64_accuracy_debug);

#endif  // XENIA_CPU_CPU_FLAGS_H_
