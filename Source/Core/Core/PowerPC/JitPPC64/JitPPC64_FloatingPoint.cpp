// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Floating-point instructions.
//
// The host is a PowerPC too, and its floating-point instructions give the results that the
// interpreter computes, bit for bit, in every rounding mode: the interpreter's arithmetic compiles
// to the same instructions, and the host picks and quiets NaN operands as the interpreter does.
// What's left to do here is what the interpreter does on top: rounding frC to 25 bits for single
// precision multiplies, the correction of ties in single precision fused multiply-adds, flushing
// denormals when FPSCR[NI] is set, and not negating NaNs.
//
// As in the other JITs, FPSCR is not updated, except by instructions that fall back to the
// interpreter: these are the record forms, and every instruction when FPRF or floating-point
// exceptions are enabled.

#include "Core/PowerPC/JitPPC64/Jit.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PowerPC.h"

using namespace PPC64Gen;

void JitPPC64::EnsureNILoaded()
{
  if (m_ni_loaded)
    return;

  const auto tmp = gpr.GetScopedReg();
  LWZ(tmp, PPC_REG, PPCSTATE_OFF(fpscr));
  ANDI_(tmp, tmp, 0x4);  // NI
  MCRF(CRF::CR4, CRF::CR0);
  m_ni_loaded = true;
}

// The flushes test the value where it is, by Power ISA 3.0's data class test and exponent
// extraction, and branch to far code only to flush: games that set NI keep it set, and a test in
// far code would take two branches and a round trip through a GPR for every result. Without those
// instructions, far code tests the bits in a GPR when NI is set.

void JitPPC64::FlushToSignedZero(VSR reg)
{
  const auto zero = fpr.GetScopedReg();
  XXLXOR(zero, zero, zero);
  XSCPSGNDP(reg, reg, zero);
}

void JitPPC64::ForceDouble(VSR reg)
{
  EnsureNILoaded();
  if (m_isa_3_0)
  {
    // As Common::FlushToZero: a denormal becomes a zero of the same sign, when NI is set.
    XSTSTDCDP(CRF::CR1, reg, 0x03);
    CRANDC(CRBit(CRF::CR1, CCFlags::EQ), CRBit(CRF::CR1, CCFlags::EQ),
           CRBit(CRF::CR4, CCFlags::EQ));
    const FixupBranch flush = BCFar(CCFlags::EQ, CRF::CR1);
    const u8* done = GetCodePtr();

    SwitchToFarCode();
    SetJumpTarget(flush);
    FlushToSignedZero(reg);
    B(done);
    SwitchToNearCode();
    return;
  }

  const FixupBranch ni = BCFar(CCFlags::NE, CRF::CR4);
  const u8* done = GetCodePtr();

  // As Common::FlushToZero: denormals become zeroes of the same sign.
  SwitchToFarCode();
  SetJumpTarget(ni);
  {
    const auto bits = gpr.GetScopedReg();
    const auto exp = gpr.GetScopedReg();
    MFVSRD(bits, reg);
    RLDICL(exp, bits, 12, 53, true);
    const FixupBranch not_denormal = BC(CCFlags::NE, CRF::CR0);
    RLDICR(bits, bits, 0, 0);
    MTVSRD(reg, bits);
    SetJumpTarget(not_denormal);
  }
  B(done);
  SwitchToNearCode();
}

