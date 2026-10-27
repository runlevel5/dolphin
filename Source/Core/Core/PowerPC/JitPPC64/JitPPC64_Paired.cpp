// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Paired-single instructions. The arithmetic works on both halves at once, held as singles (see
// PPC64FPRCache), with the vector single precision instructions. With single operands, these
// round as the interpreter's operations in double precision and rounding to single precision do:
// double precision is wide enough for the result of one operation on singles to round to single
// precision the same either way, a single frC needs no rounding to 25 bits, and the interpreter
// corrects the ties of fused multiply-adds, which leaves the result that a single rounding gives.
//
// Operands held as doubles are converted, and checked to be singles; those that later paired single
// instructions read too are then held as singles. The interpreter computes the rest, from far
// code: operands that aren't singles, and results that it may compute differently, which are tiny:
// FPSCR[NI] flushes a result that rounds up to the smallest normal single from below it, as well as
// denormals, and a fused multiply-add rounded twice can come out differently for a denormal.

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PowerPC.h"

using namespace PPC64Gen;

void JitPPC64::BindPairedOperand(PairedOperand& operand, u32 preg)
{
  operand.preg = preg;
  operand.single = fpr.HoldsSingles(preg);
  operand.host = operand.single ? fpr.RSingle(preg) : fpr.R(preg);
}

bool JitPPC64::IsReadAgainAsSingles(u32 preg) const
{
  // By a later paired single instruction of the block, before another writes it.
  for (int i = 1; i <= js.instructionsLeft; i++)
  {
    const PPCAnalyst::CodeOp& op = js.op[i];
    if (op.skip)
      continue;
    if (op.fregsIn[preg] &&
        (op.opinfo->type == OpType::PS || op.opinfo->type == OpType::StorePS))
    {
      return true;
    }
    if (op.fregOut == static_cast<s8>(preg))
      return false;
  }
  return false;
}

void JitPPC64::GetPairedSingles(UGeckoInstruction inst, std::span<PairedOperand> operands,
                                std::vector<FixupBranch>& not_singles)
{
  // Converted and back, the doubles are the same if they're singles; a signaling NaN is quieted,
  // and left to the interpreter. Holding them as singles in the cache after needs the code after
  // to run only if they are: then the branch for each goes to far code that interprets the
  // instruction and leaves the block, with the cache as it is before any of them are.
  bool keep = false;
  for (const PairedOperand& operand : operands)
    keep |= !operand.single && IsReadAgainAsSingles(operand.preg);

  for (auto it = operands.begin(); it != operands.end(); ++it)
  {
    PairedOperand& operand = *it;
    const auto same = std::find_if(operands.begin(), it, [&](const PairedOperand& other) {
      return other.preg == operand.preg;
    });
    if (operand.single || same != it)
    {
      operand.reg = operand.single ? operand.host : same->reg;
      continue;
    }

    operand.singles = fpr.GetScopedReg();
    const auto check = fpr.GetScopedReg();
    ConvertDoublesToSingles(operand.singles, operand.host);
    XVCVSPDP(check, operand.singles);
    VCMPEQUD(check, check, operand.host, true);
    // CR6's LT is set if both doublewords are equal.
    const FixupBranch not_single = BCFar(CCFlags::GE, CRF::CR6);
    if (keep)
    {
      SwitchToFarCode();
      SetJumpTarget(not_single);
      InterpretAndExit(inst);
      SwitchToNearCode();
    }
    else
    {
      not_singles.push_back(not_single);
    }
    operand.reg = operand.singles;
  }

  if (!keep)
    return;
  for (PairedOperand& operand : operands)
  {
    if (operand.single || fpr.HoldsSingles(operand.preg))
      continue;
    XXLMR(operand.host, operand.reg);
    fpr.KeepAsSingles(operand.preg);
  }
}

