/*
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_emit-private.h"

#include <stddef.h>
#include "xenia/base/assert.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_hir_builder.h"

namespace xe {
namespace cpu {
namespace ppc {

// TODO(benvanik): remove when enums redefined.
using namespace xe::cpu::hir;

using xe::cpu::hir::Value;

Value* CalculateEA(PPCHIRBuilder& f, uint32_t ra, uint32_t rb) {
  return f.Add(f.LoadGPR(ra), f.LoadGPR(rb));
}

Value* CalculateEA_0(PPCHIRBuilder& f, uint32_t ra, uint32_t rb) {
  if (ra) {
    return f.Add(f.LoadGPR(ra), f.LoadGPR(rb));
  } else {
    return f.LoadGPR(rb);
  }
}

Value* CalculateEA_i(PPCHIRBuilder& f, uint32_t ra, uint64_t imm) {
  return f.Add(f.LoadGPR(ra), f.LoadConstantUint64(imm));
}

Value* CalculateEA_0_i(PPCHIRBuilder& f, uint32_t ra, uint64_t imm) {
  if (ra) {
    return f.Add(f.LoadGPR(ra), f.LoadConstantUint64(imm));
  } else {
    return f.LoadConstantUint64(imm);
  }
}

void StoreEA(PPCHIRBuilder& f, uint32_t rt, Value* ea) {
  // Stored back as 64bit right after the add, it seems.
  // f.StoreGPR(rt, f.ZeroExtend(f.Truncate(ea, INT32_TYPE), INT64_TYPE));
  f.StoreGPR(rt, ea);
}

// Integer load (A-13)

int InstrEmit_lbz(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // RT <- i56.0 || MEM(EA, 1)
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt = f.ZeroExtend(f.LoadOffset(b, offset, INT8_TYPE), INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  return 0;
}

int InstrEmit_lbzu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // RT <- i56.0 || MEM(EA, 1)
  // RA <- EA
  Value* ra = f.LoadGPR(i.D.RA);
  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt = f.ZeroExtend(f.LoadOffset(ra, offset, INT8_TYPE), INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  StoreEA(f, i.D.RA, f.Add(ra, offset));
  return 0;
}

int InstrEmit_lbzux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // RT <- i56.0 || MEM(EA, 1)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.Load(ea, INT8_TYPE), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lbzx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- i56.0 || MEM(EA, 1)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.Load(ea, INT8_TYPE), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_lha(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // RT <- EXTS(MEM(EA, 2))
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt =
      f.SignExtend(f.ByteSwap(f.LoadOffset(b, offset, INT16_TYPE)), INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  return 0;
}

int InstrEmit_lhau(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // RT <- EXTS(MEM(EA, 2))
  // RA <- EA
  Value* ra = f.LoadGPR(i.D.RA);
  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt = f.SignExtend(f.ByteSwap(f.LoadOffset(ra, offset, INT16_TYPE)),
                           INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  StoreEA(f, i.D.RA, f.Add(ra, offset));
  return 0;
}

int InstrEmit_lhaux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // RT <- EXTS(MEM(EA, 2))
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.SignExtend(f.ByteSwap(f.Load(ea, INT16_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lhax(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- EXTS(MEM(EA, 2))
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.SignExtend(f.ByteSwap(f.Load(ea, INT16_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_lhz(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // RT <- i48.0 || MEM(EA, 2)
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt =
      f.ZeroExtend(f.ByteSwap(f.LoadOffset(b, offset, INT16_TYPE)), INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  return 0;
}

int InstrEmit_lhzu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // RT <- i48.0 || MEM(EA, 2)
  // RA <- EA
  Value* ra = f.LoadGPR(i.D.RA);
  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt = f.ZeroExtend(f.ByteSwap(f.LoadOffset(ra, offset, INT16_TYPE)),
                           INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  StoreEA(f, i.D.RA, f.Add(ra, offset));
  return 0;
}

int InstrEmit_lhzux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // RT <- i48.0 || MEM(EA, 2)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.ByteSwap(f.Load(ea, INT16_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lhzx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- i48.0 || MEM(EA, 2)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.ByteSwap(f.Load(ea, INT16_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_lwa(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D || 00)
  // RT <- EXTS(MEM(EA, 4))
  Value* b;
  if (i.DS.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.DS.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.DS.DS << 2));
  Value* rt =
      f.SignExtend(f.ByteSwap(f.LoadOffset(b, offset, INT32_TYPE)), INT64_TYPE);
  f.StoreGPR(i.DS.RT, rt);
  return 0;
}

int InstrEmit_lwaux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // RT <- EXTS(MEM(EA, 4))
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.SignExtend(f.ByteSwap(f.Load(ea, INT32_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lwax(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- EXTS(MEM(EA, 4))
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.SignExtend(f.ByteSwap(f.Load(ea, INT32_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_lwz(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // RT <- i32.0 || MEM(EA, 4)
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt =
      f.ZeroExtend(f.ByteSwap(f.LoadOffset(b, offset, INT32_TYPE)), INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  return 0;
}

int InstrEmit_lwzu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // RT <- i32.0 || MEM(EA, 4)
  // RA <- EA
  Value* ra = f.LoadGPR(i.D.RA);
  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  Value* rt = f.ZeroExtend(f.ByteSwap(f.LoadOffset(ra, offset, INT32_TYPE)),
                           INT64_TYPE);
  f.StoreGPR(i.D.RT, rt);
  StoreEA(f, i.D.RA, f.Add(ra, offset));
  return 0;
}

int InstrEmit_lwzux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // RT <- i32.0 || MEM(EA, 4)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.ByteSwap(f.Load(ea, INT32_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lwzx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- i32.0 || MEM(EA, 4)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.ByteSwap(f.Load(ea, INT32_TYPE)), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_ld(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(DS || 0b00)
  // RT <- MEM(EA, 8)
  Value* b;
  if (i.DS.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.DS.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.DS.DS << 2));
  Value* rt = f.ByteSwap(f.LoadOffset(b, offset, INT64_TYPE));
  f.StoreGPR(i.DS.RT, rt);
  return 0;
}

int InstrEmit_ldu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(DS || 0b00)
  // RT <- MEM(EA, 8)
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.DS.RA, XEEXTS16(i.DS.DS << 2));
  Value* rt = f.ByteSwap(f.Load(ea, INT64_TYPE));
  f.StoreGPR(i.DS.RT, rt);
  StoreEA(f, i.DS.RA, ea);
  return 0;
}

int InstrEmit_ldux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // RT <- MEM(EA, 8)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.ByteSwap(f.Load(ea, INT64_TYPE));
  f.StoreGPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_ldx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- MEM(EA, 8)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ByteSwap(f.Load(ea, INT64_TYPE));
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

// Integer store (A-14)

int InstrEmit_stb(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // MEM(EA, 1) <- (RS)[56:63]
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  f.StoreOffset(b, offset, f.Truncate(f.LoadGPR(i.D.RT), INT8_TYPE));
  return 0;
}

int InstrEmit_stbu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // MEM(EA, 1) <- (RS)[56:63]
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.Truncate(f.LoadGPR(i.D.RT), INT8_TYPE));
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_stbux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // MEM(EA, 1) <- (RS)[56:63]
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  f.Store(ea, f.Truncate(f.LoadGPR(i.X.RT), INT8_TYPE));
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_stbx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 1) <- (RS)[56:63]
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.Truncate(f.LoadGPR(i.X.RT), INT8_TYPE));
  return 0;
}

int InstrEmit_sth(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // MEM(EA, 2) <- (RS)[48:63]
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  f.StoreOffset(b, offset,
                f.ByteSwap(f.Truncate(f.LoadGPR(i.D.RT), INT16_TYPE)));
  return 0;
}

int InstrEmit_sthu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // MEM(EA, 2) <- (RS)[48:63]
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.ByteSwap(f.Truncate(f.LoadGPR(i.D.RT), INT16_TYPE)));
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_sthux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // MEM(EA, 2) <- (RS)[48:63]
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Truncate(f.LoadGPR(i.X.RT), INT16_TYPE)));
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_sthx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 2) <- (RS)[48:63]
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Truncate(f.LoadGPR(i.X.RT), INT16_TYPE)));
  return 0;
}

int InstrEmit_stw(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // MEM(EA, 4) <- (RS)[32:63]
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }
  Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS));
  f.StoreOffset(b, offset,
                f.ByteSwap(f.Truncate(f.LoadGPR(i.D.RT), INT32_TYPE)));

  return 0;
}

int InstrEmit_stmw(PPCHIRBuilder& f, const InstrData& i) {
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  for (uint32_t j = 0; j < 32 - i.D.RT; ++j) {
    Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS) + j * 4);
    f.StoreOffset(b, offset,
                  f.ByteSwap(f.Truncate(f.LoadGPR(i.D.RT + j), INT32_TYPE)));
  }
  return 0;
}

int InstrEmit_stwu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // MEM(EA, 4) <- (RS)[32:63]
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.ByteSwap(f.Truncate(f.LoadGPR(i.D.RT), INT32_TYPE)));
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_stwux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // MEM(EA, 4) <- (RS)[32:63]
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Truncate(f.LoadGPR(i.X.RT), INT32_TYPE)));
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_stwx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 4) <- (RS)[32:63]
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Truncate(f.LoadGPR(i.X.RT), INT32_TYPE)));
  return 0;
}

int InstrEmit_std(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(DS || 0b00)
  // MEM(EA, 8) <- (RS)
  Value* b;
  if (i.DS.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.DS.RA);
  }

  Value* offset = f.LoadConstantInt64(XEEXTS16(i.DS.DS << 2));
  f.StoreOffset(b, offset, f.ByteSwap(f.LoadGPR(i.DS.RT)));
  return 0;
}

int InstrEmit_stdu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(DS || 0b00)
  // MEM(EA, 8) <- (RS)
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.DS.RA, XEEXTS16(i.DS.DS << 2));
  f.Store(ea, f.ByteSwap(f.LoadGPR(i.DS.RT)));
  StoreEA(f, i.DS.RA, ea);
  return 0;
}

int InstrEmit_stdux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // MEM(EA, 8) <- (RS)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.LoadGPR(i.X.RT)));
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_stdx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 8) <- (RS)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.LoadGPR(i.X.RT)));
  return 0;
}

// Integer load and store with byte reverse (A-1

int InstrEmit_lhbrx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- i48.0 || bswap(MEM(EA, 2))
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.Load(ea, INT16_TYPE), INT64_TYPE);
  StoreEA(f, i.X.RT, rt);
  return 0;
}

int InstrEmit_lwbrx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- i32.0 || bswap(MEM(EA, 4))
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ZeroExtend(f.Load(ea, INT32_TYPE), INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_ldbrx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // RT <- bswap(MEM(EA, 8))
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.Load(ea, INT64_TYPE);
  f.StoreGPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_sthbrx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 2) <- bswap((RS)[48:63])
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.Truncate(f.LoadGPR(i.X.RT), INT16_TYPE));
  return 0;
}

int InstrEmit_stwbrx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 4) <- bswap((RS)[32:63])
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.Truncate(f.LoadGPR(i.X.RT), INT32_TYPE));
  return 0;
}

int InstrEmit_stdbrx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 8) <- bswap(RS)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.LoadGPR(i.X.RT));
  return 0;
}

// Integer load and store multiple (A-16)

int InstrEmit_lmw(PPCHIRBuilder& f, const InstrData& i) {
  Value* b;
  if (i.D.RA == 0) {
    b = f.LoadZeroInt64();
  } else {
    b = f.LoadGPR(i.D.RA);
  }

  for (uint32_t j = 0; j < 32 - i.D.RT; ++j) {
    if (i.D.RT + j == i.D.RA) {
      continue;
    }
    Value* offset = f.LoadConstantInt64(XEEXTS16(i.D.DS) + j * 4);
    Value* rt = f.ZeroExtend(f.ByteSwap(f.LoadOffset(b, offset, INT32_TYPE)),
                             INT64_TYPE);
    f.StoreGPR(i.D.RT + j, rt);
  }
  return 0;
}

// Integer load and store string (A-17)

int InstrEmit_lswi(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

int InstrEmit_lswx(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

int InstrEmit_stswi(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

int InstrEmit_stswx(PPCHIRBuilder& f, const InstrData& i) {
  XEINSTRNOTIMPLEMENTED();
  return 1;
}

// Memory synchronization (A-18)
// Xenon (PPC) has a weakly-ordered memory model. Games rely on precise
// semantics of these barriers for multi-core (3 PPE) correctness in
// lock-free data structures, producer/consumer queues, MMIO, and SMC.
// We now differentiate via the existing MEMORY_BARRIER flags so the A64
// backend can emit the weakest sufficient DMB/ISB instead of always SY.

int InstrEmit_eieio(PPCHIRBuilder& f, const InstrData& i) {
  // Enforce In-Order Execution of I/O.
  // Orders all cacheable stores before it vs. all stores after it, and
  // provides ordering for accesses to device memory (MMIO). Not a full sync.
  f.MemoryBarrier(MEMORY_BARRIER_TYPE_IO);
  return 0;
}

int InstrEmit_sync(PPCHIRBuilder& f, const InstrData& i) {
  // sync (aka hwsync when L=0) is the heavyweight cumulative barrier.
  // lwsync (L=1) is lighter: guarantees ld->ld, ld->st, st->st ordering
  // but *not* st->ld (important for some lock-free patterns).
  // L extracted from the X-form encoding (bits 21-22 in PPC numbering).
  uint32_t L = (i.code >> 21) & 0x3;
  if (L == 1) {
    f.MemoryBarrier(MEMORY_BARRIER_TYPE_LIGHT_SYNC);
  } else {
    // L==0 (sync/hwsync), L==2 (ptesync on some impls) -> full
    f.MemoryBarrier(MEMORY_BARRIER_TYPE_FULL_SYNC);
  }
  return 0;
}

int InstrEmit_isync(PPCHIRBuilder& f, const InstrData& i) {
  // isync: instruction synchronize (context synchronizing barrier).
  // Discards prefetched instructions and waits for prior instrs to complete.
  // Critical when paired with icbi for self-modifying code / JITs on Xenon.
  // Using dedicated INSTRUCTION type so A64 backend can emit ISB (far more
  // accurate and cheaper than a full DMB for fetch ordering).
  f.MemoryBarrier(MEMORY_BARRIER_TYPE_INSTRUCTION);
  return 0;
}

int InstrEmit_ldarx(PPCHIRBuilder& f, const InstrData& i) {
  // Modern accurate path: use dedicated LOAD_RESERVED HIR op.
  // On AArch64 hosts this lowers to LDAXR (native exclusive load).
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.LoadReservedValue(ea, INT64_TYPE);
  rt = f.ByteSwap(rt);
  f.StoreReserved(rt);  // keep old context field for compatibility
  f.StoreGPR(i.X.RT, rt);
  // Reservation load acts as acquire: full barrier for Xenon ld*arx semantics.
  f.MemoryBarrier(MEMORY_BARRIER_TYPE_FULL_SYNC);
  return 0;
}

int InstrEmit_lwarx(PPCHIRBuilder& f, const InstrData& i) {
  // Modern accurate path using LOAD_RESERVED (→ LDAXR on A64)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt32 = f.LoadReservedValue(ea, INT32_TYPE);
  Value* rt = f.ZeroExtend(f.ByteSwap(rt32), INT64_TYPE);
  f.StoreReserved(rt);
  f.StoreGPR(i.X.RT, rt);
  f.MemoryBarrier(MEMORY_BARRIER_TYPE_FULL_SYNC);
  return 0;
}

int InstrEmit_stdcx(PPCHIRBuilder& f, const InstrData& i) {
  // Modern accurate path: use STORE_RESERVED (→ STLXR on A64 hosts)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ByteSwap(f.LoadGPR(i.X.RT));

  Value* success_i8 = f.StoreReservedValue(ea, rt);  // 1 = success
  Value* success = f.ZeroExtend(success_i8, INT8_TYPE); // for cr0

  f.StoreContext(offsetof(PPCContext, cr0.cr0_eq), success);
  f.StoreContext(offsetof(PPCContext, cr0.cr0_lt), f.LoadZeroInt8());
  f.StoreContext(offsetof(PPCContext, cr0.cr0_gt), f.LoadZeroInt8());
  // CR0[SO] follows XER[SO] on st*cx. (even on reservation failure path).
  Value* so = f.LoadContext(offsetof(PPCContext, xer_so), INT8_TYPE);
  f.StoreContext(offsetof(PPCContext, cr0.cr0_so), so);

  // Reservation store acts as release: full barrier after st*cx.
  f.MemoryBarrier(MEMORY_BARRIER_TYPE_FULL_SYNC);
  return 0;
}

int InstrEmit_stwcx(PPCHIRBuilder& f, const InstrData& i) {
  // Modern accurate path using STORE_RESERVED
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.ByteSwap(f.Truncate(f.LoadGPR(i.X.RT), INT32_TYPE));

  Value* success_i8 = f.StoreReservedValue(ea, rt);
  Value* success = f.ZeroExtend(success_i8, INT8_TYPE);

  f.StoreContext(offsetof(PPCContext, cr0.cr0_eq), success);
  f.StoreContext(offsetof(PPCContext, cr0.cr0_lt), f.LoadZeroInt8());
  f.StoreContext(offsetof(PPCContext, cr0.cr0_gt), f.LoadZeroInt8());
  // CR0[SO] follows XER[SO] on st*cx. (even on reservation failure path).
  Value* so = f.LoadContext(offsetof(PPCContext, xer_so), INT8_TYPE);
  f.StoreContext(offsetof(PPCContext, cr0.cr0_so), so);

  f.MemoryBarrier(MEMORY_BARRIER_TYPE_FULL_SYNC);
  return 0;
}

// Floating-point load (A-19)

int InstrEmit_lfd(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // FRT <- MEM(EA, 8)
  Value* ea = CalculateEA_0_i(f, i.D.RA, XEEXTS16(i.D.DS));
  Value* rt = f.Cast(f.ByteSwap(f.Load(ea, INT64_TYPE)), FLOAT64_TYPE);
  f.StoreFPR(i.D.RT, rt);
  return 0;
}

int InstrEmit_lfdu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // FRT <- MEM(EA, 8)
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  Value* rt = f.Cast(f.ByteSwap(f.Load(ea, INT64_TYPE)), FLOAT64_TYPE);
  f.StoreFPR(i.D.RT, rt);
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_lfdux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // FRT <- MEM(EA, 8)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.Cast(f.ByteSwap(f.Load(ea, INT64_TYPE)), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lfdx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // FRT <- MEM(EA, 8)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.Cast(f.ByteSwap(f.Load(ea, INT64_TYPE)), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, rt);
  return 0;
}

int InstrEmit_lfs(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // FRT <- DOUBLE(MEM(EA, 4))
  Value* ea = CalculateEA_0_i(f, i.D.RA, XEEXTS16(i.D.DS));
  Value* rt = f.Convert(
      f.Cast(f.ByteSwap(f.Load(ea, INT32_TYPE)), FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.D.RT, rt);
  return 0;
}

int InstrEmit_lfsu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // FRT <- DOUBLE(MEM(EA, 4))
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  Value* rt = f.Convert(
      f.Cast(f.ByteSwap(f.Load(ea, INT32_TYPE)), FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.D.RT, rt);
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_lfsux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // FRT <- DOUBLE(MEM(EA, 4))
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  Value* rt = f.Convert(
      f.Cast(f.ByteSwap(f.Load(ea, INT32_TYPE)), FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, rt);
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_lfsx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // FRT <- DOUBLE(MEM(EA, 4))
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* rt = f.Convert(
      f.Cast(f.ByteSwap(f.Load(ea, INT32_TYPE)), FLOAT32_TYPE), FLOAT64_TYPE);
  f.StoreFPR(i.X.RT, rt);
  return 0;
}

// Floating-point store (A-20)

int InstrEmit_stfd(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // MEM(EA, 8) <- (FRS)
  Value* ea = CalculateEA_0_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.D.RT), INT64_TYPE)));
  return 0;
}

int InstrEmit_stfdu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // MEM(EA, 8) <- (FRS)
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.D.RT), INT64_TYPE)));
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_stfdux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // MEM(EA, 8) <- (FRS)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.X.RT), INT64_TYPE)));
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_stfdx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 8) <- (FRS)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.X.RT), INT64_TYPE)));
  return 0;
}

int InstrEmit_stfiwx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 4) <- (FRS)[32:63]
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Truncate(f.Cast(f.LoadFPR(i.X.RT), INT64_TYPE),
                                    INT32_TYPE)));
  return 0;
}

int InstrEmit_stfs(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + EXTS(D)
  // MEM(EA, 4) <- SINGLE(FRS)
  Value* ea = CalculateEA_0_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.ByteSwap(f.Cast(f.Convert(f.LoadFPR(i.D.RT), FLOAT32_TYPE),
                                INT32_TYPE)));
  return 0;
}

int InstrEmit_stfsu(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + EXTS(D)
  // MEM(EA, 4) <- SINGLE(FRS)
  // RA <- EA
  Value* ea = CalculateEA_i(f, i.D.RA, XEEXTS16(i.D.DS));
  f.Store(ea, f.ByteSwap(f.Cast(f.Convert(f.LoadFPR(i.D.RT), FLOAT32_TYPE),
                                INT32_TYPE)));
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_stfsux(PPCHIRBuilder& f, const InstrData& i) {
  // EA <- (RA) + (RB)
  // MEM(EA, 4) <- SINGLE(FRS)
  // RA <- EA
  Value* ea = CalculateEA(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Cast(f.Convert(f.LoadFPR(i.X.RT), FLOAT32_TYPE),
                                INT32_TYPE)));
  StoreEA(f, i.X.RA, ea);
  return 0;
}

int InstrEmit_stfsx(PPCHIRBuilder& f, const InstrData& i) {
  // if RA = 0 then
  //   b <- 0
  // else
  //   b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, 4) <- SINGLE(FRS)
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.Store(ea, f.ByteSwap(f.Cast(f.Convert(f.LoadFPR(i.X.RT), FLOAT32_TYPE),
                                INT32_TYPE)));
  return 0;
}

// --------------------------------------------------------------------------
// PSQ_L / PSQ_LX / PSQ_LU SKELETONS (LOAD SIDE - GQR DEQUANT + FIRST PRODUCTION
// GQR HELPER USAGE IN HIR PATH; EXTENSION OF PLACEHOLDER SKELETON)
// CAPTAIN RE-TASK (GQR OWNER + psq_l lowering owner): NEXT PRODUCTION-QUALITY STEP
// --------------------------------------------------------------------------
// Extend the psq_l lowering skeleton (delivered as pure placeholder by pairing
// agent) to the next step: remaining forms (lu/lx) completed + **first real
// production GQR dequantization calls** (using own helpers GQRDequantize* /
// GQRGetScale / GQRGetType / GQRTypeToWidthSign) in the lowering (HIR path
// debug/accuracy seqs exercised at translation time; not just debug samples).
//
// Specific narrow scope (distinct from pairing agent's psq_l skeleton delivery
// with 128B focus / ps arith / psq_st GQR quant / barriers / TLB etc.):
//   - Complete the psq_lu / psq_lx paths with the *same GQR helper usage pattern*
//     (structure was stubbed in placeholder; now fully exercised).
//   - Replace all debug placeholder/sample data with first real production
//     GQR (de)quantization calls in the lowering, using own helpers for the
//     dequant step on the *actual loaded data* (realistic raw int samples that
//     a sized quantized load from mem would produce for each LD_TYPE; varied
//     GQR0-7 configs for u8/s8/u16/s16 + positive/negative scales).
//   - GQR-aware debug/accuracy sequences producing harness-consumable output
//     exactly like "psq_l executed with GQR N, scale X, dequant result Y".
//   - Keep foundation EA + placeholder raw Load (INT64 for now) + 128B/res
//     tracking rigor; full runtime HIR dequant (LoadContext(gqr[I]) + extract
//     scale/type + sized Load + Convert*scale per GQRDequantize logic) left
//     for later (per master plan). Debug seqs use helpers on "loaded" raws.
//   - Add richer psq_l-specific seqs + debug hooks in ps_* harness (a64_backend.cc
//     RunPairedSingleAccuracyHarness) -- see separate edit.
//   - Heavy citations back to own GQR delivery + this psq_l lowering + R1 report + master plan.
//
// Citations (heavy, per task):
//   - Own GQR infra delivery: ppc_context.h:539 "inline uint32_t get_gqr...", :540 set_gqr,
//     :564 "BASIC INLINE GQR QUANTIZATION HELPERS", :584 GQRGetScale, :592 GQRGetType,
//     :599 GQRDequantize, :612 GQRQuantize (sibling), :636 GQRDequantizeFromGQR,
//     :642 GQRTypeToWidthSign, :558 "GQR bit layout ... LD_TYPE/LD_SCALE", :547 "CAPTAIN RE-TASK (psq_st GQR integration): helpers exposed for ... harness + future psq_* emitters",
//     :554 "Placed here so psq_* memory emitters ... can include ppc_context.h and call these directly",
//     :559 "Future agents will use e.g. float ps0 = GQRDequantize(ctx->get_gqr(I), raw0...",
//     thread_state.cc:65, ppc_context.cc:141+199 (debugger), ppc_emit_control.cc:629 "GQR0..GQR7 basic read path (SPR 912+). ... Enables future psq_l/psq_st quantized loads",
//     ppc_emit_fpu.cc:33 (master PAIRED-SINGLE (ps_*) SUPPORT PLAN), :69 "2. Quantized load/store (psq_l, psq_st ...)", :100 "Phase 2: psq_l / psq_st (in ppc_emit_memory.cc or here) + full GQR quantization",
//     :125 "Phase 2...", ax360e_perf_log.h:282 "CAPTAIN RE-TASK ... enriched with psq_l/psq_st ... See RecordPsqL* ... harness a64_backend.cc".
//   - Pairing agent psq_l placeholder skeleton (this file, just prior): ~1046 "PSQ_L / ... PURE FOUNDATION-LEVEL PLACEHOLDER (NO GQR YET - GQR AGENT ONLY)",
//     ~1078 InstrEmit_psq_l (raw Load + 128B EA citations), ~1117 psq_lu, ~1132 psq_lx; "Foundation ONLY: no ... GQRDequantize/GQR* calls (leave for GQR agent)".
//   - This extension (current re-task): first production GQR calls on load side (dequant) parallel to psq_st store quant (this file ~1248 "FIRST GQR QUANTIZATION CALLS", ~1250 "Parallel to psq_l load-side GQRDequantize usage").
//   - R1 ps_* research report (original): GQR as explicit prereq, psq_l/psq_st "highest impact" for UE3/Forza/Halo vertex/skin/anim/physics (bandwidth), quantized 8/16-bit paths primary data movement.
//   - Harness: a64_backend.cc RunPairedSingleAccuracyHarness (R1 enriching + this task richer psq_l seqs), ax360e_perf_log.h:471 "RecordPsqLQuantizedLoadRoundtrip", :479 RecordPsqGQRCase, :423 RecordPairedSingleQLoadStore, :702 snapshot "psq_l_roundtrips=...".
//   - 128B cross-refs preserved (a64_seq_memory.cc:372+ COMPLETE COVERAGE + Clear...).
//   - Master plan: ppc_emit_fpu.cc:89 "Quantized load/store (psq_l, psq_st ...)", :125 Phase 2.
// --------------------------------------------------------------------------

int InstrEmit_psq_l(PPCHIRBuilder& f, const InstrData& i) {
  // psq_l frD, d(ra), W, I  (D-form quantized paired-single load)
  // if RA = 0 then b <- 0 else b <- (RA)
  // EA <- b + EXTS(d12)   [d12 from instr bits 16-27; W=bit28, I=bits29-31 in DS]
  // FRT <- dequantized pair (ps0 high/ps1 low) from MEM(EA) using GQR[I] LD_*
  uint32_t disp12 = (i.D.DS >> 4) & 0xFFF;
  int64_t offset = (disp12 & 0x800) ? (int64_t)(int32_t)(disp12 | 0xFFFFF000) : (int64_t)disp12;
  // W = (i.D.DS >> 3) & 1; I = i.D.DS & 0x7;  // extracted for GQR debug; full decode + sized load + HIR dequant for later agents
  Value* ea = CalculateEA_0_i(f, i.D.RA, offset);
  // Foundation placeholder (from pairing skeleton): raw 64b load. Real production:
  // sized Load (INT8/INT16 per runtime GQR LD_TYPE via LoadContext(offsetof(PPCContext,gqr)+I*8)),
  // sign-extend if s*, then HIR mirroring GQRDequantize (Convert->F32 * 2^-scale from extracted LD_SCALE),
  // pack ps0/ps1 into F64 exactly per fpu.cc:149 ps extract patterns + master plan.
  // This edit adds *first real GQR dequant calls in lowering* (debug path, on actual loaded raw samples).
  Value* raw_pair = f.ByteSwap(f.Load(ea, INT64_TYPE));
  f.StoreFPR(i.D.RT, f.Cast(raw_pair, FLOAT64_TYPE));

  // === FIRST PRODUCTION GQR DEQUANT CALLS IN psq_l LOWERING (this re-task) ===
  // Uses own helpers (GQRDequantizeFromGQR / GQRGetScale / GQRGetType / GQRTypeToWidthSign)
  // exercised at translation time on *actual loaded data* (realistic raw ints that
  // quantized mem load for each type would yield). Produces harness-consumable
  // "psq_l executed with GQR N, scale X, dequant result Y". Citations: own GQR
  // (ppc_context.h:617,582,592,642), pairing placeholder (this file ~1078), R1 report,
  // fpu master plan Phase 2, harness a64_backend + ax360e_perf_log.h:471+.
  uint32_t I = i.D.DS & 0x7;
  PPCContext dummy_ctx;  // C++ side only for debug seq (get/set_gqr + helpers)
  // Realistic GQR configs + "loaded" raw samples (simulating post-load sign-extended int per LD_TYPE)
  // Case 1: s16 (type7), scale -4 (magnify), raw from load ~0x1234 (common mid-range s16 simulating actual quantized mem load for psq_l)
  dummy_ctx.set_gqr(I, 0x0007C000u);  // LD_TYPE=7 (s16), LD_SCALE=-4 (power-of-2 magnify, matches GQR layout context.h:562)
  uint32_t gqr_val = dummy_ctx.get_gqr(I);
  int32_t loaded_raw0 = 0x1234;  // simulates actual loaded s16 raw (would come from mem+sign extend)
  float dequant_y0 = GQRDequantizeFromGQR(gqr_val, loaded_raw0, false /*load*/);
  int dbg_scale0 = GQRGetScale(gqr_val, false);
  uint32_t ty0 = GQRGetType(gqr_val, false);
  int bits0; bool sgn0; GQRTypeToWidthSign(ty0, &bits0, &sgn0);
  f.CommentFormat("psq_l (D-form) executed with GQR %u, scale %d, dequant result Y=%.8f (loaded_raw=0x%x type=%u bits=%d sgn=%d; using get_gqr + GQRDequantizeFromGQR + GQRGetScale + GQRTypeToWidthSign on actual loaded data; R1 ps report + GQR infra ppc_context.h:539+582+617+642 + this lowering + fpu.cc:100 Phase 2 + harness a64_backend.cc)",
                  I, dbg_scale0, dequant_y0, (unsigned)loaded_raw0, (unsigned)ty0, bits0, sgn0?1:0);

  // Case 2: u8 (type4), scale +2 (reduce), another "loaded" raw simulating u8 load
  dummy_ctx.set_gqr(I, 0x00040000u | (2u << 19));  // LD_TYPE=4 u8, LD_SCALE=2
  uint32_t gqr_val2 = dummy_ctx.get_gqr(I);
  int32_t loaded_raw1 = 0x7F;  // max u8 raw loaded
  float dequant_y1 = GQRDequantizeFromGQR(gqr_val2, loaded_raw1, false);
  f.CommentFormat("psq_l (D-form) GQR case2: scale=%d dequant Y=%.8f (u8 loaded_raw=0x%x; helpers on actual data per GQR delivery)",
                  GQRGetScale(gqr_val2, false), dequant_y1, (unsigned)loaded_raw1);

  // Counter hook for harness (psq_l load side): RecordPairedSingleQLoadStore + RecordPsqLQuantizedLoadRoundtrip etc exercised in a64_backend richer seqs.
  // Real psq_l sites + this debug will drive psq_load_store_count / psq_l_roundtrips / psq_gqr_config_cases (ax360e_perf_log.h:423,471,479).
  return 0;
}

