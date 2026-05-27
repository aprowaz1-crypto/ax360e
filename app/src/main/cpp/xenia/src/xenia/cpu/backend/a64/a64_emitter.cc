/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_util.h"

#include <cstddef>

#include <climits>
#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/debugging.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/function_debug_info.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/symbol.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

#include "oaknut/feature_detection/cpu_feature.hpp"
#include "oaknut/feature_detection/feature_detection.hpp"
#include "oaknut/feature_detection/feature_detection_idregs.hpp"

//#include "aarch64_disasm.h"
std::string aarch64_disasm(uint64_t base, uint32_t* code,size_t count);

DEFINE_bool(debugprint_trap_log, false,
            "Log debugprint traps to the active debugger", "CPU");
DEFINE_bool(ignore_undefined_externs, true,
            "Don't exit when an undefined extern is called.", "CPU");

//通过查找NOP并替换指令实现地址重定位，因此需要禁用该选项
#if 0
DEFINE_bool(emit_source_annotations, false,
            "Add extra movs and nops to make disassembly easier to read.",
            "CPU");
#endif
namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using namespace xe::literals;
using namespace oaknut::util;

static const size_t kStashOffset = 32;
// static const size_t kStashOffsetHigh = 32 + 32;

// Register indices that the HIR is allowed to use for operands
const uint8_t A64Emitter::gpr_reg_map_[A64Emitter::GPR_COUNT] = {
    19, 20, 21, 22, 23, 24, 25, 26,
};

const uint8_t A64Emitter::fpr_reg_map_[A64Emitter::FPR_COUNT] = {
    8, 9, 10, 11, 12, 13, 14, 15,
};

A64Emitter::A64Emitter(A64Backend* backend)
    : VectorCodeGenerator(assembly_buffer),
      processor_(backend->processor()),
      backend_(backend),
      code_cache_(backend->code_cache()) {
  oaknut::CpuFeatures cpu_ = oaknut::detect_features();

  // Combine with id register detection
#if OAKNUT_SUPPORTS_READING_ID_REGISTERS > 0
#if OAKNUT_SUPPORTS_READING_ID_REGISTERS == 1
  const std::optional<oaknut::id::IdRegisters> id_registers =
      oaknut::read_id_registers();
#elif OAKNUT_SUPPORTS_READING_ID_REGISTERS == 2
  const std::optional<oaknut::id::IdRegisters> id_registers =
      oaknut::read_id_registers(0);
#endif
  if (id_registers.has_value()) {
    cpu_ = cpu_ | oaknut::detect_features_via_id_registers(*id_registers);
  }
#endif

#define TEST_EMIT_FEATURE(emit, ext)                \
  if ((cvars::a64_extension_mask & emit) == emit) { \
    feature_flags_ |= (cpu_.has(ext) ? emit : 0);   \
  }

  TEST_EMIT_FEATURE(kA64EmitLSE, oaknut::CpuFeature::LSE);
  TEST_EMIT_FEATURE(kA64EmitF16C, oaknut::CpuFeature::FP16Conv);

#undef TEST_EMIT_FEATURE
}

A64Emitter::~A64Emitter() = default;

bool A64Emitter::Emit(GuestFunction* function, HIRBuilder* builder,
                      uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
                      void** out_code_address, size_t* out_code_size,
                      std::vector<SourceMapEntry>* out_source_map) {
  SCOPE_profile_cpu_f("cpu");

  // Reset.
  debug_info_ = debug_info;
  debug_info_flags_ = debug_info_flags;
  trace_data_ = &function->trace_data();
  source_map_arena_.Reset();
  current_instr_ = nullptr;
  last_guest_address_ = 0;
  last_hir_offset_ = 0;

  // Fill the generator with code.
  EmitFunctionInfo func_info = {};
  if (!Emit(builder, func_info)) {
    return false;
  }

  // Copy the final code to the cache and relocate it.
  *out_code_size = offset();
  *out_code_address = Emplace(func_info, function);

  // Stash source map.
  source_map_arena_.CloneContents(out_source_map);

  return true;
}

void* A64Emitter::Emplace(const EmitFunctionInfo& func_info,
                          GuestFunction* function) {

  // Copy the current oaknut instruction-buffer into the code-cache
  void* new_execute_address;
  void* new_write_address;

  assert_true(func_info.code_size.total == offset());

  if (function) {
    code_cache_->PlaceGuestCode(function->address(), assembly_buffer.data(),
                                func_info, function, new_execute_address,
                                new_write_address);
  } else {
    code_cache_->PlaceHostCode(0, assembly_buffer.data(), func_info,
                               new_execute_address, new_write_address);
  }

#if 0
    code_cache_->fix_opcodes_.clear();
#endif
  // Reset the oaknut instruction-buffer
  assembly_buffer.clear();
  label_lookup_.clear();

  return new_execute_address;
}