void JitPPC64::ForceSingle(VSR reg)
{
  EnsureNILoaded();
  // With NI, a value below the smallest normal single is flushed to a zero of the same sign before
  // it's rounded. Rounding can't make any other value denormal.
  if (m_isa_3_0)
  {
    // Below it is an exponent below the smallest normal single's: zeroes and denormals too, which
    // the flush leaves as they are, and never NaNs or infinities.
    {
      const auto exp = gpr.GetScopedReg();
      XSXEXPDP(exp, reg);
      CMPLDI(CRF::CR1, exp, 0x381);
    }
    CRANDC(CRBit(CRF::CR1, CCFlags::LT), CRBit(CRF::CR1, CCFlags::LT),
           CRBit(CRF::CR4, CCFlags::EQ));
    const FixupBranch flush = BCFar(CCFlags::LT, CRF::CR1);
    const u8* round = GetCodePtr();
    XSRSP(reg, reg);

    SwitchToFarCode();
    SetJumpTarget(flush);
    FlushToSignedZero(reg);
    // Rounding leaves a zero as it is.
    B(round);
    SwitchToNearCode();
    return;
  }

  const FixupBranch ni = BCFar(CCFlags::NE, CRF::CR4);
  const u8* round = GetCodePtr();
  XSRSP(reg, reg);
  const u8* done = GetCodePtr();

  SwitchToFarCode();
  SetJumpTarget(ni);
  {
    const auto bits = gpr.GetScopedReg();
    const auto tmp = gpr.GetScopedReg();
    const auto smallest_normal = gpr.GetScopedReg();
    MFVSRD(bits, reg);
    CLRLDI(tmp, bits, 1);
    LIS(smallest_normal, 0x3810);
    SLDI(smallest_normal, smallest_normal, 32);
    CMPLD(CRF::CR1, tmp, smallest_normal);
    const FixupBranch normal = BC(CCFlags::GE, CRF::CR1);
    RLDICR(bits, bits, 0, 0);
    MTVSRD(reg, bits);
    B(done);
    SetJumpTarget(normal);
  }
  B(round);
  SwitchToNearCode();
}

void JitPPC64::ConvertSinglesToDoubles(VSR out, VSR in, bool quiet)
{
  if (quiet)
  {
    XVCVSPDP(out, in);
    return;
  }
  // xscvspdpn converts word 0 without quieting.
  XSCVSPDPN(VSR::VS2, in);
  XXSLDWI(VSR::VS3, in, in, 2);
  XSCVSPDPN(VSR::VS3, VSR::VS3);
  XXPERMDI(out, VSR::VS2, VSR::VS3, 0);
}

void JitPPC64::ConvertDoublesToSingles(VSR out, VSR in)
{
  // POWER9 and Power10 copy words 0 and 2 into words 1 and 3, which Power ISA 2.07 leaves
  // undefined.
  XVCVDPSP(out, in);
  if (!m_isa_3_0)
    VMRGEW(out, out, out);
}

void JitPPC64::InterpretFromFarCode(UGeckoInstruction inst, const u8* done)
{
  InterpretFromFarCode(inst, done, fpr.HoldsSingles(inst.FD));
}

void JitPPC64::InterpretFromFarCode(UGeckoInstruction inst, const u8* done, bool fd_was_single)
{
  // The interpreter may set FPSCR's flags, and leave FD as it was. Storing all of FA, FB, FC and
  // FD stores whatever registers those fields name, which is harmless for fields an instruction
  // doesn't use, and for an FD that it only writes, which may hold anything if it was just bound.
  for (const u32 reg : {u32(inst.FA), u32(inst.FB), u32(inst.FC)})
  {
    if (reg != inst.FD)
      fpr.EmitStoreValue(reg);
  }
  fpr.EmitStoreValue(inst.FD, fd_was_single);
  {
    const BitSet32 saved = SaveScratchRegs();
    ABI_CallFunction(Interpreter::GetInterpreterOp(inst), &m_system.GetInterpreter(), inst.hex);
    RestoreScratchRegs(saved);
  }
  fpr.EmitLoadValue(inst.FD);
  if (ShouldHandleFPExceptionForInstruction(js.op))
    WriteConditionalExceptionExit(EXCEPTION_PROGRAM);
  B(done);
}

