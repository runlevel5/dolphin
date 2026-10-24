// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Paired-single instructions. Each half is computed as the single precision floating-point
// instructions compute it, by the same code, with half 1 swapped into doubleword 0.

#include "Core/PowerPC/JitPPC64/Jit.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"

using namespace PPC64Gen;

void JitPPC64::ps_arith(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions || (jo.div_by_zero_exceptions && inst.SUBOP5 == 18));
  FALLBACK_IF(m_fprf && js.op->wantsFPRF);

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

  const bool use_c = op5 >= 25;
  const bool use_b = op5 != 25;
  const bool round_c = use_c && !js.op->fprIsSingle[inst.FC];

  const VSR a = fpr.R(inst.FA);
  const VSR b = use_b ? fpr.R(inst.FB) : VSR::VS0;
  const VSR c = use_c ? fpr.R(inst.FC) : VSR::VS0;
  const VSR d = fpr.RW(inst.FD);

  // Half 1 of the operands, in doubleword 0, and C's half 1 for half 0 of ps_muls1 and ps_madds1.
  const auto a1 = fpr.GetScopedReg();
  const auto b1 = fpr.GetScopedReg();
  const auto c1 = fpr.GetScopedReg();
  XXSWAPD(a1, a);
  if (use_b)
    XXSWAPD(b1, b);
  if (use_c && c_half != 0)
    XXSWAPD(c1, c);
  const VSR c_for_half_0 = c_half == 1 ? VSR(c1) : c;
  const VSR c_for_half_1 = c_half == 0 ? c : VSR(c1);

  const auto result0 = fpr.GetScopedReg();
  const auto result1 = fpr.GetScopedReg();
  EmitFloatArith(op5, true, round_c, result0, a, b, c_for_half_0);
  EmitFloatArith(op5, true, round_c, result1, a1, b1, c_for_half_1);
  XXPERMDI(d, result0, result1, 0);
}

void JitPPC64::ps_sumX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);
  FALLBACK_IF(m_fprf && js.op->wantsFPRF);

  // ps_sum0: a.ps0 + b.ps1, c.ps1; ps_sum1: c.ps0, a.ps0 + b.ps1. Both halves are rounded to single
  // precision.
  const bool sum1 = inst.SUBOP5 == 11;
  const VSR a = fpr.R(inst.FA);
  const VSR b = fpr.R(inst.FB);
  const VSR c = fpr.R(inst.FC);
  const VSR d = fpr.RW(inst.FD);

  const auto b1 = fpr.GetScopedReg();
  const auto sum = fpr.GetScopedReg();
  const auto other = fpr.GetScopedReg();
  XXSWAPD(b1, b);
  EmitFloatArith(21, true, false, sum, a, b1, VSR::VS0);
  if (sum1)
    XXLMR(other, c);
  else
    XXSWAPD(other, c);
  ForceSingle(other);

  if (sum1)
    XXPERMDI(d, other, sum, 0);
  else
    XXPERMDI(d, sum, other, 0);
}

void JitPPC64::ps_sel(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITPairedOff);
  FALLBACK_IF(inst.Rc);

  const VSR a = fpr.R(inst.FA);
  const VSR b = fpr.R(inst.FB);
  const VSR c = fpr.R(inst.FC);
  const VSR d = fpr.RW(inst.FD);

  // Each half is a >= -0.0 ? c : b, which is b if a is a NaN.
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
  // These set FPSCR[FPCC].
  FALLBACK_IF(m_fprf);

  // ps_cmpu0 and ps_cmpo0 compare ps0, ps_cmpu1 and ps_cmpo1 ps1.
  const bool half1 = inst.SUBOP10 == 64 || inst.SUBOP10 == 96;
  const VSR a = fpr.R(inst.FA);
  const VSR b = fpr.R(inst.FB);

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
  SetCRFieldFromHostFloatCompare(inst.CRFD, CRF::CR1);
}
