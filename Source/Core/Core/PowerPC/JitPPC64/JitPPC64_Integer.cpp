// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Integer instructions. Guest values are 32-bit, in the low halves of 64-bit host registers whose
// upper halves are undefined, so the host's 32-bit ("word") instructions do most of the work.
// Flags are the exception: the host computes XER[CA] and CR0 from 64-bit results, so carries and
// CR0 are computed explicitly for 32 bits.

#include "Core/PowerPC/JitPPC64/Jit.h"

#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/ConditionRegister.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"

using namespace PPC64Gen;

void JitPPC64::ComputeRC0(GPR value)
{
  // As Interpreter::Helper_UpdateCR0: the sign-extended value, with SO copied from XER, and bit 63
  // set if the value is zero so that setting SO doesn't make GT true.
  const auto cr = gpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  EXTSW(cr, value);
  LBZ(tmp, PPC_REG, PPCSTATE_OFF(xer_so_ov));
  RLWINM(tmp, tmp, 31, 31, 31);  // SO
  RLDIMI(cr, tmp, PowerPC::CR_EMU_SO_BIT, 63 - PowerPC::CR_EMU_SO_BIT);
  CNTLZW(tmp, value);
  RLWINM(tmp, tmp, 27, 31, 31);  // 1 if the value is zero
  SLDI(tmp, tmp, 63);
  OR(cr, cr, tmp);
  STD(cr, PPC_REG, PPCSTATE_OFF_CR(0));
}

void JitPPC64::SetCRFieldFromHostCompare(u32 field, CRF host_field)
{
  // Build the four PowerPC bits (LT, GT, EQ and SO from XER) and convert them like
  // ConditionRegister::SetField, with its table.
  const auto bits = gpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  MFOCRF(bits, host_field);
  // Rotate the field's LT, GT and EQ into bits 3-1.
  const u32 shift = (4 * DecodeReg(host_field) + 4) % 32;
  RLWINM(bits, bits, shift, 28, 30);
  LBZ(tmp, PPC_REG, PPCSTATE_OFF(xer_so_ov));
  RLWINM(tmp, tmp, 31, 31, 31);  // SO
  OR(bits, bits, tmp);
  SetCRField(field, bits);
}

void JitPPC64::GetCRField(GPR out, u32 field)
{
  // As ConditionRegister::GetField.
  const auto cr = gpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  LD(cr, PPC_REG, PPCSTATE_OFF_CR(field));
  // LT and SO: bits 62 and 59 to bits 3 and 0.
  SRDI(out, cr, PowerPC::CR_EMU_SO_BIT);
  ANDI_(out, out, PowerPC::CR_LT | PowerPC::CR_SO);
  // EQ: the low 32 bits are zero.
  CNTLZW(tmp, cr);
  RLWINM(tmp, tmp, 28, 30, 30);  // (cntlzw >> 5) << 1
  OR(out, out, tmp);
  // GT: the value is positive.
  NEG(tmp, cr);
  ANDC(tmp, tmp, cr);  // Negative only if cr > 0
  SRDI(tmp, tmp, 63);
  RLDIMI(out, tmp, PowerPC::CR_GT_BIT, 63 - PowerPC::CR_GT_BIT);
}

void JitPPC64::SetCRField(u32 field, GPR value)
{
  // As ConditionRegister::SetField: look the field's value up in its table.
  const auto table = gpr.GetScopedReg();
  const auto offset = gpr.GetScopedReg();
  MOVP2R(table, PowerPC::ConditionRegister::s_crTable.data());
  RLDIC(offset, value, 3, 64 - 7);  // (value & 0xF) * 8
  LDX(offset, table, offset);
  STD(offset, PPC_REG, PPCSTATE_OFF_CR(field));
}