void JitPPC64::Force25Bit(VSR out, VSR in)
{
  // Force25Bit computes (x & 0xfffffffff8000000) + (x & 0x8000000): the mask keeps bit 27, which
  // is also the rounding bit, so this is (x + 0x8000000) & 0xfffffffff0000000, which rounds to
  // nearest, ties away from zero, at bit 28.
  const auto bits = gpr.GetScopedReg();
  const auto exp = gpr.GetScopedReg();
  MFVSRD(bits, in);
  RLDICL(exp, bits, 12, 53, true);
  const FixupBranch denormal_or_zero = BCFar(CCFlags::EQ, CRF::CR0);
  ADDIS(bits, bits, 0x800);
  RLDICR(bits, bits, 0, 35);
  const u8* done = GetCodePtr();
  MTVSRD(out, bits);

  // Denormals are normalized first, which moves the mask and the rounding bit down by the number of
  // leading zeroes of the fraction. For zeroes, this leaves the value as it is.
  SwitchToFarCode();
  SetJumpTarget(denormal_or_zero);
  {
    const auto keep_mask = gpr.GetScopedReg();
    const auto round = gpr.GetScopedReg();
    CLRLDI(exp, bits, 12);
    CNTLZD(exp, exp);
    ADDI(exp, exp, -11);
    LI(keep_mask, -1);
    SLDI(keep_mask, keep_mask, 28);
    SRAD(keep_mask, keep_mask, exp);
    LIS(round, 0x800);
    SRD(round, round, exp);
    ADD(bits, bits, round);
    AND(bits, bits, keep_mask);
  }
  B(done);
  SwitchToNearCode();
}

void JitPPC64::SetFPRF(VSR value, bool single)
{
  // As PowerPCState::UpdateFPRFDouble or UpdateFPRFSingle. The JITs don't set FPSCR's other status
  // bits.
  // With Power ISA 3.0, the routine tests the value in VS0, which isn't allocated.
  if (m_isa_3_0)
    XXLMR(VSR::VS0, value);
  else
    MFVSRD(GPR::R3, value);
  BL(Routine(single ? m_fprf_single_routine : m_fprf_double_routine));
}

void JitPPC64::SetFPCCFromHostFloatCompare(CRF host_field)
{
  // FPCC, bits 12-15 of FPSCR, gets the compare's FL, FG, FE and FU, as the guest's CR field does.
  const auto field = gpr.GetScopedReg();
  const auto fpscr = gpr.GetScopedReg();
  MFOCRF(field, host_field);
  RLWINM(field, field, (4 * DecodeReg(host_field) + 4) % 32, 28, 31);
  LWZ(fpscr, PPC_REG, PPCSTATE_OFF(fpscr));
  RLWIMI(fpscr, field, 12, 16, 19);
  STW(fpscr, PPC_REG, PPCSTATE_OFF(fpscr));
}

void JitPPC64::SetCRFieldFromHostFloatCompare(u32 field, CRF host_field)
{
  // The host field holds FL, FG, FE and FU, as the guest's does.
  const auto tmp = gpr.GetScopedReg();
  MFOCRF(tmp, host_field);
  RLWINM(tmp, tmp, (4 * DecodeReg(host_field) + 4) % 32, 28, 31);
  SetCRField(field, tmp);
}