void JitPPC64::InterpretAndExit(UGeckoInstruction inst)
{
  // The guest registers are written back for the interpreter, and the block continues at the next
  // instruction.
  FlushRegisters(FlushMode::MaintainState);
  MOVI2R(GPR::R12, js.compilerPC);
  STW(GPR::R12, PPC_REG, PPCSTATE_OFF(pc));
  ABI_CallFunction(Interpreter::GetInterpreterOp(inst), &m_system.GetInterpreter(), inst.hex);
  WriteExit(js.compilerPC + 4);
}

FixupBranch JitPPC64::BranchIfTinySingles(VSR singles)
{
  // Whether a word's magnitude is a denormal or the smallest normal single, 0x00800000, but not 0:
  // added to bias, 0 gives bias itself, and those give more, wrapping around from the magnitudes
  // above them. Power ISA 3.0's xxspltib makes a bias of 0xFEFEFEFE, which also takes magnitudes up
  // to 0x01010101, a little above 2^-125, to far code: rare, and computed there the same.
  const auto magnitude = fpr.GetScopedReg();
  const auto bias = fpr.GetScopedReg();
  XVABSSP(magnitude, singles);
  if (m_isa_3_1)
  {
    XXSPLTIW(bias, 0xFF7FFFFF);
  }
  else if (m_isa_3_0)
  {
    XXSPLTIB(bias, 0xFE);
  }
  else
  {
    // ~(1 << 23), shifting by the low 5 bits of -9.
    const auto shift = fpr.GetScopedReg();
    VSPLTISW(bias, 1);
    VSPLTISW(shift, -9);
    VSLW(bias, bias, shift);
    XXLNOR(bias, bias, bias);
  }
  VADDUWM(magnitude, magnitude, bias);
  VCMPGTUW(magnitude, magnitude, bias, true);
  // CR6's EQ is set if no word is.
  return BCFar(CCFlags::NE, CRF::CR6);
}

FixupBranch JitPPC64::EmitTinySinglesFallback(FixupBranch tiny, bool fma, const u8* resume)
{
  // Without NI, which is in CR4, the interpreter only computes a fused multiply-add differently.
  SetJumpTarget(tiny);
  if (!fma)
  {
    const FixupBranch ni = BC(CCFlags::NE, CRF::CR4);
    B(resume);
    SetJumpTarget(ni);
  }
  return B();
}

void JitPPC64::SetFPRFFromSingles(VSR singles)
{
  const auto ps0 = fpr.GetScopedReg();
  XSCVSPDPN(ps0, singles);
  SetFPRF(ps0, true);
}