FixupBranch JitPPC64::JumpIfCRFieldBit(u32 field, u32 bit, bool jump_if_set)
{
  const auto cr = gpr.GetScopedReg();
  LD(cr, PPC_REG, PPCSTATE_OFF_CR(field));

  switch (bit)
  {
  case PowerPC::CR_SO_BIT:  // bit 59 set
    RLDICL(cr, cr, 64 - PowerPC::CR_EMU_SO_BIT, 63, true);
    return BC(jump_if_set ? CCFlags::NE : CCFlags::EQ, CRF::CR0);
  case PowerPC::CR_EQ_BIT:  // bits 31-0 == 0
    CMPWI(CRF::CR0, cr, 0);
    return BC(jump_if_set ? CCFlags::EQ : CCFlags::NE, CRF::CR0);
  case PowerPC::CR_GT_BIT:  // val > 0
    CMPDI(CRF::CR0, cr, 0);
    return BC(jump_if_set ? CCFlags::GT : CCFlags::LE, CRF::CR0);
  case PowerPC::CR_LT_BIT:  // bit 62 set
  default:
    RLDICL(cr, cr, 64 - PowerPC::CR_EMU_LT_BIT, 63, true);
    return BC(jump_if_set ? CCFlags::NE : CCFlags::EQ, CRF::CR0);
  }
}

void JitPPC64::AddWithCarry(GPR d, GPR x, GPR y, CarryIn carry_in)
{
  // Add the zero-extended values in 64 bits, where the carry out of 32 bits is bit 32.
  const auto sum = gpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  CLRLDI(sum, x, 32);
  CLRLDI(tmp, y, 32);
  ADD(sum, sum, tmp);
  if (carry_in == CarryIn::One)
  {
    ADDI(sum, sum, 1);
  }
  else if (carry_in == CarryIn::XER)
  {
    LBZ(tmp, PPC_REG, PPCSTATE_OFF(xer_ca));
    ADD(sum, sum, tmp);
  }
  SRDI(tmp, sum, 32);
  STB(tmp, PPC_REG, PPCSTATE_OFF(xer_ca));
  MR(d, sum);
}

void JitPPC64::StoreHostCarry()
{
  // The host's word shifts compute CA the way the guest's do.
  const auto tmp = gpr.GetScopedReg();
  MFXER(tmp);
  RLWINM(tmp, tmp, 3, 31, 31);  // XER[CA] is 0x20000000
  STB(tmp, PPC_REG, PPCSTATE_OFF(xer_ca));
}

void JitPPC64::addix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const u32 imm = inst.OPCD == 15 ? static_cast<u32>(inst.SIMM_16) << 16 :
                                    static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  if (inst.RA == 0)
  {
    // li or lis
    MOVI2R(gpr.RW(inst.RD), imm);
    return;
  }

  const GPR a = gpr.R(inst.RA);
  const GPR d = gpr.RW(inst.RD);
  if (inst.OPCD == 15)
    ADDIS(d, a, inst.SIMM_16);
  else
    ADDI(d, a, inst.SIMM_16);
}

void JitPPC64::addic(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR a = gpr.R(inst.RA);
  const auto imm = gpr.GetScopedReg();
  MOVI2R(imm, static_cast<u32>(static_cast<s32>(inst.SIMM_16)));
  const GPR d = gpr.RW(inst.RD);
  AddWithCarry(d, a, imm, CarryIn::Zero);
  // addic. is opcode 13
  if (inst.OPCD == 13)
    ComputeRC0(d);
}

void JitPPC64::subfic(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  // d = imm - a = ~a + imm + 1
  const auto not_a = gpr.GetScopedReg();
  NOT(not_a, gpr.R(inst.RA));
  const auto imm = gpr.GetScopedReg();
  MOVI2R(imm, static_cast<u32>(static_cast<s32>(inst.SIMM_16)));
  AddWithCarry(gpr.RW(inst.RD), not_a, imm, CarryIn::One);
}

void JitPPC64::mulli(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR a = gpr.R(inst.RA);
  MULLI(gpr.RW(inst.RD), a, inst.SIMM_16);
}