bool A64Emitter::Emit(HIRBuilder* builder, EmitFunctionInfo& func_info) {
  oaknut::Label epilog_label;
  epilog_label_ = &epilog_label;

  // Calculate stack size. We need to align things to their natural sizes.
  // This could be much better (sort by type/etc).
  auto locals = builder->locals();
  size_t stack_offset = StackLayout::GUEST_STACK_SIZE;
  for (auto it = locals.begin(); it != locals.end(); ++it) {
    auto slot = *it;
    size_t type_size = GetTypeSize(slot->type);

    // Align to natural size.
    stack_offset = xe::align(stack_offset, type_size);
    slot->set_constant((uint32_t)stack_offset);
    stack_offset += type_size;
  }

  // Ensure 16b alignment.
  stack_offset -= StackLayout::GUEST_STACK_SIZE;
  stack_offset = xe::align(stack_offset, static_cast<size_t>(16));

  struct _code_offsets {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = offset();

  // Function prolog.
  // Must be 16b aligned.
  // Windows is very strict about the form of this and the epilog:
  // https://docs.microsoft.com/en-us/cpp/build/prolog-and-epilog?view=vs-2017
  // IMPORTANT: any changes to the prolog must be kept in sync with
  //     A64CodeCache, which dynamically generates exception information.
  //     Adding or changing anything here must be matched!
  size_t stack_size = StackLayout::GUEST_STACK_SIZE + stack_offset;

  // The SUB instruction can only encode immediates withi 0xFFF or 0xFFF000
  // If the stack size is greater than 0xFFF, then just align it to 0x1000
  if (stack_size > 0xFFF) {
    stack_size = xe::align(stack_size, static_cast<size_t>(0x1000));
  }

  assert_true(stack_size % 16 == 0);
  func_info.stack_size = stack_size;
  stack_size_ = stack_size;

    PushStackpoint();
  STP(X29, X30, SP, PRE_INDEXED, -16);
  MOV(X29, SP);

  SUB(SP, SP, (uint32_t)stack_size);

  code_offsets.prolog_stack_alloc = offset();
  code_offsets.body = offset();

  STR(GetContextReg(), SP, StackLayout::GUEST_CTX_HOME);
  STR(X0, SP, StackLayout::GUEST_RET_ADDR);
  STR(XZR, SP, StackLayout::GUEST_CALL_RET_ADDR);

  // Safe now to do some tracing.
  if (debug_info_flags_ & DebugInfoFlags::kDebugInfoTraceFunctions) {

    // We require 32-bit addresses.
    assert_true(uint64_t(trace_data_->header()) < UINT_MAX);
    auto trace_header = trace_data_->header();

    // Call count.
    MOV(W0, 1);
    MOV(X5, reinterpret_cast<uintptr_t>(
                low_address(&trace_header->function_call_count)));
    LDADDAL(X0, X0, X5);

    // Get call history slot.
    static_assert(FunctionTraceData::kFunctionCallerHistoryCount == 4,
                  "bitmask depends on count");
    LDR(X0, X5);
    AND(W0, W0, 0b00000011);

    // Record call history value into slot (guest addr in W1).
    MOV(X5, reinterpret_cast<uintptr_t>(
                low_address(&trace_header->function_caller_history)));
    STR(W1, X5, X0, oaknut::IndexExt::LSL, 2);

    // Calling thread. Load X0 with thread ID.
    EmitGetCurrentThreadId();
    MOV(W5, 1);
    LSL(W0, W5, W0);

    MOV(X5, reinterpret_cast<uintptr_t>(
                low_address(&trace_header->function_thread_use)));
    LDSET(W0, WZR, X5);
  }

  // Load membase.
  LDR(GetMembaseReg(), GetContextReg(),
      offsetof(ppc::PPCContext, virtual_membase));

  // Body.
  auto block = builder->first_block();

    synchronize_stack_on_next_instruction_ = false;
  while (block) {
    // Mark block labels.
    auto label = block->label_head;
    while (label) {
      l(label_lookup_[label->name]);
      label = label->next;
    }

    // Process instructions.
    const Instr* instr = block->instr_head;
    while (instr) {
        current_instr_ = const_cast<Instr*>(instr);
        if (synchronize_stack_on_next_instruction_) {
            if (instr->opcode->num != hir::OPCODE_SOURCE_OFFSET) {
                synchronize_stack_on_next_instruction_ = false;
                EnsureSynchronizedGuestAndHostStack();
            }
        }
      const Instr* new_tail = instr;
      if (!SelectSequence(this, instr, &new_tail)) {
        // No sequence found! Use the centralized unhandled system.
        ReportUnhandledA64Opcode(this, instr, "SelectSequence");
        EmitStructuredFallback(*this, instr);

        if (cvars::a64_accuracy_debug) {
          XELOGE("A64 Accuracy Debug: Unhandled HIR opcode surfaced via structured fallback + trap.");
          UnimplementedInstr(instr);  // will BRK in debug mode
        }
        // In normal mode we continue (with zeroed dest) instead of hard crashing the guest.
        // Stop this block to avoid cascading bad codegen.
        break;
      }
      instr = new_tail;
    }

    block = block->next;
  }
  current_instr_ = nullptr;

  // Function epilog.
  l(epilog_label);
  epilog_label_ = nullptr;
  EmitTraceUserCallReturn();
  LDR(GetContextReg(), SP, StackLayout::GUEST_CTX_HOME);

  code_offsets.epilog = offset();

  ADD(SP, SP, (uint32_t)stack_size);
    PopStackpoint();
  MOV(SP, X29);
  LDP(X29, X30, SP, POST_INDEXED, 16);

  RET();

  code_offsets.tail = offset();
#if 0
  if (cvars::emit_source_annotations) {
    NOP();
    NOP();
    NOP();
    NOP();
    NOP();
  }
#endif
  assert_zero(code_offsets.prolog);
  func_info.code_size.total = offset();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = offset() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;

  return true;
}

void A64Emitter::MarkSourceOffset(const Instr* i) {
  auto entry = source_map_arena_.Alloc<SourceMapEntry>();
  entry->guest_address = static_cast<uint32_t>(i->src1.offset);
  entry->hir_offset = uint32_t(i->block->ordinal << 16) | i->ordinal;
  entry->code_offset = static_cast<uint32_t>(offset());
  last_guest_address_ = entry->guest_address;
  last_hir_offset_ = entry->hir_offset;
#if 0
  if (cvars::emit_source_annotations) {
    NOP();
    NOP();
    MOV(X0, entry->guest_address);
    NOP();
    NOP();
  }
#endif
  if (debug_info_flags_ & DebugInfoFlags::kDebugInfoTraceFunctionCoverage) {
    const uint32_t instruction_index =
        (entry->guest_address - trace_data_->start_address()) / 4;
    MOV(X0, 1);
    MOV(X1, reinterpret_cast<uintptr_t>(
                low_address(trace_data_->instruction_execute_counts() +
                            instruction_index * 8)));
    LDADDAL(X0, ZR, X1);
  }
}

void A64Emitter::EmitGetCurrentThreadId() {
  // X27 must point to context. We could fetch from the stack if needed.
  LDRH(W0, GetContextReg(), offsetof(ppc::PPCContext, thread_id));
}

void A64Emitter::EmitTraceUserCallReturn() {}

void A64Emitter::DebugBreak() { BRK(0xF000); }

uint64_t TrapDebugPrint(void* raw_context, uint64_t address) {
  auto thread_state = *reinterpret_cast<ThreadState**>(raw_context);
  uint32_t str_ptr = uint32_t(thread_state->context()->r[3]);
  // uint16_t str_len = uint16_t(thread_state->context()->r[4]);
  auto str = thread_state->memory()->TranslateVirtual<const char*>(str_ptr);
  // TODO(benvanik): truncate to length?
  XELOGD("(DebugPrint) {}", str);

  if (cvars::debugprint_trap_log) {
    debugging::DebugPrint("(DebugPrint) {}", str);
  }

  return 0;
}

uint64_t TrapDebugBreak(void* raw_context, uint64_t address) {
  auto thread_state = *reinterpret_cast<ThreadState**>(raw_context);
  XELOGE("tw/td forced trap hit! This should be a crash!");
  if (cvars::break_on_debugbreak) {
    xe::debugging::Break();
  }
  return 0;
}

void A64Emitter::Trap(uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26:
      // 0x0FE00014 is a 'debug print' where r3 = buffer r4 = length
      CallNative(TrapDebugPrint, 0);
      break;
    case 0:
    case 22:
      // Always trap?
      // TODO(benvanik): post software interrupt to debugger.
      CallNative(TrapDebugBreak, 0);
      break;
    case 25:
      // ?
      break;
    default:
      XELOGW("Unknown trap type {}", trap_type);
      BRK(0xF000);
      break;
  }
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  // This is now primarily the "loud" path used in accuracy_debug mode.
  // Normal operation prefers ReportUnhandledA64Opcode + EmitStructuredFallback.
  const char* op_name = i && i->opcode ? i->opcode->name : "<unknown>";
  XELOGE("A64: Emitting hard UNIMPLEMENTED/TRAP (accuracy_debug path) for HIR instr: {}", op_name);

  if (cvars::a64_accuracy_debug) {
    XELOGE("A64 Accuracy Debug: Conservative path hit. Guest will see SIGILL/BRK. "
           "Use logcat + recent unhandled report for diagnosis.");
  }

  // Hard trap only in accuracy/debug mode. In normal runs the structured
  // fallback above already gave us a chance to continue.
  BRK(0xF000);

  if (cvars::a64_accuracy_debug) {
    assert_always("A64 unhandled instr in accuracy debug mode");
  }
}