void JitPPC64::ps_arith(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || (jo.div_by_zero_exceptions && inst.SUBOP5 == 18));

  // ps_muls0, ps_muls1, ps_madds0 and ps_madds1 are ps_mul and ps_madd, with one half of C for
  // both halves.
  u32 op5 = inst.SUBOP5;
  int c_half = -1;
  switch (op5)
  {
  case 12:  // ps_muls0
  case 13:  // ps_muls1
    c_half = op5 - 12;
    op5 = 25;
    break;
  case 14:  // ps_madds0
  case 15:  // ps_madds1
    c_half = op5 - 14;
    op5 = 29;
    break;
  default:
    break;
  }

  const bool use_c = op5 >= 25;  // ps_mul and the fused multiply-adds
  const bool use_b = op5 != 25;  // ps_mul doesn't use B
  const bool fma = use_b && use_c;

  // All guest registers are bound before the branches to far code, so that the cache is the same
  // on every path.
  std::array<PairedOperand, 3> operands;
  size_t count = 0;
  BindPairedOperand(operands[count++], inst.FA);
  if (use_b)
    BindPairedOperand(operands[count++], inst.FB);
  if (use_c)
    BindPairedOperand(operands[count++], inst.FC);
  const VSR d = fpr.BindForSingles(inst.FD);
  EnsureNILoaded();

  std::vector<FixupBranch> interpret;
  GetPairedSingles(inst, std::span(operands.data(), count), interpret);
  const bool d_was_single = fpr.HoldsSingles(inst.FD);
  FixupBranch tiny;
  const u8* resume;
  {
    const VSR a_operand = operands[0].reg;
    const VSR b_operand = use_b ? operands[1].reg : VSR::VS0;
    VSR c_operand = use_c ? operands[count - 1].reg : VSR::VS0;

    // C's half for both halves, for ps_muls0, ps_muls1, ps_madds0 and ps_madds1.
    PPC64FPRCache::ScopedReg c_halves;
    if (c_half >= 0)
    {
      c_halves = fpr.GetScopedReg();
      XXSPLTW(c_halves, c_operand, 2 * c_half);
      c_operand = c_halves;
    }

    const auto result = fpr.GetScopedReg();
    switch (op5)
    {
    case 18:
      XVDIVSP(result, a_operand, b_operand);
      break;
    case 20:
      XVSUBSP(result, a_operand, b_operand);
      break;
    case 21:
      XVADDSP(result, a_operand, b_operand);
      break;
    case 25:
      XVMULSP(result, a_operand, c_operand);
      break;
    default:
      // These compute result = a * c + result; the negated forms negate after rounding, except
      // NaNs, as the guest's do.
      XXLMR(result, b_operand);
      switch (op5)
      {
      case 28:
        XVMSUBASP(result, a_operand, c_operand);
        break;
      case 29:
        XVMADDASP(result, a_operand, c_operand);
        break;
      case 30:
        XVNMSUBASP(result, a_operand, c_operand);
        break;
      default:
        XVNMADDASP(result, a_operand, c_operand);
        break;
      }
      break;
    }

    tiny = BranchIfTinySingles(result);
    resume = GetCodePtr();
    XXLMR(d, result);
    if (m_fprf && js.op->wantsFPRF)
      SetFPRFFromSingles(result);
  }
  const u8* done = GetCodePtr();
  fpr.MarkSingles(inst.FD, true);
  // The far code calls the interpreter, which needs the scratch registers free.
  for (PairedOperand& operand : operands)
    operand.singles.Unlock();

  SwitchToFarCode();
  interpret.push_back(EmitTinySinglesFallback(tiny, fma, resume));
  for (const FixupBranch& branch : interpret)
    SetJumpTarget(branch);
  InterpretFromFarCode(inst, done, d_was_single);
  SwitchToNearCode();
}

void JitPPC64::ps_sumX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  // ps_sum0: a.ps0 + b.ps1, c.ps1; ps_sum1: c.ps0, a.ps0 + b.ps1. Both halves are rounded to single
  // precision, which leaves a single half of C as it is, unless NI flushes it, or it's a signaling
  // NaN, which is quieted.
  const bool sum1 = inst.SUBOP5 == 11;
  std::array<PairedOperand, 3> operands;
  BindPairedOperand(operands[0], inst.FA);
  BindPairedOperand(operands[1], inst.FB);
  BindPairedOperand(operands[2], inst.FC);
  const VSR d = fpr.BindForSingles(inst.FD);
  EnsureNILoaded();

  std::vector<FixupBranch> interpret;
  GetPairedSingles(inst, operands, interpret);
  const bool d_was_single = fpr.HoldsSingles(inst.FD);
  FixupBranch tiny;
  const u8* resume;
  {
    const VSR a_operand = operands[0].reg;
    const VSR b_operand = operands[1].reg;
    VSR c_operand = operands[2].reg;
    PPC64FPRCache::ScopedReg c_quiet;
    if (operands[2].single && !fpr.IsQuiet(inst.FC))
    {
      // Converting to doubles and back quiets signaling NaNs, which converted doubles aren't.
      c_quiet = fpr.GetScopedReg();
      XVCVSPDP(c_quiet, c_operand);
      ConvertDoublesToSingles(c_quiet, c_quiet);
      c_operand = c_quiet;
    }

    // The sum in words 0 and 1, from b.ps1 rotated there.
    const auto sum = fpr.GetScopedReg();
    XXSLDWI(sum, b_operand, b_operand, 2);
    XVADDSP(sum, a_operand, sum);
    const auto result = fpr.GetScopedReg();
    if (sum1)
      XXPERMDI(result, c_operand, sum, 0);
    else
      XXPERMDI(result, sum, c_operand, 1);

    tiny = BranchIfTinySingles(result);
    resume = GetCodePtr();
    XXLMR(d, result);
    // Of the sum, as ps_sum0 sets it for ps0 and ps_sum1 for ps1.
    if (m_fprf && js.op->wantsFPRF)
      SetFPRFFromSingles(sum);
  }
  const u8* done = GetCodePtr();
  fpr.MarkSingles(inst.FD, true);
  // The far code calls the interpreter, which needs the scratch registers free.
  for (PairedOperand& operand : operands)
    operand.singles.Unlock();

  SwitchToFarCode();
  interpret.push_back(EmitTinySinglesFallback(tiny, false, resume));
  for (const FixupBranch& branch : interpret)
    SetJumpTarget(branch);
  InterpretFromFarCode(inst, done, d_was_single);
  SwitchToNearCode();
}