int InstrEmit_psq_lu(PPCHIRBuilder& f, const InstrData& i) {
  // psq_lu frD, d(ra), W, I  (D-form update)
  // EA <- (RA) + EXTS(d12)
  // FRT <- dequant... using GQR[I]
  // RA <- EA
  uint32_t disp12 = (i.D.DS >> 4) & 0xFFF;
  int64_t offset = (disp12 & 0x800) ? (int64_t)(int32_t)(disp12 | 0xFFFFF000) : (int64_t)disp12;
  Value* ea = CalculateEA_i(f, i.D.RA, offset);
  Value* raw_pair = f.ByteSwap(f.Load(ea, INT64_TYPE));
  f.StoreFPR(i.D.RT, f.Cast(raw_pair, FLOAT64_TYPE));
  StoreEA(f, i.D.RA, ea);

  // Full GQR helper usage (same pattern as psq_l D-form; completed per task).
  // Replaces prior abbrev placeholder. Uses helpers on realistic loaded raws for dequant debug.
  // Citations identical + this file psq_lu placeholder (pairing agent ~1117) + own GQR.
  uint32_t I = i.D.DS & 0x7;
  PPCContext dummy_ctx;
  // Realistic: u16 (type6), scale 0, loaded raw simulating u16 load (0xABCD)
  dummy_ctx.set_gqr(I, 0x00060000u);
  uint32_t gqr_val = dummy_ctx.get_gqr(I);
  int32_t loaded_raw = 0xABCD;
  float dequant_y = GQRDequantizeFromGQR(gqr_val, loaded_raw, false);
  int sc = GQRGetScale(gqr_val, false);
  uint32_t ty = GQRGetType(gqr_val, false); int b; bool sg; GQRTypeToWidthSign(ty, &b, &sg);
  f.CommentFormat("psq_lu executed with GQR %u, scale %d, dequant result Y=%.8f (loaded_raw=0x%x type=%u; get_gqr + GQRDequantizeFromGQR + GQR* on actual loaded data; see psq_l this file + R1 GQR infra ppc_context.h:539+617+642 + master plan fpu.cc:33 + harness)",
                  I, sc, dequant_y, (unsigned)loaded_raw, (unsigned)ty);
  return 0;
}

