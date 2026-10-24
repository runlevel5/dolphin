// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Quantized loads and stores (psq_l and psq_st).
//
// The GQRs that a block uses but doesn't set are taken to be the values they had when it was
// compiled, as Jit64 does, and checked at the start of the block (see DoJit). With them known,
// each instruction is compiled for its type and scale, and its memory access gets fastmem.
// Otherwise, the instruction falls back to the interpreter.

#include "Core/PowerPC/JitPPC64/Jit.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"

using namespace PPC64Gen;

namespace
{
struct QuantizedType
{
  bool valid = false;
  bool is_float = false;
  bool is_signed = false;
  // The size of one half, in bits
  u32 size = 0;
};

QuantizedType DecodeQuantizedType(EQuantizeType type)
{
  switch (type)
  {
  case QUANTIZE_FLOAT:
    return {true, true, false, 32};
  case QUANTIZE_U8:
    return {true, false, false, 8};
  case QUANTIZE_U16:
    return {true, false, false, 16};
  case QUANTIZE_S8:
    return {true, false, true, 8};
  case QUANTIZE_S16:
    return {true, false, true, 16};
  default:
    return {};
  }
}

// Quantizing multiplies by 2^exponent, and dequantizing by 2^-exponent, as m_quantizeTable and
// m_dequantizeTable do: the 6-bit scale is signed.
s32 ScaleExponent(u32 scale)
{
  return scale < 32 ? static_cast<s32>(scale) : static_cast<s32>(scale) - 64;
}
}  // namespace

void JitPPC64::psq_lXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStorePairedOff);
  FALLBACK_IF(jo.memcheck);

  const bool indexed = inst.OPCD == 4;
  const bool update = inst.OPCD == 57 || (indexed && (inst.SUBOP6 & 32));
  const u32 i = indexed ? inst.Ix : inst.I;
  const bool single = indexed ? inst.Wx : inst.W;
  FALLBACK_IF(!js.constantGqrValid[i]);
  // The invalid form is left to the interpreter.
  FALLBACK_IF(update && inst.RA == 0);

  const UGQR gqr(js.constantGqr[i]);
  const QuantizedType type = DecodeQuantizedType(gqr.ld_type);
  FALLBACK_IF(!type.valid);
  const s32 exponent = ScaleExponent(gqr.ld_scale);

  const auto addr = gpr.GetScopedReg();
  const auto value = gpr.GetScopedReg();
  if (indexed)
  {
    if (inst.RA == 0)
      CLRLDI(addr, gpr.R(inst.RB), 32);
    else
      ADD(addr, gpr.R(inst.RA), gpr.R(inst.RB));
  }
  else
  {
    if (inst.RA == 0)
      MOVI2R(addr, static_cast<u32>(static_cast<s32>(inst.SIMM_12)));
    else
      ADDI(addr, gpr.R(inst.RA), inst.SIMM_12);
  }
  CLRLDI(addr, addr, 32);
  const GPR a = update ? gpr.RW(inst.RA, true) : GPR::R0;
  const VSR d = fpr.RW(inst.FD);

  EmitMemoryAccess({.size = single ? type.size : 2 * type.size}, value, addr);

  // The halves are in big-endian order in value: half 0 in the upper bits.
  const auto half0 = fpr.GetScopedReg();
  const auto half1 = fpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  if (type.is_float)
  {
    // As ConvertToDouble, from the single in word 0.
    if (single)
      SLDI(value, value, 32);
    else
      SLDI(tmp, value, 32);
    MTVSRD(half0, value);
    XSCVSPDPN(half0, half0);
    if (!single)
    {
      MTVSRD(half1, tmp);
      XSCVSPDPN(half1, half1);
    }
  }
  else
  {
    // float(T(value)) * m_dequantizeTable[scale]: exact, as is doing it in double precision.
    const auto convert = [&](VSR out, GPR integer) {
      if (type.is_signed)
        type.size == 8 ? EXTSB(integer, integer) : EXTSH(integer, integer);
      MTVSRD(out, integer);
      XSCVSXDDP(out, out);
      if (exponent != 0)
      {
        const auto factor = fpr.GetScopedReg();
        LI(integer, static_cast<s16>(1023 - exponent));
        SLDI(integer, integer, 52);
        MTVSRD(factor, integer);
        XSMULDP(out, out, factor);
      }
    };
    if (single)
    {
      convert(half0, value);
    }
    else
    {
      CLRLDI(tmp, value, 64 - type.size);
      SRDI(value, value, type.size);
      convert(half0, value);
      convert(half1, tmp);
    }
  }

  if (single)
  {
    // ps1 is 1.0.
    LIS(tmp, 0x3FF0);
    SLDI(tmp, tmp, 32);
    MTVSRD(half1, tmp);
  }
  XXPERMDI(d, half0, half1, 0);

  if (update)
    MR(a, addr);
}