void JitPPC64::arith_imm(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR a = gpr.RW(inst.RA);
  switch (inst.OPCD)
  {
  case 24:  // ori
    ORI(a, s, inst.UIMM);
    break;
  case 25:  // oris
    ORIS(a, s, inst.UIMM);
    break;
  case 26:  // xori
    XORI(a, s, inst.UIMM);
    break;
  case 27:  // xoris
    XORIS(a, s, inst.UIMM);
    break;
  case 28:  // andi.
    ANDI_(a, s, inst.UIMM);
    ComputeRC0(a);
    break;
  case 29:  // andis.
    ANDIS_(a, s, inst.UIMM);
    ComputeRC0(a);
    break;
  }
}

void JitPPC64::boolX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR b = gpr.R(inst.RB);
  const GPR a = gpr.RW(inst.RA);
  switch (inst.SUBOP10)
  {
  case 28:  // and
    AND(a, s, b);
    break;
  case 60:  // andc
    ANDC(a, s, b);
    break;
  case 444:  // or
    OR(a, s, b);
    break;
  case 412:  // orc
    ORC(a, s, b);
    break;
  case 316:  // xor
    XOR(a, s, b);
    break;
  case 476:  // nand
    NAND(a, s, b);
    break;
  case 124:  // nor
    NOR(a, s, b);
    break;
  case 284:  // eqv
    EQV(a, s, b);
    break;
  }
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::extsXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR a = gpr.RW(inst.RA);
  if (inst.SUBOP10 == 954)
    EXTSB(a, s);
  else
    EXTSH(a, s);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::cntlzwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR a = gpr.RW(inst.RA);
  CNTLZW(a, s);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::addx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  ADD(d, a, b);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::addcx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  AddWithCarry(d, a, b, CarryIn::Zero);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::addex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const GPR a = gpr.R(inst.RA);
  if (inst.SUBOP10 == 234)
  {
    // addme: d = a + CA - 1
    const auto minus_one = gpr.GetScopedReg();
    LI(minus_one, -1);
    const GPR d = gpr.RW(inst.RD);
    AddWithCarry(d, a, minus_one, CarryIn::XER);
    if (inst.Rc)
      ComputeRC0(d);
    return;
  }

  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  AddWithCarry(d, a, b, CarryIn::XER);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::addzex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const auto zero = gpr.GetScopedReg();
  LI(zero, 0);
  const GPR a = gpr.R(inst.RA);
  const GPR d = gpr.RW(inst.RD);
  AddWithCarry(d, a, zero, CarryIn::XER);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::subfx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);

  if (inst.SUBOP10 == 8)
  {
    // subfc: d = b - a = ~a + b + 1
    const auto not_a = gpr.GetScopedReg();
    NOT(not_a, a);
    const GPR d = gpr.RW(inst.RD);
    AddWithCarry(d, not_a, b, CarryIn::One);
    if (inst.Rc)
      ComputeRC0(d);
    return;
  }

  const GPR d = gpr.RW(inst.RD);
  SUBF(d, a, b);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::subfex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  // d = ~a + b + CA, or ~a - 1 + CA for subfme
  const auto not_a = gpr.GetScopedReg();
  NOT(not_a, gpr.R(inst.RA));
  if (inst.SUBOP10 == 232)
  {
    const auto minus_one = gpr.GetScopedReg();
    LI(minus_one, -1);
    const GPR d = gpr.RW(inst.RD);
    AddWithCarry(d, not_a, minus_one, CarryIn::XER);
    if (inst.Rc)
      ComputeRC0(d);
    return;
  }
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  AddWithCarry(d, not_a, b, CarryIn::XER);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::subfzex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  // d = ~a + CA
  const auto not_a = gpr.GetScopedReg();
  NOT(not_a, gpr.R(inst.RA));
  const auto zero = gpr.GetScopedReg();
  LI(zero, 0);
  const GPR d = gpr.RW(inst.RD);
  AddWithCarry(d, not_a, zero, CarryIn::XER);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::negx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const GPR a = gpr.R(inst.RA);
  const GPR d = gpr.RW(inst.RD);
  NEG(d, a);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::mullwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  MULLW(d, a, b);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::mulhwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  MULHW(d, a, b);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::mulhwux(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  MULHWU(d, a, b);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::divwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  // The host leaves the result undefined when dividing by zero, or 0x80000000 by -1. As the
  // interpreter, the result is then -1 if the dividend is negative, and 0 otherwise.
  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);
  const auto tmp = gpr.GetScopedReg();

  CMPWI(CRF::CR1, b, 0);
  const FixupBranch by_zero = BC(CCFlags::EQ, CRF::CR1);
  CMPWI(CRF::CR1, b, -1);
  const FixupBranch not_minus_one = BC(CCFlags::NE, CRF::CR1);
  LIS(tmp, static_cast<s16>(0x8000));
  CMPW(CRF::CR1, a, tmp);
  const FixupBranch overflow = BC(CCFlags::EQ, CRF::CR1);

  SetJumpTarget(not_minus_one);
  DIVW(d, a, b);
  const FixupBranch done = B();

  SetJumpTarget(by_zero);
  SetJumpTarget(overflow);
  SRAWI(d, a, 31);

  SetJumpTarget(done);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::divwux(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  INTERPRET_IF(inst.OE);

  // The interpreter returns 0 when dividing by zero, where the host leaves it undefined.
  const GPR a = gpr.R(inst.RA);
  const GPR b = gpr.R(inst.RB);
  const GPR d = gpr.RW(inst.RD);

  CMPLWI(CRF::CR1, b, 0);
  const FixupBranch by_zero = BC(CCFlags::EQ, CRF::CR1);
  DIVWU(d, a, b);
  const FixupBranch done = B();
  SetJumpTarget(by_zero);
  LI(d, 0);
  SetJumpTarget(done);
  if (inst.Rc)
    ComputeRC0(d);
}