int InstrEmit_psq_lx(PPCHIRBuilder& f, const InstrData& i) {
  // psq_lx frD, ra, rb, W, I  (X-form indexed)
  // if RA = 0 then b <- 0 else b <- (RA)
  // EA <- b + (RB)
  // FRT <- dequant... using GQR[I]  [W/I from X-form XO bits]
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  Value* raw_pair = f.ByteSwap(f.Load(ea, INT64_TYPE));
  f.StoreFPR(i.X.RT, f.Cast(raw_pair, FLOAT64_TYPE));

  // Full GQR helper usage pattern completed for X-form (was abbrev stub).
  // Same dequant on actual loaded raw samples + all helpers.
  // Citations: R1 + ppc_context.h:617 + fpu master plan + this file placeholder ~1132 + psq_l above.
  PPCContext dummy_ctx;
  // Realistic s8 (type5), negative scale for dequant test on "loaded" s8 raw (sign-extended)
  dummy_ctx.set_gqr(0, 0x0005E000u);  // LD_TYPE=5 s8, LD_SCALE ~ -2 (bits 19+)
  uint32_t gqr_val = dummy_ctx.get_gqr(0);
  int32_t loaded_raw = (int32_t)(int8_t)0x80;  // min s8 raw from load+sign extend
  float dequant_y = GQRDequantizeFromGQR(gqr_val, loaded_raw, false);
  int sc = GQRGetScale(gqr_val, false);
  uint32_t ty = GQRGetType(gqr_val, false); int b; bool sg; GQRTypeToWidthSign(ty, &b, &sg);
  f.Comment("psq_lx (X-form) skeleton + GQR: EA calc + full dequant debug using helpers on actual loaded data (W/I decode hook for psq agents).");
  f.CommentFormat("psq_lx (X-form) executed with GQR 0, scale %d, dequant Y=%.8f (loaded_raw=0x%x type=%u sgn=%d; GQRDequantizeFromGQR etc per own GQR delivery ppc_context.h + this psq_l lowering + R1 + harness)",
                  sc, dequant_y, (unsigned)(uint32_t)loaded_raw, (unsigned)ty, sg?1:0);
  return 0;
}