void JitPPC64::EmitFloatArith(u32 op5, bool single, bool round_c, VSR result, VSR a, VSR b, VSR c)
{
  const bool use_c = op5 >= 25;  // fmul and the fused multiply-adds
  const bool use_b = op5 != 25;  // fmul doesn't use B
  const bool fma = use_b && use_c;
  const bool negate = op5 == 30 || op5 == 31;
  const bool subtract_b = op5 == 28 || op5 == 30;

  PPC64FPRCache::ScopedReg rounded_c;
  VSR c_operand = c;
  if (round_c)
  {
    rounded_c = fpr.GetScopedReg();
    Force25Bit(rounded_c, c);
    c_operand = rounded_c;
  }

  switch (op5)
  {
  case 18:
    XSDIVDP(result, a, b);
    break;
  case 20:
    XSSUBDP(result, a, b);
    break;
  case 21:
    XSADDDP(result, a, b);
    break;
  case 25:
    XSMULDP(result, a, c_operand);
    break;
  default:
    // xsmaddadp and friends compute result = a * c + result. For double precision, the host's
    // negated forms round before negating, and don't negate NaNs, as the guest's do; for single
    // precision, the negation has to come after rounding to single precision.
    XXLMR(result, b);
    if (single || !negate)
      subtract_b ? XSMSUBADP(result, a, c_operand) : XSMADDADP(result, a, c_operand);
    else
      subtract_b ? XSNMSUBADP(result, a, c_operand) : XSNMADDADP(result, a, c_operand);
    break;
  }

  if (single && fma)
  {
    // With a rounded C, a NaN C gives a NaN result made from C before it was rounded.
    if (round_c)
    {
      XSCMPUDP(CRF::CR1, result, result);
      const FixupBranch nan = BCFar(CCFlags::SO, CRF::CR1);
      const u8* done = GetCodePtr();

      SwitchToFarCode();
      SetJumpTarget(nan);
      XSCMPUDP(CRF::CR1, a, b);
      const FixupBranch a_or_b_nan = BC(CCFlags::SO, CRF::CR1);
      XSCMPUDP(CRF::CR1, c, c);
      const FixupBranch c_not_nan = BC(CCFlags::NS, CRF::CR1);
      // Quiet C.
      XSADDDP(result, c, c);
      SetJumpTarget(a_or_b_nan);
      SetJumpTarget(c_not_nan);
      B(done);
      SwitchToNearCode();
    }

    if (m_accurate_fmadds)
    {
      // Correct ties, as NI_madd_msub: when the low 29 bits of the double precision result are a
      // tie for rounding to single precision, nudge it by one in the direction of the error that
      // rounding the fused multiply-add to double precision made.
      const auto bits = gpr.GetScopedReg();
      MFVSRD(bits, result);
      CLRLDI(bits, bits, 35);
      XORIS(bits, bits, 0x1000);
      CMPLDI(CRF::CR1, bits, 0);
      const FixupBranch tie = BCFar(CCFlags::EQ, CRF::CR1);
      const u8* done = GetCodePtr();

      SwitchToFarCode();
      SetJumpTarget(tie);
      {
        const auto b_sign = fpr.GetScopedReg();
        const auto a_prime = fpr.GetScopedReg();
        const auto b_prime = fpr.GetScopedReg();
        const auto delta_a = fpr.GetScopedReg();
        if (subtract_b)
          XSNEGDP(b_sign, b);
        else
          XXLMR(b_sign, b);
        XSSUBDP(a_prime, b_sign, result);
        XSADDDP(b_prime, result, a_prime);
        XXLMR(delta_a, a_prime);
        XSMADDADP(delta_a, a, c_operand);
        XSSUBDP(b_prime, b_sign, b_prime);   // delta_b
        XSADDDP(a_prime, delta_a, b_prime);  // error

        // if (error != 0) bits += (error > 0) == (result > 0) ? 1 : -1;
        XXLXOR(b_sign, b_sign, b_sign);
        XSCMPUDP(CRF::CR1, a_prime, b_sign);
        const FixupBranch no_error = BC(CCFlags::EQ, CRF::CR1);
        XSCMPUDP(CRF::CR0, result, b_sign);
        CREQV(CRBit(CRF::CR1, CCFlags::LT), CRBit(CRF::CR1, CCFlags::GT),
              CRBit(CRF::CR0, CCFlags::GT));
        MFVSRD(bits, result);
        ADDI(bits, bits, 1);
        const FixupBranch up = BC(CCFlags::LT, CRF::CR1);
        ADDI(bits, bits, -2);
        SetJumpTarget(up);
        MTVSRD(result, bits);
        SetJumpTarget(no_error);
      }
      B(done);
      SwitchToNearCode();
    }
  }

  if (single)
  {
    ForceSingle(result);
    if (negate)
    {
      XSCMPUDP(CRF::CR1, result, result);
      const FixupBranch nan = BC(CCFlags::SO, CRF::CR1);
      XSNEGDP(result, result);
      SetJumpTarget(nan);
    }
  }
  else
  {
    ForceDouble(result);
  }
}

bool JitPPC64::IsSingle(u32 reg) const
{
  // As the analyzer says, but for the result of fres or ps_res, which keeps a NaN input as it is,
  // as a double, which the interpreter then rounds as a multiplicand. Jit64 doesn't.
  if (!js.op->fprIsSingle[reg])
    return false;
  for (const PPCAnalyst::CodeOp* op = js.op; op != m_code_buffer.data();)
  {
    --op;
    if (op->fregOut == static_cast<s8>(reg))
      return !((op->inst.OPCD == 59 || op->inst.OPCD == 4) && op->inst.SUBOP5 == 24);
  }
  return true;
}