void JitPPC64::slwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR b = gpr.R(inst.RB);
  const GPR a = gpr.RW(inst.RA);
  SLW(a, s, b);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::srwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR b = gpr.R(inst.RB);
  const GPR a = gpr.RW(inst.RA);
  SRW(a, s, b);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::srawx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR b = gpr.R(inst.RB);
  const GPR a = gpr.RW(inst.RA);
  SRAW(a, s, b);
  StoreHostCarry();
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::srawix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR a = gpr.RW(inst.RA);
  SRAWI(a, s, inst.SH);
  StoreHostCarry();
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::rlwinmx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR a = gpr.RW(inst.RA);
  RLWINM(a, s, inst.SH, inst.MB, inst.ME);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::rlwnmx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR b = gpr.R(inst.RB);
  const GPR a = gpr.RW(inst.RA);
  RLWNM(a, s, b, inst.MB, inst.ME);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::rlwimix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  const GPR s = gpr.R(inst.RS);
  const GPR a = gpr.RW(inst.RA, true);
  RLWIMI(a, s, inst.SH, inst.MB, inst.ME);
  if (inst.Rc)
    ComputeRC0(a);
}

void JitPPC64::cmp(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  CMPW(CRF::CR1, gpr.R(inst.RA), gpr.R(inst.RB));
  SetCRFieldFromHostCompare(inst.CRFD, CRF::CR1);
}

void JitPPC64::cmpl(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  CMPLW(CRF::CR1, gpr.R(inst.RA), gpr.R(inst.RB));
  SetCRFieldFromHostCompare(inst.CRFD, CRF::CR1);
}

void JitPPC64::cmpi(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  CMPWI(CRF::CR1, gpr.R(inst.RA), inst.SIMM_16);
  SetCRFieldFromHostCompare(inst.CRFD, CRF::CR1);
}

void JitPPC64::cmpli(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  CMPLWI(CRF::CR1, gpr.R(inst.RA), inst.UIMM);
  SetCRFieldFromHostCompare(inst.CRFD, CRF::CR1);
}