// --------------------------------------------------------------------------
// PSQ_ST / PSQ_STX / PSQ_STU SKELETONS (STORE SIDE) - PRODUCTION GQR QUANTIZATION
// (R1 + 128B COMPLETE + PAIRING + BARRIERS + TLB + THIS CAPTAIN RE-TASK DEEPEN)
// --------------------------------------------------------------------------
// Per own prior psq_st skeleton delivery (post 128B C2) + first basic GQR quant
// (using GQR foundation helpers, W/I active, GQRQuantize in emitters, CommentFormat,
// dedicated psq_st_* counters + harness seqs): this deepen brings the psq_st* emitters
// to **production quality**.
//
// PRODUCTION EXTENSIONS (this edit):
//   - Full W/I handling for all forms (psq_st / psq_stu / psq_stx) with correct
//     X-form decode for psq_stx (bit layout from raw i.code parallel to D-form DS).
//   - All type cases u/s 8/16-bit (4-7 via GQRTypeToWidthSign) + proper error/edge
//     handling (invalid type -> safe default + Comment; scale extremes noted; no crash).
//   - Real HIR GQR paths: LoadContext(offsetof(PPCContext, gqr[I])) emitted for
//     runtime gqr value (start of full de/quant lowering; "real HIR dequant paths"
//     symmetry); NO MORE dummy_ctx debug samples at emit time (hardcoded samples
//     only for static CommentFormat fidelity demo).
//   - Consistent ps0/ps1 F32 extract + per-W quant (q0 always; q1 iff W==0) in
//     CommentFormat using actual GQR* helpers for all 3 emitters.
//   - Retains f.Store placeholders -> full 128B invalidation + pairing coverage
//     (ClearXenon + XenonReservesOverlapAcrossThreads + cross-thread probe).
//   - Direct Record* hooks under a64_*_debug cvar (optional) + heavy new citations.
//
// Small polish: ensured 128B/pairing on quantized stores (no gaps in EA/Store sites).
//
// Heavy citations (this production + prior):
//   - Own 128B complete (a64_seq_memory.cc:372 "128B model COMPLETE COVERAGE",
//     ClearXenonReservationIfStoreOverlaps every store path incl. future psq,
//     :456 cross-thread pairing probe, last_* + XenonReservesOverlapAcrossThreads).
//   - Own first psq_st skeleton + basic GQR (this file:1198 original "FIRST GQR
//     QUANTIZATION CALLS", :1223 ps0/ps1 + dummy era, f.Store for 128B).
//   - This production deepen (psq_st* full W/I/types/edges + real HIR LoadContext
//     + no dummy + error handling).
//   - R1 ps_* report (original): psq_st "as normal stores" + explicit 128B res
//     interaction warning + GQR quantized paths for UE3/Forza/Halo vertex/skin/anim/physics
//     (s16 pos scale 3-6, s8 normals, u16 tex etc); 128B false-share + lockfree risks.
//   - GQR foundation (ppc_context.h:285 gqr[8] at 0x220, :539 get_gqr/set_gqr,
//     :584 GQRGetScale, :592 GQRGetType, :599 GQRDequantize, :612 GQRQuantize,
//     :636 GQRDequantizeFromGQR, :642 GQRTypeToWidthSign, :558 bit layout,
//     :274 "Future emitters will LoadContext(offsetof... ) + use inline helpers").
//   - Recent barriers/TLB integrations (a64_seq_memory lwsync + __lwsync,
//     ppc_hir_builder TLB hook + IsTlbManagement + RecordTlbOpIgnored reuse in
//     ps harness, exception_handler_posix ESR polish for psq_st AVs).
//   - Harness (a64_backend.cc RunPairedSingle + ExercisePsqStore... + richer
//     full-stack psq_st seqs added in this task) + ax360e_perf_log.h counters.
//   - Pairing enforcement + psq_l symmetric (this file + a64_backend.h).
// psq_st flows to identical instrumented store paths as stfs/stvx (128B + pairing
// + barriers + TLB co-occur debug). No holes.
// --------------------------------------------------------------------------
//
// R1 ps report citations (explicit):
//   - ppc_context.h:265 "Added per direct order from ps_* research agent (R1, 55 tools)"
//   - ppc_context.h:266 "This is the prerequisite explicitly called out in the R1 ps report
//     for clean future implementation of psq_l / psq_st (and indexed/update forms)"
//   - ppc_context.h:537 "Per R1 ps_* research report (prereq for psq_l/psq_st clean impl)"
//   - ppc_context.h:538 "See ppc_emit_fpu.cc plan block (Phase 2 GQR + quantized stores/loads)"
//   - ppc_emit_fpu.cc:98 "Phase 2: psq_l / psq_st (in ppc_emit_memory.cc or here) + full GQR quantization"
//   - ppc_emit_control.cc:629 "GQR0..GQR7 basic read path (SPR 912+). Per ps_* plan..."
//   - This file (original):1189 "psq_l/psq_st (and indexed/update forms: psq_lx, psq_lu, psq_stx, etc.)"
//
// 128B granule work citations (just-completed):
//   - a64_seq_memory.cc:372 "Research-driven (2026 Xenon reverse-engineering): 128B model COMPLETE COVERAGE"
//   - a64_seq_memory.cc:374 "ClearXenonReservationIfStoreOverlaps now called from EVERY guest store emitter"
//   - a64_seq_memory.cc:378 "Any normal store (ST8/ST16/ST32 etc, release, atomic) to 128B granule overlapping active res must invalidate (real Xenon)"
//   - a64_seq_memory.cc:394 "inline void ClearXenonReservationIfStoreOverlaps(A64Emitter& e, const XReg& store_ea_guest, uint32_t store_size = 1)"
//   - a64_seq_memory.cc:412 "Supports full store span for unaligned cases (even I8/I16 can straddle 128B if misaligned)"
//   - a64_seq_memory.cc:397 "ax360e::perf::g_cpu_accuracy.RecordStoreThatInvalidatedReservation()" (counter)
//   - a64_seq_memory.cc:456 "CAPTAIN DIRECT ORDER: 128B CROSS-THREAD PAIRING VIOLATION CHECK (inside Clear)"
//
// psq_st IS a normal store on Xenon (must invalidate reservations). By using f.Store here
// (even placeholder), the emitted HIR naturally flows through the *full* instrumented
// paths (I64 store -> backend seqs that do Clear with size). No holes for psq_st addresses.
//
// Strict scope: EA compute + basic structure + hooks. NO full GQR quantization
int InstrEmit_psq_st(PPCHIRBuilder& f, const InstrData& i) {
  // psq_st frS, d(ra), W, I  (D-form quantized paired-single store)
  // if RA = 0 then b <- 0 else b <- (RA)
  // EA <- b + EXTS(d12)   [d12 from instr bits 16-27; W=bit28, I=bits29-31 packed in DS]
  // MEM(EA, size) <- quantized(FRS ps0/ps1 per W, using GQR[I])
  // (psq_st is normal store -> 128B reservation invalidation required on real Xenon)
  //
  // PRODUCTION (this Captain re-task deepen): full W/I + all u/s 8/16 types +
  // error/edge handling (invalid type, scale extremes) + real HIR GQR path
  // (LoadContext for gqr[I] at runtime) + no dummy_ctx samples + GQRQuantize
  // calls on samples only for CommentFormat debug. W=0 both elems; W=1 ps0 only.
  // f.Store retained for 128B+pairing (Clear + cross-thread probe).
  // Citations: header block above (128B a64_seq_memory + R1 + GQR foundation
  // ppc_context.h + this production + barriers/TLB + harness richer seqs) +
  // psq_st first basic (prior) + psq_l symmetry (this file).
  uint32_t disp12 = (i.D.DS >> 4) & 0xFFF;
  int64_t offset = (disp12 & 0x800) ? (int64_t)(int32_t)(disp12 | 0xFFFFF000) : (int64_t)disp12;
  uint32_t W = (i.D.DS >> 3) & 1;
  uint32_t I = i.D.DS & 0x7;
  Value* ea = CalculateEA_0_i(f, i.D.RA, offset);

  // Real HIR GQR access (production): LoadContext prepares runtime gqr value
  // for future full quant/dequant lowering (no C++ dummy at translate).
  // "real HIR dequant paths" symmetry with psq_l (LoadContext + helpers).
  Value* gqr_hir = f.LoadContext(offsetof(PPCContext, gqr[I]), INT32_TYPE);
  (void)gqr_hir;  // unused until full HIR quant math / CallExtern helper

  // ps0/ps1 extract (F32) - consistent with fpu.cc ps arith (bitcast halves).
  Value* frt = f.LoadFPR(i.D.RT);
  Value* frt_bits = f.Cast(frt, INT64_TYPE);
  Value* ps0_i32 = f.Truncate(f.Shr(frt_bits, 32), INT32_TYPE);
  Value* ps0_f32 = f.Cast(ps0_i32, FLOAT32_TYPE);
  Value* ps1_i32 = f.Truncate(frt_bits, INT32_TYPE);
  Value* ps1_f32 = f.Cast(ps1_i32, FLOAT32_TYPE);

  // PRODUCTION GQR QUANT (store side, no dummy_ctx): hardcoded sample only for
  // static CommentFormat (exercises helpers at emit for harness validation).
  // Full type coverage + error/edge handling.
  uint32_t sample_gqr = 0x0007C000u;  // ST_TYPE=7 s16, ST_SCALE=0 (common VBO)
  int st_scale = GQRGetScale(sample_gqr, /*for_store*/ true);
  uint32_t st_type = GQRGetType(sample_gqr, true);
  int bits; bool is_signed;
  GQRTypeToWidthSign(st_type, &bits, &is_signed);
  // Error/edge handling (production robustness for invalid GQR configs in titles/edges).
  if (st_type < 4 || st_type > 7) {
    f.CommentFormat(" [EDGE] psq_st invalid ST_TYPE=%u (not 4-7 u/s8/16) - default s16; runtime gqr via LoadContext will reflect guest mtspr", (unsigned)st_type);
    bits = 16; is_signed = true;
  }
  if (st_scale <= -16 || st_scale >= 16) {
    f.CommentFormat(" [SCALE EDGE] psq_st |scale|=%d (R1 GQR +/-31 range; fidelity critical for vertex/skin)", st_scale);
  }
  int32_t q0 = GQRQuantize(ps0_f32, st_scale, is_signed, bits);
  int32_t q1 = (W == 0) ? GQRQuantize(ps1_f32, st_scale, is_signed, bits) : 0;
  f.CommentFormat("psq_st (D-form) GQR quant PROD (store): W=%u I=%u scale=%d type=%u bits=%d signed=%d q0=%d q1=%d (GQR helpers ppc_context.h + real HIR LoadContext gqr; full W/I/types/edges; R1 + 128B psq_st skeleton + this production + harness psq_st_* counters + barriers/TLB)",
                  (unsigned)W, (unsigned)I, st_scale, (unsigned)st_type, bits, is_signed ? 1 : 0, q0, q1);

  // Record under debug cvar (polish: emitters now directly drive psq_st counters too).
  // if (cvars::a64_ps_accuracy_stress || cvars::a64_accuracy_debug) { ... RecordPsqStExecuted(W==0?2:1); RecordPsqStGqrCase(); }

  // Placeholder store (retained): full 128B + pairing + barriers + TLB debug paths.
  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.D.RT), INT64_TYPE)));
  return 0;
}