void JitPPC64::fp_arith(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || (jo.div_by_zero_exceptions && inst.SUBOP5 == 18));

  const u32 op5 = inst.SUBOP5;
  const bool single = inst.OPCD == 59;
  const bool use_c = op5 >= 25;  // fmul and the fused multiply-adds
  const bool use_b = op5 != 25;  // fmul doesn't use B
  const bool round_c = single && use_c && !fpr.HoldsSingles(inst.FC) && !IsSingle(inst.FC);

  PPC64FPRCache::ScopedReg a_doubles, b_doubles, c_doubles;
  const VSR a = fpr.RForArith(inst.FA, a_doubles);
  const VSR b = use_b ? fpr.RForArith(inst.FB, b_doubles) : VSR::VS0;
  const VSR c = use_c ? fpr.RForArith(inst.FC, c_doubles) : VSR::VS0;
  // Double precision instructions only write ps0.
  const VSR d = fpr.RW(inst.FD, !single);

  const auto result = fpr.GetScopedReg();
  EmitFloatArith(op5, single, round_c, result, a, b, c);
  if (single)
    XXPERMDI(d, result, result, 0);
  else
    XXPERMDI(d, result, d, 1);
  if (m_fprf && js.op->wantsFPRF)
    SetFPRF(result, single);
}

void JitPPC64::fp_logic(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);

  // These only change the sign bit, of ps0, or of both halves for the paired single forms, which
  // keep singles as singles. The others do too, if FD holds singles.
  const bool paired = inst.OPCD == 4;
  if (fpr.HoldsSingles(inst.FB) && (paired || fpr.HoldsSingles(inst.FD)))
  {
    const VSR b = fpr.RSingle(inst.FB);
    const bool quiet = fpr.IsQuiet(inst.FB) && (paired || fpr.IsQuiet(inst.FD));
    const VSR d = fpr.BindForSingles(inst.FD);
    // The scalar forms merge ps0 into FD.
    PPC64FPRCache::ScopedReg half0;
    if (!paired)
      half0 = fpr.GetScopedReg();
    const VSR result = paired ? d : VSR(half0);
    switch (inst.SUBOP10)
    {
    case 40:  // fneg, ps_neg
      XVNEGSP(result, b);
      break;
    case 72:  // fmr, ps_mr
      if (result != b)
        XXLMR(result, b);
      break;
    case 136:  // fnabs, ps_nabs
      XVNABSSP(result, b);
      break;
    case 264:  // fabs, ps_abs
      XVABSSP(result, b);
      break;
    default:
      ASSERT_MSG(DYNA_REC, false, "Unexpected instruction {:#010x}", inst.hex);
      break;
    }
    if (!paired)
      XXPERMDI(d, result, d, 1);
    fpr.MarkSingles(inst.FD, quiet);
    return;
  }

  // Otherwise, as doubles: ps0 of a single is converted, exactly, into a scratch register.
  PPC64FPRCache::ScopedReg b_ps0;
  VSR b;
  if (!paired && fpr.HoldsSingles(inst.FB))
  {
    b_ps0 = fpr.GetScopedReg();
    XSCVSPDPN(b_ps0, fpr.RSingle(inst.FB));
    b = b_ps0;
  }
  else
  {
    b = fpr.R(inst.FB);
  }
  const VSR d = fpr.RW(inst.FD, !paired);

  if (paired)
  {
    switch (inst.SUBOP10)
    {
    case 40:  // ps_neg
      XVNEGDP(d, b);
      break;
    case 72:  // ps_mr
      if (d != b)
        XXLMR(d, b);
      break;
    case 136:  // ps_nabs
      XVNABSDP(d, b);
      break;
    case 264:  // ps_abs
      XVABSDP(d, b);
      break;
    default:
      ASSERT_MSG(DYNA_REC, false, "Unexpected instruction {:#010x}", inst.hex);
      break;
    }
    return;
  }

  const auto result = fpr.GetScopedReg();
  switch (inst.SUBOP10)
  {
  case 40:  // fneg
    XSNEGDP(result, b);
    break;
  case 72:  // fmr
    XXLMR(result, b);
    break;
  case 136:  // fnabs
    XSNABSDP(result, b);
    break;
  case 264:  // fabs
    XSABSDP(result, b);
    break;
  default:
    ASSERT_MSG(DYNA_REC, false, "Unexpected instruction {:#010x}", inst.hex);
    break;
  }
  XXPERMDI(d, result, d, 1);
}

