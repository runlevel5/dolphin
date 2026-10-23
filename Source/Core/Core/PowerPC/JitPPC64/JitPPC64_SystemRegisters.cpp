// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"

using namespace PPC64Gen;

// SPRs other than XER, LR and CTR have side effects or are privileged, so only these three are
// accessed directly; the interpreter handles the others.

void JitPPC64::mfspr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  const u32 spr = ((inst.SPR & 0x1F) << 5) | ((inst.SPR >> 5) & 0x1F);
  switch (spr)
  {
  case SPR_LR:
  case SPR_CTR:
    LWZ(gpr.RW(inst.RD), PPC_REG, PPCSTATE_OFF_SPR(spr));
    break;

  case SPR_XER:
  {
    // XER = stringctrl | CA << 29 | (SO << 1 | OV) << 30
    const GPR d = gpr.RW(inst.RD);
    const auto tmp = gpr.GetScopedReg();
    LHZ(d, PPC_REG, PPCSTATE_OFF(xer_stringctrl));
    LBZ(tmp, PPC_REG, PPCSTATE_OFF(xer_ca));
    RLWIMI(d, tmp, XER_CA_SHIFT, 31 - XER_CA_SHIFT, 31 - XER_CA_SHIFT);
    LBZ(tmp, PPC_REG, PPCSTATE_OFF(xer_so_ov));
    RLWIMI(d, tmp, XER_OV_SHIFT, 0, 31 - XER_OV_SHIFT);
    break;
  }

  default:
    FallBackToInterpreter(inst);
    break;
  }
}

void JitPPC64::mtspr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  const u32 spr = ((inst.SPR & 0x1F) << 5) | ((inst.SPR >> 5) & 0x1F);
  switch (spr)
  {
  case SPR_LR:
  case SPR_CTR:
    STW(gpr.R(inst.RD), PPC_REG, PPCSTATE_OFF_SPR(spr));
    break;

  case SPR_XER:
  {
    // As PowerPCState::SetXER: stringctrl is the byte count and compare byte, then CA, SO and OV.
    const GPR s = gpr.R(inst.RD);
    const auto tmp = gpr.GetScopedReg();
    RLWINM(tmp, s, 0, 16, 31);  // Bits 0-6 are the byte count, 8-15 the compare byte
    ANDI_(tmp, tmp, 0xFF7F);
    STH(tmp, PPC_REG, PPCSTATE_OFF(xer_stringctrl));
    RLWINM(tmp, s, 32 - XER_CA_SHIFT, 31, 31);
    STB(tmp, PPC_REG, PPCSTATE_OFF(xer_ca));
    RLWINM(tmp, s, 32 - XER_OV_SHIFT, 30, 31);
    STB(tmp, PPC_REG, PPCSTATE_OFF(xer_so_ov));
    break;
  }

  default:
    FallBackToInterpreter(inst);
    break;
  }
}

void JitPPC64::mfcr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  const GPR d = gpr.RW(inst.RD);
  const auto field = gpr.GetScopedReg();
  for (u32 i = 0; i < 8; i++)
  {
    GetCRField(field, i);
    if (i == 0)
      SLWI(d, field, 28);
    else
      RLWIMI(d, field, 28 - 4 * i, 4 * i, 4 * i + 3);
  }
}

void JitPPC64::mtcrf(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  const GPR s = gpr.R(inst.RS);
  const auto field = gpr.GetScopedReg();
  for (u32 i = 0; i < 8; i++)
  {
    if ((inst.CRM & (0x80 >> i)) == 0)
      continue;

    RLWINM(field, s, (4 * i + 4) % 32, 28, 31);
    SetCRField(i, field);
  }
}

void JitPPC64::mcrf(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  if (inst.CRFS == inst.CRFD)
    return;

  const auto tmp = gpr.GetScopedReg();
  LD(tmp, PPC_REG, PPCSTATE_OFF_CR(inst.CRFS));
  STD(tmp, PPC_REG, PPCSTATE_OFF_CR(inst.CRFD));
}

void JitPPC64::crXXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  // Work on the four PowerPC bits of each field, as the interpreter does.
  const auto a = gpr.GetScopedReg();
  const auto b = gpr.GetScopedReg();
  const auto d = gpr.GetScopedReg();

  // Bit n of a field (LT = 0) is bit 3 - n of its four bits.
  GetCRField(a, inst.CRBA >> 2);
  RLWINM(a, a, (32 - (3 - (inst.CRBA & 3))) % 32, 31, 31);
  GetCRField(b, inst.CRBB >> 2);
  RLWINM(b, b, (32 - (3 - (inst.CRBB & 3))) % 32, 31, 31);

  switch (inst.SUBOP10)
  {
  case 257:  // crand
    AND(a, a, b);
    break;
  case 129:  // crandc
    ANDC(a, a, b);
    break;
  case 289:  // creqv
    EQV(a, a, b);
    break;
  case 225:  // crnand
    NAND(a, a, b);
    break;
  case 33:  // crnor
    NOR(a, a, b);
    break;
  case 449:  // cror
    OR(a, a, b);
    break;
  case 417:  // crorc
    ORC(a, a, b);
    break;
  case 193:  // crxor
    XOR(a, a, b);
    break;
  }

  const u32 bit = 3 - (inst.CRBD & 3);
  GetCRField(d, inst.CRBD >> 2);
  RLWIMI(d, a, bit, 31 - bit, 31 - bit);
  SetCRField(inst.CRBD >> 2, d);
}