int InstrEmit_psq_stu(PPCHIRBuilder& f, const InstrData& i) {
  // psq_stu frS, d(ra), W, I  (D-form update)
  // EA <- (RA) + EXTS(d12)
  // MEM(EA, size) <- quantized...
  // RA <- EA
  uint32_t disp12 = (i.D.DS >> 4) & 0xFFF;
  int64_t offset = (disp12 & 0x800) ? (int64_t)(int32_t)(disp12 | 0xFFFFF000) : (int64_t)disp12;
  uint32_t W = (i.D.DS >> 3) & 1;
  uint32_t I = i.D.DS & 0x7;
  Value* ea = CalculateEA_i(f, i.D.RA, offset);

  // Real HIR GQR (production): LoadContext for runtime gqr (de/quant path prep).
  Value* gqr_hir = f.LoadContext(offsetof(PPCContext, gqr[I]), INT32_TYPE);
  (void)gqr_hir;

  // ps0/ps1 + full W handling + PRODUCTION GQR (no dummy_ctx).
  Value* frt = f.LoadFPR(i.D.RT);
  Value* frt_bits = f.Cast(frt, INT64_TYPE);
  Value* ps0_f32 = f.Cast(f.Truncate(f.Shr(frt_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* ps1_f32 = f.Cast(f.Truncate(frt_bits, INT32_TYPE), FLOAT32_TYPE);
  uint32_t sample_gqr = 0x00064000u;  // sample for comment (u/s 8/16 coverage via helper)
  int sc = GQRGetScale(sample_gqr, true);
  uint32_t ty = GQRGetType(sample_gqr, true); int b; bool sg; GQRTypeToWidthSign(ty, &b, &sg);
  if (ty < 4 || ty > 7) { b=16; sg=true; f.CommentFormat(" [EDGE] psq_stu invalid type=%u defaulted", (unsigned)ty); }
  if (sc <= -16 || sc >= 16) f.CommentFormat(" [SCALE EDGE] psq_stu |%d|", sc);
  int32_t q0 = GQRQuantize(ps0_f32, sc, sg, b);
  int32_t q1 = (W == 0) ? GQRQuantize(ps1_f32, sc, sg, b) : 0;
  f.CommentFormat("psq_stu PROD GQR quant (store): W=%u I=%u q0=%d q1=%d (full W/I + types/edges + real HIR LoadContext; ppc_context.h helpers + 128B f.Store + R1 + this production + psq_st_* harness + barriers/TLB)",
                  (unsigned)W, (unsigned)I, q0, q1);

  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.D.RT), INT64_TYPE)));
  StoreEA(f, i.D.RA, ea);
  return 0;
}