void JitPPC64::fselx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);

  const VSR a = fpr.R(inst.FA);
  const VSR b = fpr.R(inst.FB);
  const VSR c = fpr.R(inst.FC);
  const VSR d = fpr.RW(inst.FD, true);

  // d.ps0 = a >= -0.0 ? c : b, which is b if a is a NaN.
  const auto mask = fpr.GetScopedReg();
  const auto result = fpr.GetScopedReg();
  XXLXOR(mask, mask, mask);
  XVCMPGEDP(mask, a, mask);
  XXSEL(result, b, c, mask);
  XXPERMDI(d, result, d, 1);
}

void JitPPC64::frspx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  PPC64FPRCache::ScopedReg b_doubles;
  const VSR b = fpr.RForArith(inst.FB, b_doubles);
  const VSR d = fpr.RW(inst.FD);

  const auto result = fpr.GetScopedReg();
  XXLMR(result, b);
  ForceSingle(result);
  XXPERMDI(d, result, result, 0);
  if (m_fprf && js.op->wantsFPRF)
    SetFPRF(result, true);
}

void JitPPC64::fctiwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const bool round_toward_zero = inst.SUBOP10 == 15;
  PPC64FPRCache::ScopedReg b_doubles;
  const VSR b = fpr.RForArith(inst.FB, b_doubles);
  const VSR d = fpr.RW(inst.FD, true);

  // The host's fctiw and fctiwz round and saturate as the guest's do, with 0x80000000 for NaNs,
  // but they only work on F0-F31, so use F0, which isn't allocated, and leave the upper half of
  // the result undefined.
  XXLMR(VSR::VS0, b);
  if (round_toward_zero)
    FCTIWZ(FPR::F0, FPR::F0);
  else
    FCTIW(FPR::F0, FPR::F0);

  // As ConvertToInteger: 0xfff8000000000000 | value, with bit 32 set for a zero from a negative
  // operand.
  const auto value = gpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  MFVSRD(value, VSR::VS0);
  CLRLDI(value, value, 32);
  MFVSRD(tmp, b);
  SRDI(tmp, tmp, 63);
  {
    const auto is_zero = gpr.GetScopedReg();
    CNTLZW(is_zero, value);
    SRWI(is_zero, is_zero, 5);
    AND(tmp, tmp, is_zero);
  }
  SLDI(tmp, tmp, 32);
  OR(value, value, tmp);
  MOVI2R(tmp, 0xFFF8000000000000);
  OR(value, value, tmp);
  MTVSRD(VSR::VS0, value);
  XXPERMDI(d, VSR::VS0, d, 1);
}

void JitPPC64::fcmpX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  FALLBACK_IF(jo.fp_exceptions);

  PPC64FPRCache::ScopedReg a_doubles, b_doubles;
  const VSR a = fpr.RForArith(inst.FA, a_doubles);
  const VSR b = fpr.RForArith(inst.FB, b_doubles);

  XSCMPUDP(CRF::CR1, a, b);
  // With FPRF, fcmpu and fcmpo set FPSCR[FPCC].
  if (m_fprf)
    SetFPCCFromHostFloatCompare(CRF::CR1);
  SetCRFieldFromHostFloatCompare(inst.CRFD, CRF::CR1);
}