// This is used by the A64ThunkEmitter's ResolveFunctionThunk.
uint64_t ResolveFunction(void* raw_context, uint64_t target_address) {
            auto guest_context = reinterpret_cast<ppc::PPCContext_s*>(raw_context);

            auto thread_state = guest_context->thread_state;

            // TODO(benvanik): required?
            assert_not_zero(target_address);

            /*
                    todo: refactor this!

                   The purpose of this code is to allow guest longjmp to call into
               the body of an existing host function. There are a lot of conditions we
               have to check here to ensure that we do not mess up a normal call to a
               function

                   The address must be within an XexModule (may need to make some changes
               to instructionaddressflags to remove this limitation) The target address
               must be a known return site. The guest address must be part of a function
               that was already translated.

            */

            if (cvars::enable_host_guest_stack_synchronization) {
#if 0
                auto processor = thread_state->processor();

                auto module_for_address =
                        processor->LookupModule(static_cast<uint32_t>(target_address));

                if (module_for_address) {
                    XexModule* xexmod = dynamic_cast<XexModule*>(module_for_address);
                    if (xexmod) {
                        InfoCacheFlags* flags = xexmod->GetInstructionAddressFlags(
                                static_cast<uint32_t>(target_address));
                        if (flags) {
                            if (flags->is_return_site) {
                                auto ones_with_address = processor->FindFunctionsWithAddress(
                                        static_cast<uint32_t>(target_address));

                                if (ones_with_address.size() != 0) {
                                    // this loop to find a host address for the guest address is
                                    // necessary because FindFunctionsWithAddress works via a range
                                    // check, but if the function consists of multiple blocks
                                    // scattered around with "holes" of instructions that cannot be
                                    // reached in between those holes the instructions that cannot be
                                    // reached will incorrectly be considered members of the function

                                    A64Function* candidate = nullptr;
                                    uintptr_t host_address = 0;
                                    for (auto&& entry : ones_with_address) {
                                        A64Function* xfunc = static_cast<A64Function*>(entry);

                                        host_address = xfunc->MapGuestAddressToMachineCode(
                                                static_cast<uint32_t>(target_address));
                                        // host address does exist within the function, and that host
                                        // function is not the start of the function, it is instead
                                        // somewhere within its existing body
                                        // i originally did not have this (xfunc->machine_code() !=
                                        // reinterpret_cast<const uint8_t*>(host_address))) condition
                                        // here when i distributed builds for testing, no issues arose
                                        // related to it but i wanted to be more explicit
                                        if (host_address &&
                                            xfunc->machine_code() !=
                                            reinterpret_cast<const uint8_t*>(host_address)) {
                                            candidate = xfunc;
                                            break;
                                        }
                                    }
                                    // we found an existing X64Function, and a return site within that
                                    // function that has a host address w/ native code
                                    if (candidate && host_address) {
                                        A64Backend* backend =
                                                static_cast<A64Backend*>(processor->backend());
                                        // grab the backend context, next we have to check whether the
                                        // guest and host stack are out of sync if they arent, its fine
                                        // for the backend to create a new function for the guest
                                        // address we're resolving if they are, it means that the reason
                                        // we're resolving this address is because context is being
                                        // restored (probably by longjmp)
                                        A64BackendContext* backend_context =
                                                backend->BackendContextForGuestContext(guest_context);

                                        uint32_t current_stackpoint_index =
                                                backend_context->current_stackpoint_depth;

                                        --current_stackpoint_index;

                                        A64BackendStackpoint* stackpoints =
                                                backend_context->stackpoints;

                                        uint32_t current_guest_stackpointer =
                                                static_cast<uint32_t>(guest_context->r[1]);
                                        uint32_t num_frames_bigger = 0;

                                        /*
                                                if the current guest stack pointer is bigger than the
                                           recorded pointer for this stack thats fine, plenty of
                                           functions restore the original stack pointer early

                                                if more than 1... we're longjmping and sure of it at
                                           this point (jumping to a return site that has already been
                                           emitted)
                                        */
                                        while (current_stackpoint_index != 0xFFFFFFFF) {
                                            if (current_guest_stackpointer >
                                                stackpoints[current_stackpoint_index].guest_stack_) {
                                                --current_stackpoint_index;
                                                ++num_frames_bigger;

                                            } else {
                                                break;
                                            }
                                        }
                                        /*
                                                                DEFINITELY a longjmp, return original
                                           host address. returning the existing host address is going to
                                           set off some extra machinery we have set up to support this

                                                                to break it down, our caller (us being
                                           this ResolveFunction that this comment is in) is
                                           X64Backend::resolve_function_thunk_ which is implemented in
                                           x64_backend.cc X64HelperEmitter::EmitResolveFunctionThunk, or
                                           a call from the resolver table

                                                                the x64 fastcall abi dictates that the
                                           stack must always be 16 byte aligned. We select our stack
                                           size for functions to ensure that we keep rsp aligned to 16
                                           bytes

                                                                but by calling into the body of an
                                           existing function we've pushed our return address onto the
                                           stack (dont worry about this return address, it gets
                                           discarded in a later step)

                                                                this means that the stack is no longer
                                           16 byte aligned, (rsp % 16) now == 8, and this is the only
                                           time outside of the prolog or epilog of a function that this
                                           will be the case

                                                                so, after all direct or indirect
                                           function calls we set
                                           X64Emitter::synchronize_stack_on_next_instruction_ to true.
                                                                On the next instruction that is not
                                           OPCODE_SOURCE_OFFSET we will emit a check when we see
                                           synchronize_stack_on_next_instruction_ is true. We have to
                                           skip OPCODE_SOURCE_OFFSET because its not a "real"
                                           instruction and if we emit on it the return address of the
                                           function call will point to AFTER our check, so itll never be
                                           executed.

                                                                our check is just going to do test esp,
                                           15 to see if the stack is misaligned. (using esp instead of
                                           rsp saves 1 byte). We tail emit the handling for when the
                                           check succeeds because in 99.99999% of function calls it will
                                           be aligned, in the end the runtime cost of these checks is 5
                                           bytes for the test instruction which ought to be one cycle
                                           and 5 bytes for the jmp with no cycles taken for the jump
                                           which will be predicted not taken.

                                          Our handling for the check is implemented in
                                          X64HelperEmitter::EmitGuestAndHostSynchronizeStackHelper. we
                                          don't call it directly though, instead we go through
                                          backend()->synchronize_guest_and_host_stack_helper_for_size(num_bytes_needed_to_represent_stack_size).
                                          we place the stack size after the call instruction so we can
                                          load it in the helper and readjust the return address to point
                                          after the literal value.

                                                          The helper is going to search the array of
                                          stackpoints to find the first one that is greater than or
                                          equal to the current stack pointer, when it finds the entry it
                                          will set the currently host rsp to the host stack pointer
                                          value in the entry, and then subtract the stack size of the
                                          caller from that. the current stackpoint index is adjusted to
                                          point to the one after the stackpoint we restored to.

                                                          The helper then jumps back to the function
                                          that was longjmp'ed to, with the host stack in its proper
                                          state. it just works!



                                         */

                                        if (num_frames_bigger > 1) {
                                            /*
                                             * can't do anything about this right now :(
                                             * epic mickey is quite slow due to having to call resolve on
                                             * every longjmp, and it longjmps a lot but if we add an
                                             * indirection we lose our stack misalignment check
                                             */
                                            /* reinterpret_cast<X64CodeCache*>(backend->code_cache())
                                ->AddIndirection(static_cast<uint32_t>(target_address),
                                                 static_cast<uint32_t>(host_address));
                                                  */

                                            return host_address;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
#endif
            }
            auto fn = thread_state->processor()->ResolveFunction(
                    static_cast<uint32_t>(target_address));
            assert_not_null(fn);
            auto x64_fn = static_cast<A64Function*>(fn);
            uint64_t addr = reinterpret_cast<uint64_t>(x64_fn->machine_code());

            return addr;
}

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  assert_not_null(function);
  auto fn = static_cast<A64Function*>(function);
  // Resolve address to the function to call and store in X16.
  if (fn->machine_code()) {

    // TODO(benvanik): is it worth it to do this? It removes the need for
    // a ResolveFunction call, but makes the table less useful.
    //assert_zero(uint64_t(fn->machine_code()) & 0xFFFFFFFF00000000);
    MOV(X16, uint64_t(fn->machine_code()));
  } else if (code_cache_->has_indirection_table()) {

    // Load the pointer to the indirection table maintained in A64CodeCache.
    // The target dword will either contain the address of the generated code
    // or a thunk to ResolveAddress.
    MOV(X17, function->address());
    //code_cache_->fix_opcodes_.insert(InstFix::REPLACE_TO_MOVK_X17_IMM16_SHIFT32(offset()));

    NOP();
    LDR(W16, X17);
    //code_cache_->fix_opcodes_.insert(InstFix::REPLACE_TO_MOVK_X16_IMM16_SHIFT32(offset()));

    NOP();
  } else {
    // Old-style resolve.
    // Not too important because indirection table is almost always available.
    // TODO: Overwrite the call-site with a straight call.
    CallNative(&ResolveFunction, function->address());
    MOV(X16, X0);
  }

  // Actually jump/call to X16.
  if (instr->flags & hir::CALL_TAIL) {
    // Since we skip the prolog we need to mark the return here.
    EmitTraceUserCallReturn();

    // Pass the callers return address over.
    LDR(X0, SP, StackLayout::GUEST_RET_ADDR);

    ADD(SP, SP, static_cast<uint32_t>(stack_size()));

      PopStackpoint();
    MOV(SP, X29);
    LDP(X29, X30, SP, POST_INDEXED, 16);

    BR(X16);
  } else {
    // Return address is from the previous SET_RETURN_ADDRESS.
    LDR(X0, SP, StackLayout::GUEST_CALL_RET_ADDR);

    BLR(X16);

      synchronize_stack_on_next_instruction_ = true;
  }
}

void A64Emitter::CallIndirect(const hir::Instr* instr,
                              const oaknut::XReg& reg) {
  // Check if return.
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    LDR(W16, SP, StackLayout::GUEST_RET_ADDR);
    CMP(reg.toW(), W16);
    B(oaknut::Cond::EQ, epilog_label());
  }

  // Load the pointer to the indirection table maintained in A64CodeCache.
  // The target dword will either contain the address of the generated code
  // or a thunk to ResolveAddress.
  if (code_cache_->has_indirection_table()) {
    if (reg.toW().index() != W17.index()) {
      MOV(W17, reg.toW());
    }

    //code_cache_->fix_opcodes_.insert(InstFix::REPLACE_TO_MOVK_X17_IMM16_SHIFT32(offset()));

    NOP();

    LDR(W16, X17);
    //code_cache_->fix_opcodes_.insert(InstFix::REPLACE_TO_MOVK_X16_IMM16_SHIFT32(offset()));

    NOP();
  } else {
    // Old-style resolve.
    // Not too important because indirection table is almost always available.
    MOV(X0, GetContextReg());
    MOV(W1, reg.toW());

    MOV(X16, reinterpret_cast<uint64_t>(ResolveFunction));
    BLR(X16);
    MOV(X16, X0);
  }

  // Actually jump/call to X16.
  if (instr->flags & hir::CALL_TAIL) {
    // Since we skip the prolog we need to mark the return here.
    EmitTraceUserCallReturn();

    // Pass the callers return address over.
    LDR(X0, SP, StackLayout::GUEST_RET_ADDR);

    ADD(SP, SP, static_cast<uint32_t>(stack_size()));

      PopStackpoint();
    MOV(SP, X29);
    LDP(X29, X30, SP, POST_INDEXED, 16);

    BR(X16);
  } else {
    // Return address is from the previous SET_RETURN_ADDRESS.
    LDR(X0, SP, StackLayout::GUEST_CALL_RET_ADDR);

    BLR(X16);

      synchronize_stack_on_next_instruction_ = true;
  }
}

uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {
  auto function = reinterpret_cast<Function*>(function_ptr);
  if (!cvars::ignore_undefined_externs) {
    xe::FatalError(fmt::format("undefined extern call to {:08X} {}",
                               function->address(), function->name().c_str()));
  } else {
    XELOGE("undefined extern call to {:08X} {}", function->address(),
           function->name());
  }
  // Return a generic failure code (0x80004005 = E_FAIL) so games don't
  // interpret the result as success and spin in tight polling loops.
  // Also sleep briefly to prevent CPU-burning retry loops.
  xe::threading::Sleep(std::chrono::milliseconds(16));
  return 0x80004005;
}
void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  bool undefined = true;
  if (function->behavior() == Function::Behavior::kBuiltin) {
    auto builtin_function = static_cast<const BuiltinFunction*>(function);
    if (builtin_function->handler()) {
      undefined = false;
      // x0 = target function
      // x1 = arg0
      // x2 = arg1
      // x3 = arg2
      MOV(X0, reinterpret_cast<uint64_t>(builtin_function->handler()));
      MOV(X1, reinterpret_cast<uint64_t>(builtin_function->arg0()));
      MOV(X2, reinterpret_cast<uint64_t>(builtin_function->arg1()));

      auto thunk = backend()->guest_to_host_thunk();
      MOV(X16, reinterpret_cast<uint64_t>(thunk));
      BLR(X16);

      // x0 = host return
    }
  } else if (function->behavior() == Function::Behavior::kExtern) {
    auto extern_function = static_cast<const GuestFunction*>(function);
    if (extern_function->extern_handler()) {
      undefined = false;
      // x0 = target function
      // x1 = arg0
      // x2 = arg1
      // x3 = arg2
      MOV(X0, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
      LDR(X1, GetContextReg(), offsetof(ppc::PPCContext, kernel_state));

      auto thunk = backend()->guest_to_host_thunk();
      MOV(X16, reinterpret_cast<uint64_t>(thunk));
      BLR(X16);

      // x0 = host return
    }
  }
  if (undefined) {
    CallNative(UndefinedCallExtern, reinterpret_cast<uint64_t>(function));
  }
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNative(uint64_t (*fn)(void* raw_context)) {
  CallNativeSafe(reinterpret_cast<void*>(fn));
}

void A64Emitter::CallNative(uint64_t (*fn)(void* raw_context, uint64_t arg0)) {
  CallNativeSafe(reinterpret_cast<void*>(fn));
}

void A64Emitter::CallNative(uint64_t (*fn)(void* raw_context, uint64_t arg0),
                            uint64_t arg0) {
  MOV(GetNativeParam(0), arg0);
  CallNativeSafe(reinterpret_cast<void*>(fn));
}

void A64Emitter::CallNativeSafe(void* fn) {
  // X0 = target function
  // X1 = arg0
  // X2 = arg1
  // X3 = arg2
  auto thunk = backend()->guest_to_host_thunk();

  MOV(X0, reinterpret_cast<uint64_t>(fn));

  MOV(X16, reinterpret_cast<uint64_t>(thunk));
  BLR(X16);

  // X0 = host return
}

void A64Emitter::SetReturnAddress(uint64_t value) {
  MOV(X0, value|code_cache_->execute_address_high());
  STR(X0, SP, StackLayout::GUEST_CALL_RET_ADDR);
}

oaknut::XReg A64Emitter::GetNativeParam(uint32_t param) {
  if (param == 0)
    return X1;
  else if (param == 1)
    return X2;
  else if (param == 2)
    return X3;

  assert_always();
  return X3;
}

// Important: If you change these, you must update the thunks in a64_backend.cc!
oaknut::XReg A64Emitter::GetContextReg() { return X27; }
oaknut::XReg A64Emitter::GetMembaseReg() { return X28; }

void A64Emitter::ReloadContext() {
  LDR(GetContextReg(), SP, StackLayout::GUEST_CTX_HOME);
}

void A64Emitter::ReloadMembase() {
  LDR(GetMembaseReg(), GetContextReg(),
      offsetof(ppc::PPCContext, virtual_membase));
}

bool A64Emitter::ConstantFitsIn32Reg(uint64_t v) {
  if ((v & ~0x7FFFFFFF) == 0) {
    // Fits under 31 bits, so just load using normal mov.
    return true;
  } else if ((v & ~0x7FFFFFFF) == ~0x7FFFFFFF) {
    // Negative number that fits in 32bits.
    return true;
  }
  return false;
}

void A64Emitter::MovMem64(const oaknut::XRegSp& addr, intptr_t offset,
                          uint64_t v) {
  if (v == 0) {
    STR(XZR, addr, offset);
  } else if (!(v >> 32)) {
    // All high bits are zero, 32-bit MOV
    MOV(W0, static_cast<uint32_t>(v));
    STR(X0, addr, offset);
  } else {
    // 64bit number that needs double movs.
    MOV(X0, v);
    STR(X0, addr, offset);
  }
}

static const vec128_t v_consts[] = {
    /* VZero                */ vec128f(0.0f),
    /* VOnePD               */ vec128d(1.0),
    /* VNegativeOne         */ vec128f(-1.0f, -1.0f, -1.0f, -1.0f),
    /* VFFFF                */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VMaskX16Y16          */
    vec128i(0x0000FFFFu, 0xFFFF0000u, 0x00000000u, 0x00000000u),
    /* VFlipX16Y16          */
    vec128i(0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u),
    /* VFixX16Y16           */ vec128f(-32768.0f, 0.0f, 0.0f, 0.0f),
    /* VNormalizeX16Y16     */
    vec128f(1.0f / 32767.0f, 1.0f / (32767.0f * 65536.0f), 0.0f, 0.0f),
    /* V0001                */ vec128f(0.0f, 0.0f, 0.0f, 1.0f),
    /* V3301                */ vec128f(3.0f, 3.0f, 0.0f, 1.0f),
    /* V3331                */ vec128f(3.0f, 3.0f, 3.0f, 1.0f),
    /* V3333                */ vec128f(3.0f, 3.0f, 3.0f, 3.0f),
    /* VSignMaskPS          */
    vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    /* VSignMaskPD          */
    vec128i(0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u),
    /* VAbsMaskPS           */
    vec128i(0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu),
    /* VAbsMaskPD           */
    vec128i(0xFFFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu),
    /* VByteSwapMask        */
    vec128i(0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu),
    /* VByteOrderMask       */
    vec128i(0x01000302u, 0x05040706u, 0x09080B0Au, 0x0D0C0F0Eu),
    /* VPermuteControl15    */ vec128b(15),
    /* VPermuteByteMask     */ vec128b(0x1F),
    /* VPackD3DCOLORSat     */ vec128i(0x404000FFu),
    /* VPackD3DCOLOR        */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0C000408u),
    /* VUnpackD3DCOLOR      */
    vec128i(0xFFFFFF0Eu, 0xFFFFFF0Du, 0xFFFFFF0Cu, 0xFFFFFF0Fu),
    /* VPackFLOAT16_2       */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000302u),
    /* VUnpackFLOAT16_2     */
    vec128i(0x0D0C0F0Eu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VPackFLOAT16_4       */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000302u, 0x05040706u),
    /* VUnpackFLOAT16_4     */
    vec128i(0x09080B0Au, 0x0D0C0F0Eu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VPackSHORT_Min       */ vec128i(0x403F8001u),
    /* VPackSHORT_Max       */ vec128i(0x40407FFFu),
    /* VPackSHORT_2         */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000504u),
    /* VPackSHORT_4         */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000504u, 0x09080D0Cu),
    /* VUnpackSHORT_2       */
    vec128i(0xFFFF0F0Eu, 0xFFFF0D0Cu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VUnpackSHORT_4       */
    vec128i(0xFFFF0B0Au, 0xFFFF0908u, 0xFFFF0F0Eu, 0xFFFF0D0Cu),
    /* VUnpackSHORT_Overflow */ vec128i(0x403F8000u),
    /* VPackUINT_2101010_MinUnpacked */
    vec128i(0x403FFE01u, 0x403FFE01u, 0x403FFE01u, 0x40400000u),
    /* VPackUINT_2101010_MaxUnpacked */
    vec128i(0x404001FFu, 0x404001FFu, 0x404001FFu, 0x40400003u),
    /* VPackUINT_2101010_MaskUnpacked */
    vec128i(0x3FFu, 0x3FFu, 0x3FFu, 0x3u),
    /* VPackUINT_2101010_MaskPacked */
    vec128i(0x3FFu, 0x3FFu << 10, 0x3FFu << 20, 0x3u << 30),
    /* VPackUINT_2101010_Shift */ vec128i(0, 10, 20, 30),
    /* VUnpackUINT_2101010_Overflow */ vec128i(0x403FFE00u),
    /* VPackULONG_4202020_MinUnpacked */
    vec128i(0x40380001u, 0x40380001u, 0x40380001u, 0x40400000u),
    /* VPackULONG_4202020_MaxUnpacked */
    vec128i(0x4047FFFFu, 0x4047FFFFu, 0x4047FFFFu, 0x4040000Fu),
    /* VPackULONG_4202020_MaskUnpacked */
    vec128i(0xFFFFFu, 0xFFFFFu, 0xFFFFFu, 0xFu),
    /* VPackULONG_4202020_PermuteXZ */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x0A0908FFu, 0xFF020100u),
    /* VPackULONG_4202020_PermuteYW */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x0CFFFF06u, 0x0504FFFFu),
    /* VUnpackULONG_4202020_Permute */
    vec128i(0xFF0E0D0Cu, 0xFF0B0A09u, 0xFF080F0Eu, 0xFFFFFF0Bu),
    /* VUnpackULONG_4202020_Overflow */ vec128i(0x40380000u),
    /* VOneOver255          */ vec128f(1.0f / 255.0f),
    /* VMaskEvenPI16        */
    vec128i(0x0000FFFFu, 0x0000FFFFu, 0x0000FFFFu, 0x0000FFFFu),
    /* VShiftMaskEvenPI16   */
    vec128i(0x0000000Fu, 0x0000000Fu, 0x0000000Fu, 0x0000000Fu),
    /* VShiftMaskPS         */
    vec128i(0x0000001Fu, 0x0000001Fu, 0x0000001Fu, 0x0000001Fu),
    /* VShiftByteMask       */
    vec128i(0x000000FFu, 0x000000FFu, 0x000000FFu, 0x000000FFu),
    /* VSwapWordMask        */
    vec128i(0x03030303u, 0x03030303u, 0x03030303u, 0x03030303u),
    /* VUnsignedDwordMax    */
    vec128i(0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u),
    /* V255                 */ vec128f(255.0f),
    /* VPI32                */ vec128i(32),
    /* VSignMaskI8          */
    vec128i(0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u),
    /* VSignMaskI16         */
    vec128i(0x80008000u, 0x80008000u, 0x80008000u, 0x80008000u),
    /* VSignMaskI32         */
    vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    /* VSignMaskF32         */
    vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    /* VShortMinPS          */ vec128f(SHRT_MIN),
    /* VShortMaxPS          */ vec128f(SHRT_MAX),
    /* VIntMin              */ vec128i(INT_MIN),
    /* VIntMax              */ vec128i(INT_MAX),
    /* VIntMaxPD            */ vec128d(INT_MAX),
    /* VPosIntMinPS         */ vec128f((float)0x80000000u),
    /* VQNaN                */ vec128i(0x7FC00000u),
    /* VInt127              */ vec128i(0x7Fu),
    /* V2To32               */ vec128f(0x1.0p32f),
};

// First location to try and place constants.
static const uintptr_t kConstDataLocationLow = 0x20000000;
static const uintptr_t kConstDataSize = sizeof(v_consts);

// Increment the location by this amount for every allocation failure.
static const uintptr_t kConstDataIncrement = 0x00001000;

// This function places constant data that is used by the emitter later on.
// Only called once and used by multiple instances of the emitter.
//
// TODO(DrChat): This should be placed in the code cache with the code, but
// doing so requires RIP-relative addressing, which is difficult to support
// given the current setup.
uintptr_t A64Emitter::PlaceConstData(uintptr_t high_address) {
  uint8_t* ptr = reinterpret_cast<uint8_t*>(high_address|kConstDataLocationLow);
  void* mem = nullptr;
  while (!mem) {
    mem = memory::AllocFixed(
        ptr, xe::round_up(kConstDataSize, memory::page_size()),
        memory::AllocationType::kReserveCommit, memory::PageAccess::kReadWrite);

    ptr += kConstDataIncrement;
  }

  // The pointer must not be greater than 31 bits.
  //assert_zero(reinterpret_cast<uintptr_t>(mem) & ~0x7FFFFFFF);
  std::memcpy(mem, v_consts, sizeof(v_consts));
  memory::Protect(mem, kConstDataSize, memory::PageAccess::kReadOnly, nullptr);

  return reinterpret_cast<uintptr_t>(mem);
}

void A64Emitter::FreeConstData(uintptr_t data) {
  memory::DeallocFixed(reinterpret_cast<void*>(data), 0,
                       memory::DeallocationType::kRelease);
}

uintptr_t A64Emitter::GetVConstPtr() const { return backend_->emitter_data(); }

uintptr_t A64Emitter::GetVConstPtr(VConst id) const {
  // Load through fixed constant table setup by PlaceConstData.
  // It's important that the pointer is not signed, as it will be sign-extended.
  return GetVConstPtr() + GetVConstOffset(id);
}

// Implies possible StashV(0, ...)!
void A64Emitter::LoadConstantV(oaknut::QReg dest, const vec128_t& v) {
  if (!v.low && !v.high) {
    // 0000...
    // MOVI is implemented as a register-rename while EOR(x, x, x) is not
    // https://dougallj.github.io/applecpu/firestorm.html
    MOVI(dest.B16(), 0);
  } else if (v.low == ~uint64_t(0) && v.high == ~uint64_t(0)) {
    // 1111...
    MOVI(dest.B16(), 0xFF);
  } else {
    // Try to figure out some common splat-patterns to utilize MOVI rather than
    // stashing to memory.
    const bool all_same_u8 =
        std::adjacent_find(std::cbegin(v.u8), std::cend(v.u8),
                           std::not_equal_to<>()) == std::cend(v.u8);

    if (all_same_u8) {
      // 0xXX, 0xXX, 0xXX...
      MOVI(dest.B16(), v.u8[0]);
      return;
    }

    const bool all_same_u16 =
        std::adjacent_find(std::cbegin(v.u16), std::cend(v.u16),
                           std::not_equal_to<>()) == std::cend(v.u16);

    if (all_same_u16) {
      if ((v.u16[0] & 0xFF00) == 0) {
        // 0x00XX, 0x00XX, 0x00XX...
        MOVI(dest.H8(), uint8_t(v.u16[0]));
        return;
      } else if ((v.u16[0] & 0x00FF) == 0) {
        // 0xXX00, 0xXX00, 0xXX00...
        MOVI(dest.H8(), uint8_t(v.u16[0] >> 8), oaknut::util::LSL, 8);
        return;
      }
    }

    const bool all_same_u32 =
        std::adjacent_find(std::cbegin(v.u32), std::cend(v.u32),
                           std::not_equal_to<>()) == std::cend(v.u32);

    if (all_same_u32) {
      if ((v.u32[0] & 0x00FFFFFF) == 0) {
        // This is used a lot for certain float-splats and should be checked
        // first before the others
        // 0xXX000000, 0xXX000000, 0xXX000000...
        MOVI(dest.S4(), uint8_t(v.u32[0] >> 24), oaknut::util::LSL, 24);
        return;
      } else if ((v.u32[0] & 0xFFFFFF00) == 0) {
        // 0x000000XX, 0x000000XX, 0x000000XX...
        MOVI(dest.S4(), uint8_t(v.u32[0]));
        return;
      } else if ((v.u32[0] & 0xFFFF00FF) == 0) {
        // 0x0000XX00, 0x0000XX00, 0x0000XX00...
        MOVI(dest.S4(), uint8_t(v.u32[0] >> 8), oaknut::util::LSL, 8);
        return;
      } else if ((v.u32[0] & 0xFF00FFFF) == 0) {
        // 0x00XX0000, 0x00XX0000, 0x00XX0000...
        MOVI(dest.S4(), uint8_t(v.u32[0] >> 16), oaknut::util::LSL, 16);
        return;
      }

      // Try to utilize FMOV if possible
      oaknut::FImm8 fp8(0);
      if (f32_to_fimm8(v.u32[0], fp8)) {
        FMOV(dest.S4(), fp8);
        return;
      }
    }

    // TODO(benvanik): see what other common values are.
    // TODO(benvanik): build constant table - 99% are reused.
    MovMem64(SP, kStashOffset, v.low);
    MovMem64(SP, kStashOffset + 8, v.high);
    LDR(dest, SP, kStashOffset);
  }
}

void A64Emitter::LoadConstantV(oaknut::QReg dest, float v) {
  union {
    float f;
    uint32_t i;
  } x = {v};
  if (!x.i) {
    // +0.0f (but not -0.0f because it may be used to flip the sign via xor).
    MOVI(dest.B16(), 0);
  } else if (x.i == ~uint32_t(0)) {
    // 1111...
    MOVI(dest.B16(), 0xFF);
  } else {
    // TODO(benvanik): see what other common values are.
    // TODO(benvanik): build constant table - 99% are reused.

    // Try to utilize FMOV if possible
    oaknut::FImm8 fp8(0);
    if (f32_to_fimm8(x.i, fp8)) {
      FMOV(dest.toS(), fp8);
      return;
    }

    MOV(W0, x.i);
    FMOV(dest.toS(), W0);
  }
}

void A64Emitter::LoadConstantV(oaknut::QReg dest, double v) {
  union {
    double d;
    uint64_t i;
  } x = {v};
  if (!x.i) {
    // +0.0 (but not -0.0 because it may be used to flip the sign via xor).
    MOVI(dest.toD(), oaknut::RepImm(0));
  } else if (x.i == ~uint64_t(0)) {
    // 1111...
    MOVI(dest.toD(), oaknut::RepImm(0xFF));
  } else {
    // TODO(benvanik): see what other common values are.
    // TODO(benvanik): build constant table - 99% are reused.

    // Try to utilize FMOV if possible
    oaknut::FImm8 fp8(0);
    if (f64_to_fimm8(x.i, fp8)) {
      FMOV(dest.toD(), fp8);
      return;
    }

    MOV(X0, x.i);
    FMOV(dest.toD(), X0);
  }
}

uintptr_t A64Emitter::StashV(int index, const oaknut::QReg& r) {
  // auto addr = ptr[rsp + kStashOffset + (index * 16)];
  // vmovups(addr, r);
  const auto addr = kStashOffset + (index * 16);
  STR(r, SP, addr);
  return addr;
}

uintptr_t A64Emitter::StashConstantV(int index, float v) {
  union {
    float f;
    uint32_t i;
  } x = {v};
  const auto addr = kStashOffset + (index * 16);
  MovMem64(SP, addr, x.i);
  MovMem64(SP, addr + 8, 0);
  return addr;
}

uintptr_t A64Emitter::StashConstantV(int index, double v) {
  union {
    double d;
    uint64_t i;
  } x = {v};
  const auto addr = kStashOffset + (index * 16);
  MovMem64(SP, addr, x.i);
  MovMem64(SP, addr + 8, 0);
  return addr;
}

uintptr_t A64Emitter::StashConstantV(int index, const vec128_t& v) {
  const auto addr = kStashOffset + (index * 16);
  MovMem64(SP, addr, v.low);
  MovMem64(SP, addr + 8, v.high);
  return addr;
}


        void A64Emitter::PushStackpoint() {

            if (!cvars::enable_host_guest_stack_synchronization) {
                return;
            }
// push the current host and guest stack pointers
// this is done before a stack frame is set up or any guest instructions are
// executed this code is probably the most intrusive part of the stackpoint
            const oaknut::XReg BackendContextPtr=X7;
            {
                SUB(BackendContextPtr,GetContextReg(),sizeof(A64BackendContext));
            }
            LDR(X1,BackendContextPtr,offsetof(A64BackendContext, stackpoints));
            LDR(X0,BackendContextPtr,offsetof(A64BackendContext, current_stackpoint_depth));

            LDR(X6,GetContextReg(),offsetof(ppc::PPCContext, r[1]));

            static_assert(sizeof(A64BackendStackpoint) == 1<<4);
            LSL(X2,X0,4);
            ADD(X1,X1,X2);

            MOV(X2,SP);
            STR(X2,X1,offsetof(A64BackendStackpoint, host_stack_));
            STR(X6,X1,offsetof(A64BackendStackpoint, guest_stack_));
            LDR(X6,GetContextReg(),offsetof(ppc::PPCContext, lr));
            STR(X6,X1,offsetof(A64BackendStackpoint, guest_return_address_));

            ADD(X0,X0,1);

            STR(X0,BackendContextPtr,offsetof(A64BackendContext, current_stackpoint_depth));

            XELOGE("FIXME {}:{}", __FILE__, __LINE__);
            /*MOV(X2,(uint32_t)cvars::max_stackpoints);
            CMP(X0,X2);
            B(oaknut::Cond::GE, handle_overflowed_stackpoints);*/
        }
        void A64Emitter::PopStackpoint() {

            if (!cvars::enable_host_guest_stack_synchronization) {
                return;
            }
            // todo: maybe verify that rsp and r1 == the stackpoint?
            const oaknut::XReg BackendContextPtr=X7;
            {
                SUB(BackendContextPtr,GetContextReg(),sizeof(A64BackendContext));
            }
            LDR(X0,BackendContextPtr,offsetof(A64BackendContext, current_stackpoint_depth));
            SUB(X0,X0,1);
            STR(X0,BackendContextPtr,offsetof(A64BackendContext, current_stackpoint_depth));
        }
        void A64Emitter::EnsureSynchronizedGuestAndHostStack() {

            if (!cvars::enable_host_guest_stack_synchronization) {
                return;
            }
            MOV(X0,SP);
            AND(X0,X0,15);
            oaknut::Label skip;
            CBZ(X0, skip);

            const oaknut::XReg BackendContextPtr=X7;
            {
                SUB(BackendContextPtr,GetContextReg(),sizeof(A64BackendContext));
            }
            LDR(X1,BackendContextPtr,offsetof(A64BackendContext, stackpoints));
            LDR(X0,BackendContextPtr,offsetof(A64BackendContext, current_stackpoint_depth));
            SUB(X2,X0,1);
            LDR(X6,GetContextReg(),offsetof(ppc::PPCContext, r[1]));

            MOV(X8,0);

            oaknut::Label loop;
            oaknut::Label loop_out;
            l(loop);
            static_assert(sizeof(A64BackendStackpoint) == 1<<4);
            LSL(X3,X2,4);
            ADD(X5,X1,X3);
            LDR(X5,X5,offsetof(A64BackendStackpoint, guest_stack_));
            CMP(X5,X6);
            B(oaknut::Cond::GE, loop_out);

            ADD(X8,X8,1);
            SUB(X2,X2,1);
            //js(signed_underflow, T_NEAR);  // should be impossible!!

            B(loop);

            l(loop_out);

            oaknut::Label skip_adjust;
            CMP(X8,1);
            B(oaknut::Cond::LE, skip_adjust);

            //pop(r9);  // guest retaddr
            // W17 = target PPC address
// r10d = the guest_stack
// while guest_stack is equal and return address is not equal, decrement

            oaknut::Label search_for_retaddr;
            oaknut::Label we_good_but_increment;
            l(search_for_retaddr);

            //imul(edx, ecx, sizeof(X64BackendStackpoint));
            static_assert(sizeof(A64BackendStackpoint) == 1<<4);
            LSL(X3,X2,4);

            //cmp(r10d, ptr[rbx + rdx + offsetof(X64BackendStackpoint, guest_stack_)]);
            ADD(X6,X1,X3);
            LDR(X6,X6,offsetof(A64BackendStackpoint, guest_stack_));

            CMP(X5,X6);

            //jnz(we_good_but_increment, T_NEAR);
            B(oaknut::Cond::NE, we_good_but_increment);

            //cmp(r9d,
            //    ptr[rbx + rdx + offsetof(X64BackendStackpoint, guest_return_address_)]);
            ADD(X6,X1,X3);
            LDR(X6,X6,offsetof(A64BackendStackpoint, guest_return_address_));

            CMP(X17,X6);
            //jz(we_good, T_NEAR);  // stack is equal, return address is equal, we've got

            oaknut::Label we_good;
            B(oaknut::Cond::EQ, we_good);

            SUB(X2,X2,1);

            // our destination stack
            //dec(ecx);
            //jmp(search_for_retaddr, T_NEAR);
            B(oaknut::Cond::AL, search_for_retaddr);

            l(we_good_but_increment);
            //add(edx, sizeof(X64BackendStackpoint));
            //inc(ecx);
            //jmp(checkbp, T_NEAR);
            ADD(X3,X3, sizeof(A64BackendStackpoint));
            SUB(X2,X2,1);

            oaknut::Label checkbp;
            B(oaknut::Cond::AL, checkbp);

            l(we_good);
// we're popping this return address, so go down by one
            //sub(edx, sizeof(X64BackendStackpoint));
            //dec(ecx);
            SUB(X3,X3, sizeof(A64BackendStackpoint));
            SUB(X2,X2,1);
            l(checkbp);
            //mov(rsp, ptr[rbx + rdx + offsetof(X64BackendStackpoint, host_stack_)]);
            ADD(X6,X1,X3);
            LDR(X6,X6,offsetof(A64BackendStackpoint, host_stack_));
            MOV(SP,X6);
            /*if (IsFeatureEnabled(kX64FlagsIndependentVars)) {
                inc(ecx);
            } else {
                add(ecx, 1);
            }*/
            ADD(X2,X2,1);

            MOV(X6,stack_size());

            //sub(rsp, r11);  // adjust stack
            SUB(SP,SP,X6);

            //mov(GetBackendCtxPtr(offsetof(X64BackendContext, current_stackpoint_depth)),
            //    ecx);  // set next stackpoint index to be after the one we restored to
            STR(X2,BackendContextPtr,offsetof(A64BackendContext, current_stackpoint_depth));
            //jmp(r8);
            B(oaknut::Cond::AL,skip);
            l(skip_adjust);
            l(skip);
        }
}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