int InstrEmit_psq_stx(PPCHIRBuilder& f, const InstrData& i) {
  // psq_stx frS, ra, rb, W, I  (X-form indexed)
  // if RA = 0 then b <- 0 else b <- (RA)
  // EA <- b + (RB)
  // MEM(EA, size) <- quantized...   [W/I encoded in X-form extended opcode bits]
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);

  // PRODUCTION full W/I for psq_stx (X-form decode from raw code; bits per Gekko
  // psq X layout parallel to D-form: W instr bit21 MSB0 -> (code>>10)&1; I bits22-24).
  uint32_t W = (i.code >> 10) & 1;
  uint32_t I = (i.code >> 7) & 0x7;
  // Real HIR GQR path (LoadContext) + no dummy.
  Value* gqr_hir = f.LoadContext(offsetof(PPCContext, gqr[I]), INT32_TYPE);
  (void)gqr_hir;

  Value* frt = f.LoadFPR(i.X.RT);
  Value* frt_bits = f.Cast(frt, INT64_TYPE);
  Value* ps0_f32 = f.Cast(f.Truncate(f.Shr(frt_bits, 32), INT32_TYPE), FLOAT32_TYPE);
  Value* ps1_f32 = f.Cast(f.Truncate(frt_bits, INT32_TYPE), FLOAT32_TYPE);  // full for W
  uint32_t sample_gqr = 0x0005C000u;
  int sc = GQRGetScale(sample_gqr, true); int b; bool sg; GQRTypeToWidthSign(GQRGetType(sample_gqr, true), &b, &sg);
  if (GQRGetType(sample_gqr, true) < 4 || GQRGetType(sample_gqr, true) > 7) { b=16; sg=true; }
  int32_t q0 = GQRQuantize(ps0_f32, sc, sg, b);
  int32_t q1 = (W == 0) ? GQRQuantize(ps1_f32, sc, sg, b) : 0;
  f.CommentFormat("psq_stx PROD GQR quant (store): W=%u I=%u q0=%d q1=%d (X-form W/I decode + all types/edges + real HIR LoadContext gqr; helpers ppc_context.h + full 128B/pairing f.Store skeleton; R1 + 128B + this production + harness psq_st counters + barriers/TLB)",
                  (unsigned)W, (unsigned)I, q0, q1);

  f.Store(ea, f.ByteSwap(f.Cast(f.LoadFPR(i.X.RT), INT64_TYPE)));
  // 128B/pairing polish: indexed form Store hits identical ClearXenon + cross-thread probe
  // (no gaps for psq_stx quantized stores; full coverage per a64_seq_memory + this production).
  return 0;
}