void JitPPC64::ps_sel(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  INTERPRET_IF(inst.Rc);

  // Each half is a >= -0.0 ? c : b, which is b if a is a NaN, as singles if all three are.
  if (fpr.HoldsSingles(inst.FA) && fpr.HoldsSingles(inst.FB) && fpr.HoldsSingles(inst.FC))
  {
    const VSR a = fpr.RSingle(inst.FA);
    const VSR b = fpr.RSingle(inst.FB);
    const VSR c = fpr.RSingle(inst.FC);
    const bool quiet = fpr.IsQuiet(inst.FB) && fpr.IsQuiet(inst.FC);
    const VSR d = fpr.BindForSingles(inst.FD);

    const auto mask = fpr.GetScopedReg();
    XXLXOR(mask, mask, mask);
    XVCMPGESP(mask, a, mask);
    XXSEL(d, b, c, mask);
    fpr.MarkSingles(inst.FD, quiet);
    return;
  }

  const VSR a = fpr.R(inst.FA);
  const VSR b = fpr.R(inst.FB);
  const VSR c = fpr.R(inst.FC);
  const VSR d = fpr.RW(inst.FD);

  const auto mask = fpr.GetScopedReg();
  XXLXOR(mask, mask, mask);
  XVCMPGEDP(mask, a, mask);
  XXSEL(d, b, c, mask);
}

void JitPPC64::ps_cmpXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(jo.fp_exceptions);

  // ps_cmpu0 and ps_cmpo0 compare ps0, ps_cmpu1 and ps_cmpo1 ps1.
  const bool half1 = inst.SUBOP10 == 64 || inst.SUBOP10 == 96;
  PPC64FPRCache::ScopedReg a_doubles, b_doubles;
  const VSR a = fpr.RForArith(inst.FA, a_doubles);
  const VSR b = fpr.RForArith(inst.FB, b_doubles);

  if (half1)
  {
    const auto a1 = fpr.GetScopedReg();
    const auto b1 = fpr.GetScopedReg();
    XXSWAPD(a1, a);
    XXSWAPD(b1, b);
    XSCMPUDP(CRF::CR1, a1, b1);
  }
  else
  {
    XSCMPUDP(CRF::CR1, a, b);
  }
  // With FPRF, these set FPSCR[FPCC].
  if (m_fprf)
    SetFPCCFromHostFloatCompare(CRF::CR1);
  SetCRFieldFromHostFloatCompare(inst.CRFD, CRF::CR1);
}

void JitPPC64::ps_res(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);
  EmitEstimate(inst, false, true);
}

void JitPPC64::ps_rsqrte(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);
  EmitEstimate(inst, true, true);
}