void JitPPC64::ps_mergeXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  INTERPRET_IF(inst.Rc);

  // ps_merge00: a.ps0, b.ps0; 01: a.ps0, b.ps1; 10: a.ps1, b.ps0; 11: a.ps1, b.ps1. xxpermdi
  // picks the doubleword of A with bit 1 of its DM field, and of B with bit 0, which holds a half
  // as a double, or as a single twice.
  const u32 dm = (inst.SUBOP10 - 528) / 32;
  ASSERT_MSG(DYNA_REC, dm < 4, "Unexpected instruction {:#010x}", inst.hex);
  if (fpr.HoldsSingles(inst.FA) && fpr.HoldsSingles(inst.FB))
  {
    const VSR a = fpr.RSingle(inst.FA);
    const VSR b = fpr.RSingle(inst.FB);
    const bool quiet = fpr.IsQuiet(inst.FA) && fpr.IsQuiet(inst.FB);
    XXPERMDI(fpr.BindForSingles(inst.FD), a, b, dm);
    fpr.MarkSingles(inst.FD, quiet);
    return;
  }

  const VSR a = fpr.R(inst.FA);
  const VSR b = fpr.R(inst.FB);
  XXPERMDI(fpr.RW(inst.FD), a, b, dm);
}

void JitPPC64::EmitEstimate(UGeckoInstruction inst, bool rsqrte, bool paired)
{
  // The routines estimate each half. For inputs they leave to the interpreter, call the instruction
  // in the interpreter from far code.
  const VSR b = fpr.R(inst.FB);
  const VSR d = fpr.RW(inst.FD, true);
  // ps_rsqrte rounds to single precision, depending on FPSCR[NI], on one path only.
  if (paired && rsqrte)
    EnsureNILoaded();

  const u8* routine = rsqrte ? m_frsqrte_routine : m_fres_routine;
  const auto half0 = gpr.GetScopedReg();
  const auto half1 = gpr.GetScopedReg();
  // With Power ISA 3.0, the routines test the input in VS0 too.
  MFVSRD(GPR::R3, b);
  if (m_isa_3_0)
    XXLMR(VSR::VS0, b);
  BL(Routine(routine));
  MR(half0, GPR::R3);
  MR(half1, GPR::R4);
  if (paired)
  {
    if (m_isa_3_0)
    {
      MFVSRLD(GPR::R3, b);
      XXSWAPD(VSR::VS0, b);
    }
    else
    {
      const auto b1 = fpr.GetScopedReg();
      XXSWAPD(b1, b);
      MFVSRD(GPR::R3, b1);
    }
    BL(Routine(routine));
    OR(GPR::R4, GPR::R4, half1);
    MR(half1, GPR::R3);
  }
  else
  {
    MR(GPR::R4, half1);
  }
  CMPDI(CRF::CR1, GPR::R4, 0);
  const FixupBranch interpret = BCFar(CCFlags::NE, CRF::CR1);

  if (m_isa_3_0 && !rsqrte)
  {
    // ps_res and fres write both halves, which mtvsrdd sets from GPRs.
    MTVSRDD(d, half0, paired ? half1 : half0);
    if (m_fprf && js.op->wantsFPRF)
      SetFPRF(d, true);
  }
  else
  {
    const auto result0 = fpr.GetScopedReg();
    const auto result1 = fpr.GetScopedReg();
    MTVSRD(result0, half0);
    if (paired)
    {
      MTVSRD(result1, half1);
      if (rsqrte)
      {
        // ps_rsqrte rounds its estimates to single precision; ps_res's are single already.
        ForceSingle(result0);
        ForceSingle(result1);
      }
      XXPERMDI(d, result0, result1, 0);
    }
    else if (rsqrte)
    {
      // frsqrte writes ps0.
      XXPERMDI(d, result0, d, 1);
    }
    else
    {
      // fres writes both halves.
      XXPERMDI(d, result0, result0, 0);
    }
    if (m_fprf && js.op->wantsFPRF)
      SetFPRF(result0, !(rsqrte && !paired));
  }
  const u8* done = GetCodePtr();

  SwitchToFarCode();
  SetJumpTarget(interpret);
  InterpretFromFarCode(inst, done);
  SwitchToNearCode();
}

void JitPPC64::fresx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);
  EmitEstimate(inst, false, false);
}

void JitPPC64::frsqrtex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITFloatingPointOff);
  INTERPRET_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);
  EmitEstimate(inst, true, false);
}