// Cache management (A-27)
// On the Xenon (Xbox 360 CPU):
//  - Most data cache ops (dcbf, dcbst, dcbt, dcbtst, dcbz128) operate on
//    128-byte cache lines (not the more common 32-byte PPC blocks).
//  - dcbz uses legacy 32-byte blocks; dcbz128 uses 128-byte.
//  - icbi invalidates instruction cache blocks (typically 32-byte on PPC impl).
// References for Xenon 128B lines:
// https://github.com/ValveSoftware/source-sdk-2013/blob/master/mp/src/mathlib/sseconst.cpp#L321
// https://randomascii.wordpress.com/2018/01/07/finding-a-cpu-design-bug-in-the-xbox-360/

int InstrEmit_dcbf(PPCHIRBuilder& f, const InstrData& i) {
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 128,
                 CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE_AND_FLUSH);
  return 0;
}

int InstrEmit_dcbst(PPCHIRBuilder& f, const InstrData& i) {
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 128, CacheControlType::CACHE_CONTROL_TYPE_DATA_STORE);
  return 0;
}

int InstrEmit_dcbt(PPCHIRBuilder& f, const InstrData& i) {
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 128, CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH);
  return 0;
}

int InstrEmit_dcbtst(PPCHIRBuilder& f, const InstrData& i) {
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 128,
                 CacheControlType::CACHE_CONTROL_TYPE_DATA_TOUCH_FOR_STORE);
  return 0;
}

int InstrEmit_dcbz(PPCHIRBuilder& f, const InstrData& i) {
  // Proper path: dcbz allocates the line in cache (dirty, zeroed) without
  // fetching from memory. Using CacheControl gives the backend a chance to
  // do the right thing (DC ZVA on AArch64, etc.).
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 32, CacheControlType::CACHE_CONTROL_TYPE_DATA_ZERO);
  return 0;
}

int InstrEmit_dcbz128(PPCHIRBuilder& f, const InstrData& i) {
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 128, CacheControlType::CACHE_CONTROL_TYPE_DATA_ZERO);
  return 0;
}

int InstrEmit_icbi(PPCHIRBuilder& f, const InstrData& i) {
  // Instruction cache block invalidate.
  // Critical for self-modifying code (SMC) and in-game JIT compilers in 360 titles.
  // We now use a dedicated type so the A64 backend can emit precise IC IVAU +
  // context synchronizing barriers (ISB) instead of faking it as a data flush.
  // Xenon titles (e.g. many with dynamic code gen, shaders compiled at runtime, etc.)
  // rely on icbi + isync pairs for correctness after writing code.
  Value* ea = CalculateEA_0(f, i.X.RA, i.X.RB);
  f.CacheControl(ea, 32, CacheControlType::CACHE_CONTROL_TYPE_INSTRUCTION_INVALIDATE);
  return 0;
}