void JitPPC64::QuantizeHalf(GPR out, VSR in, u32 type_size, bool is_signed, s32 exponent)
{
  // As ScaleAndClamp: SType(std::clamp(float(ps) * m_quantizeTable[scale], min, max)). Clamping
  // and truncating toward zero can go either way round, as the bounds are integers; a NaN, which
  // std::clamp passes through, converts to 0.
  const auto scaled = fpr.GetScopedReg();
  XSRSP(scaled, in);
  if (exponent != 0)
  {
    const auto factor = fpr.GetScopedReg();
    LI(out, static_cast<s16>(1023 + exponent));
    SLDI(out, out, 52);
    MTVSRD(factor, out);
    XSMULSP(scaled, scaled, factor);
  }
  XSCMPUDP(CRF::CR1, scaled, scaled);
  // Truncates, saturating to 32 bits.
  XSCVDPSXWS(scaled, scaled);
  MFVSRD(out, scaled);
  EXTSW(out, out);
  ISEL(out, GPR::R0, out, CRBit(CRF::CR1, CCFlags::SO));

  const auto bound = gpr.GetScopedReg();
  const s32 min = is_signed ? -(1 << (type_size - 1)) : 0;
  const u32 max = is_signed ? (1u << (type_size - 1)) - 1 : (1u << type_size) - 1;
  LI(bound, static_cast<s16>(min));
  CMPW(CRF::CR1, out, bound);
  ISEL(out, bound, out, CRBit(CRF::CR1, CCFlags::LT));
  MOVI2R(bound, max);
  CMPW(CRF::CR1, out, bound);
  ISEL(out, bound, out, CRBit(CRF::CR1, CCFlags::GT));
}

void JitPPC64::ConvertDoubleToSingleFTZ(GPR value)
{
  // As ConvertToSingleFTZ: like ConvertToSingle, but values too small for normal singles become
  // zeroes of the same sign.
  const auto exp = gpr.GetScopedReg();
  const auto high = gpr.GetScopedReg();
  RLDICL(exp, value, 12, 53);
  CMPLWI(CRF::CR1, exp, 896);
  SRDI(high, value, 32);
  SRDI(value, value, 29);
  RLWIMI(value, high, 0, 0, 1);
  RLWINM(high, high, 0, 0, 0);
  ISEL(value, value, high, CRBit(CRF::CR1, CCFlags::GT));
  CLRLDI(value, value, 32);
}

void JitPPC64::psq_stXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStorePairedOff);
  FALLBACK_IF(jo.memcheck);

  const bool indexed = inst.OPCD == 4;
  const bool update = inst.OPCD == 61 || (indexed && (inst.SUBOP6 & 32));
  const u32 i = indexed ? inst.Ix : inst.I;
  const bool single = indexed ? inst.Wx : inst.W;
  FALLBACK_IF(!js.constantGqrValid[i]);
  // The invalid form is left to the interpreter.
  FALLBACK_IF(update && inst.RA == 0);

  const UGQR gqr(js.constantGqr[i]);
  const QuantizedType type = DecodeQuantizedType(gqr.st_type);
  FALLBACK_IF(!type.valid);
  const s32 exponent = ScaleExponent(gqr.st_scale);

  const auto addr = gpr.GetScopedReg();
  const auto value = gpr.GetScopedReg();
  if (indexed)
  {
    if (inst.RA == 0)
      CLRLDI(addr, gpr.R(inst.RB), 32);
    else
      ADD(addr, gpr.R(inst.RA), gpr.R(inst.RB));
  }
  else
  {
    if (inst.RA == 0)
      MOVI2R(addr, static_cast<u32>(static_cast<s32>(inst.SIMM_12)));
    else
      ADDI(addr, gpr.R(inst.RA), inst.SIMM_12);
  }
  CLRLDI(addr, addr, 32);
  const VSR s = fpr.R(inst.FS);
  const GPR a = update ? gpr.RW(inst.RA, true) : GPR::R0;

  {
    const auto half1 = fpr.GetScopedReg();
    const auto value1 = gpr.GetScopedReg();
    if (!single)
      XXSWAPD(half1, s);

    if (type.is_float)
    {
      MFVSRD(value, s);
      ConvertDoubleToSingleFTZ(value);
      if (!single)
      {
        MFVSRD(value1, half1);
        ConvertDoubleToSingleFTZ(value1);
      }
    }
    else
    {
      QuantizeHalf(value, s, type.size, type.is_signed, exponent);
      if (!single)
        QuantizeHalf(value1, half1, type.size, type.is_signed, exponent);
    }

    // Half 0 goes in the upper bits, which are stored first.
    if (!single)
    {
      CLRLDI(value1, value1, 64 - type.size);
      SLDI(value, value, type.size);
      OR(value, value, value1);
    }
  }

  EmitMemoryAccess({.store = true, .size = single ? type.size : 2 * type.size}, value, addr);

  if (update)
    MR(a, addr);
}