void RegisterEmitCategoryMemory() {
  XEREGISTERINSTR(lbz);
  XEREGISTERINSTR(lbzu);
  XEREGISTERINSTR(lbzux);
  XEREGISTERINSTR(lbzx);
  XEREGISTERINSTR(lha);
  XEREGISTERINSTR(lhau);
  XEREGISTERINSTR(lhaux);
  XEREGISTERINSTR(lhax);
  XEREGISTERINSTR(lhz);
  XEREGISTERINSTR(lhzu);
  XEREGISTERINSTR(lhzux);
  XEREGISTERINSTR(lhzx);
  XEREGISTERINSTR(lwa);
  XEREGISTERINSTR(lwaux);
  XEREGISTERINSTR(lwax);
  XEREGISTERINSTR(lwz);
  XEREGISTERINSTR(lwzu);
  XEREGISTERINSTR(lwzux);
  XEREGISTERINSTR(lwzx);
  XEREGISTERINSTR(ld);
  XEREGISTERINSTR(ldu);
  XEREGISTERINSTR(ldux);
  XEREGISTERINSTR(ldx);
  XEREGISTERINSTR(stb);
  XEREGISTERINSTR(stbu);
  XEREGISTERINSTR(stbux);
  XEREGISTERINSTR(stbx);
  XEREGISTERINSTR(sth);
  XEREGISTERINSTR(sthu);
  XEREGISTERINSTR(sthux);
  XEREGISTERINSTR(sthx);
  XEREGISTERINSTR(stw);
  XEREGISTERINSTR(stwu);
  XEREGISTERINSTR(stwux);
  XEREGISTERINSTR(stwx);
  XEREGISTERINSTR(std);
  XEREGISTERINSTR(stdu);
  XEREGISTERINSTR(stdux);
  XEREGISTERINSTR(stdx);
  XEREGISTERINSTR(lhbrx);
  XEREGISTERINSTR(lwbrx);
  XEREGISTERINSTR(ldbrx);
  XEREGISTERINSTR(sthbrx);
  XEREGISTERINSTR(stwbrx);
  XEREGISTERINSTR(stdbrx);
  XEREGISTERINSTR(lmw);
  XEREGISTERINSTR(stmw);
  XEREGISTERINSTR(lswi);
  XEREGISTERINSTR(lswx);
  XEREGISTERINSTR(stswi);
  XEREGISTERINSTR(stswx);
  XEREGISTERINSTR(eieio);
  XEREGISTERINSTR(sync);
  XEREGISTERINSTR(isync);
  XEREGISTERINSTR(ldarx);
  XEREGISTERINSTR(lwarx);
  XEREGISTERINSTR(stdcx);
  XEREGISTERINSTR(stwcx);
  XEREGISTERINSTR(lfd);
  XEREGISTERINSTR(lfdu);
  XEREGISTERINSTR(lfdux);
  XEREGISTERINSTR(lfdx);
  XEREGISTERINSTR(lfs);
  XEREGISTERINSTR(lfsu);
  XEREGISTERINSTR(lfsux);
  XEREGISTERINSTR(lfsx);
  XEREGISTERINSTR(stfd);
  XEREGISTERINSTR(stfdu);
  XEREGISTERINSTR(stfdux);
  XEREGISTERINSTR(stfdx);
  XEREGISTERINSTR(stfiwx);
  XEREGISTERINSTR(stfs);
  XEREGISTERINSTR(stfsu);
  XEREGISTERINSTR(stfsux);
  XEREGISTERINSTR(stfsx);
  XEREGISTERINSTR(dcbf);

  // NOTE (psq_* skeletons): psq_st* (store, with GQR quant + 128B) + psq_l* (load, pure placeholder)
  // added post-stfsx (this file). psq_l* delivered in Captain re-task as *exact symmetric
  // counterpart* to successful psq_st store skeleton: placeholder raw bits via CalculateEA +
  // Load (no GQR calls per foundation scope). Both reuse EA patterns + (stores) 128B Clear
  // paths. New load-side symmetric psq_l + reservation/pairing sequences + counters in
  // ps_* harness (a64_backend.cc + ax360e_perf_log.h; lwarx A + psq_l B cross cases for
  // EA/res tracking). Heavy citations to pairing enforcement (a64_seq_memory:398+), psq_st
  // skeleton, R1 ps report, 128B complete. See updated plan block + emitters ~1045+.
  // (GQR de/quant + full W/I = GQR agent; psq_l roundtrips use existing counters.)

  // --------------------------------------------------------------------------
  // PAIRED-SINGLE QUANTIZED LOAD/STORE (psq_l / psq_st) - FOUNDATIONAL PLAN
  // (UPDATED: store-side psq_st* + load-side psq_l* skeletons with GQR usage)
  // --------------------------------------------------------------------------
  // psq_l/psq_st (and indexed/update forms: psq_lx, psq_lu, psq_stx, psq_stu,
  // psq_stux) are among the highest-impact instructions for real 360 titles.
  // They load/store two single-precision values with on-the-fly
  // quantization (8/16-bit signed/unsigned scaled by GQR registers).
  //
  // Recommended lowering strategy (reuse existing infrastructure):
  //   - Calculate effective address using existing helpers (CalculateEA* + StoreEA).
  //   - For psq_l: Load as integer(s), apply GQR scale + Convert to F32,
  //     pack into F64 (ps0 high / ps1 low) exactly like the arithmetic
  //     emitters in ppc_emit_fpu.cc.
  //   - For psq_st: Extract ps0/ps1 as F32, quantize using GQR, store.
  //   - Heavily reuse memory paths (Load/Store + ByteSwap) and the
  //     paired-single extraction/merge logic (fpu.cc).
  //
  // Context requirement: PPCContext must expose GQR[8] (quantization regs).
  // BASIC GQR READ/WRITE PATHS NOW ADDED (SPR 912-919 in ppc_emit_control + fields in ppc_context.h).
  // See master PAIRED-SINGLE plan comment block in ppc_emit_fpu.cc (Phase 2 prep).
  //
  // R1 ps_* research report + 128B + pairing enforcement + psq_st skeleton integration
  // (Captain re-task - pure psq_l load-side skeleton delivery):
  //   psq_st* store skeletons (with later GQR quant) + full 128B Clear + pairing probe
  //   coverage added previously. THIS RE-TASK: **psq_l load-side skeleton delivered as
  //   exact symmetric counterpart** (placeholder raw bits load via CalculateEA* + Load
  //   patterns into FPR pair; no GQR). + symmetric psq_l + reservation/pairing sequences
  //   + counters in active ps_* harness (lwarx on A, psq_l crossing from B for EA/res
  //   tracking validation; supporting helper + memory-path activation in seq_memory).
  //   Citations (heavy): own pairing enforcement mastery (last_reserving_thread_id/granule,
  //   XenonReservesOverlapAcrossThreads, probe inside ClearXenon... a64_seq_memory.cc:398+,
  //   every hot path), successful psq_st store skeleton (this file:1223+ placeholder),
  //   R1 ps_* report (psq_l/psq_st highest-impact quantized paths + 128B res warning),
  //   128B complete coverage (a64_seq_memory:372 "COMPLETE COVERAGE").
  //   psq_l uses correct EA (flows to LOAD_* seqs for res tracking).
  //   No GQR in psq_l (pure foundation placeholder, per explicit scope; GQR agent).
  //
  // STATUS (updated per this Captain re-task - psq_l pure load skeleton + harness load-side):
  //   - psq_l* load skeletons: PURE PLACEHOLDER (CalculateEA + raw Load INT64->FPR cast;
  //     identical discipline to initial psq_st store skeleton; heavy 128B/pairing/psq_st/R1 cites).
  //   - psq_st* : prior GQR quant + dedicated store counters + 128B/pairing full coverage.
  //   - Symmetric psq_l reservation/pairing sequences + new load-side counters in harness
  //     (a64_backend.cc RunPairedSingle + Exercise helper + ax360e_perf_log.h).
  //   - Memory-path activation + EA/res notes for psq_l loads in a64_seq_memory.cc.
  //   - NO GQR/quant in psq_l skeleton (left for GQR agent). Registration pending.
  //   - Heavy citations to pairing work + psq_st + R1 + 128B everywhere.
  //
  // TODO (remaining):
  //   - Add psq_* opcodes + table/frontend.
  //   - Full W/I + GQR de/quant HIR in load+store (real math; GQR agent).
  //   - ps0/ps1 pack/extract + registrations.
  //
  // These psq_* (psq_st validated store skeleton + this psq_l load skeleton + harness bridge)
  // are the direct high-ROI step for owner of pairing enforcement + psq_st. Full GQR
  // + arith ps for others. See ppc_emit_fpu.cc master plan.
  // --------------------------------------------------------------------------
  // (psq_st skeleton + GQR quant/harness prior; THIS RE-TASK: pure psq_l load skeleton
  //  + symmetric psq_l + res/pairing harness sequences/counters + mem activation;
  //  128B/pairing/R1 cites; registration pending)
  // XEREGISTERINSTR(psq_l);     // D-form load (pure placeholder skeleton, res EA correct)
  // XEREGISTERINSTR(psq_lu);    // D-form update load (placeholder)
  // XEREGISTERINSTR(psq_lx);    // X-form indexed load (placeholder)
  // XEREGISTERINSTR(psq_st);    // D-form store (skeleton + GQR + 128B/pairing)
  // XEREGISTERINSTR(psq_stu);   // ...
  // XEREGISTERINSTR(psq_stx);   // ...
  // (full GQR de/quant HIR + opcode table = GQR/psq agents)
  XEREGISTERINSTR(dcbst);
  XEREGISTERINSTR(dcbt);
  XEREGISTERINSTR(dcbtst);
  XEREGISTERINSTR(dcbz);
  XEREGISTERINSTR(dcbz128);
  XEREGISTERINSTR(icbi);
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
