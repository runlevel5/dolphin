// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Integer loads and stores.
//
// With fastmem, an access is a single byte-reversed load or store (the guest is big-endian, the
// host little-endian) relative to MEM_REG, the base of the fastmem arena for the current MSR[DR].
// Addresses that aren't mapped in the arena, such as MMIO, fault. The fault handler then patches
// the access into a branch to its slow path in far code, which calls the MMU and branches back.
// Without fastmem, accesses always take the slow path, inline.

#include "Core/PowerPC/JitPPC64/Jit.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/PPC64Emitter.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace PPC64Gen;

void JitPPC64::ComputeAddress(GPR addr, UGeckoInstruction inst, bool indexed)
{
  // EA = (rA|0) + d or (rA|0) + rB, zero-extended to 64 bits for indexing the arena.
  if (indexed)
  {
    if (inst.RA == 0)
    {
      CLRLDI(addr, gpr.R(inst.RB), 32);
      return;
    }
    ADD(addr, gpr.R(inst.RA), gpr.R(inst.RB));
  }
  else
  {
    if (inst.RA == 0)
    {
      MOVI2R(addr, static_cast<u32>(static_cast<s32>(inst.SIMM_16)));
      return;
    }
    ADDI(addr, gpr.R(inst.RA), inst.SIMM_16);
  }
  CLRLDI(addr, addr, 32);
}

void JitPPC64::EmitMemoryAccess(MemAccess access, GPR value, GPR addr)
{
  if (!jo.fastmem || m_accurate_cpu_cache_enabled)
  {
    EmitSlowAccess(access, value, addr);
    return;
  }

  const u8* fault_pc = GetCodePtr();
  EmitFastAccess(access, value, addr);
  const u8* fast_end = GetCodePtr();

  SwitchToFarCode();
  m_fault_to_handler[fault_pc] = GetCodePtr();
  EmitSlowAccess(access, value, addr);
  B(fast_end);
  SwitchToNearCode();
}

void JitPPC64::EmitFastAccess(MemAccess access, GPR value, GPR addr)
{
  // The first instruction is the one that faults, and is patched.
  if (access.store)
  {
    switch (access.size)
    {
    case 64:
      STDBRX(value, MEM_REG, addr);
      break;
    case 32:
      access.reverse ? STWX(value, MEM_REG, addr) : STWBRX(value, MEM_REG, addr);
      break;
    case 16:
      access.reverse ? STHX(value, MEM_REG, addr) : STHBRX(value, MEM_REG, addr);
      break;
    default:
      STBX(value, MEM_REG, addr);
      break;
    }
    return;
  }

  switch (access.size)
  {
  case 64:
    LDBRX(value, MEM_REG, addr);
    break;
  case 32:
    access.reverse ? LWZX(value, MEM_REG, addr) : LWBRX(value, MEM_REG, addr);
    break;
  case 16:
    access.reverse ? LHZX(value, MEM_REG, addr) : LHBRX(value, MEM_REG, addr);
    if (access.sign_extend)
      EXTSH(value, value);
    break;
  default:
    LBZX(value, MEM_REG, addr);
    break;
  }
}

void JitPPC64::EmitSlowAccess(MemAccess access, GPR value, GPR addr)
{
  // Scratch registers are volatile, so save those in use around the call; guest registers are
  // non-volatile. A load overwrites its destination anyway. No scratch VSX registers may be in use.
  ASSERT(!fpr.AnyScratchRegsInUse());
  BitSet32 to_save = gpr.GetScratchRegsInUse();
  if (!access.store)
    to_save[DecodeReg(value)] = false;
  for (const int i : to_save)
    STD(static_cast<GPR>(i), GPR::SP, ScratchSaveOffset(static_cast<GPR>(i)));

  // The PC is used by memory checks and in logs.
  MOVI2R(GPR::R3, js.compilerPC);
  STW(GPR::R3, PPC_REG, PPCSTATE_OFF(pc));

  // Pass the address and value in registers that aren't argument registers, zero-extended as the
  // ABI requires.
  CLRLDI(GPR::R12, addr, 32);
  if (access.store)
  {
    if (access.size == 64)
      MR(GPR::R0, value);
    else
      CLRLDI(GPR::R0, value, 32);
    switch (access.size)
    {
    case 64:
      ABI_CallFunction(&PowerPC::WriteFromJit<u64>, &m_mmu, GPR::R0, GPR::R12);
      break;
    case 32:
      ABI_CallFunction(access.reverse ? &PowerPC::WriteU32SwapFromJit : &PowerPC::WriteFromJit<u32>,
                       &m_mmu, GPR::R0, GPR::R12);
      break;
    case 16:
      ABI_CallFunction(access.reverse ? &PowerPC::WriteU16SwapFromJit : &PowerPC::WriteFromJit<u16>,
                       &m_mmu, GPR::R0, GPR::R12);
      break;
    default:
      ABI_CallFunction(&PowerPC::WriteFromJit<u8>, &m_mmu, GPR::R0, GPR::R12);
      break;
    }
  }
  else
  {
    switch (access.size)
    {
    case 64:
      ABI_CallFunction(&PowerPC::ReadFromJit<u64>, &m_mmu, GPR::R12);
      MR(value, GPR::R3);
      break;
    case 32:
      ABI_CallFunction(&PowerPC::ReadFromJit<u32>, &m_mmu, GPR::R12);
      if (access.reverse)
      {
        ROTLWI(value, GPR::R3, 8);
        RLWIMI(value, GPR::R3, 24, 0, 7);
        RLWIMI(value, GPR::R3, 24, 16, 23);
      }
      else
      {
        MR(value, GPR::R3);
      }
      break;
    case 16:
      ABI_CallFunction(&PowerPC::ReadFromJit<u16>, &m_mmu, GPR::R12);
      if (access.reverse)
      {
        RLWINM(value, GPR::R3, 24, 24, 31);
        RLWIMI(value, GPR::R3, 8, 16, 23);
      }
      else if (access.sign_extend)
      {
        EXTSH(value, GPR::R3);
      }
      else
      {
        MR(value, GPR::R3);
      }
      break;
    default:
      ABI_CallFunction(&PowerPC::ReadFromJit<u8>, &m_mmu, GPR::R12);
      MR(value, GPR::R3);
      break;
    }
  }

  for (const int i : to_save)
    LD(static_cast<GPR>(i), GPR::SP, ScratchSaveOffset(static_cast<GPR>(i)));
}

bool JitPPC64::HandleFastmemFault(SContext* ctx)
{
  const u8* pc = reinterpret_cast<const u8*>(ctx->CTX_PC);
  const auto handler = m_fault_to_handler.find(pc);
  if (handler == m_fault_to_handler.end())
    return false;

  {
    const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
    PPC64XEmitter emitter(const_cast<u8*>(pc), const_cast<u8*>(pc) + sizeof(u32));
    emitter.B(handler->second);
    emitter.FlushIcache();
  }
  m_fault_to_handler.erase(handler);

  // Run the slow path now, from the patched branch.
  return true;
}

void JitPPC64::lXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(jo.memcheck);

  MemAccess access{};
  bool indexed = false;
  bool update = false;
  switch (inst.OPCD)
  {
  case 31:
    indexed = true;
    switch (inst.SUBOP10)
    {
    case 55:  // lwzux
      update = true;
      [[fallthrough]];
    case 23:  // lwzx
      access.size = 32;
      break;
    case 119:  // lbzux
      update = true;
      [[fallthrough]];
    case 87:  // lbzx
      access.size = 8;
      break;
    case 311:  // lhzux
      update = true;
      [[fallthrough]];
    case 279:  // lhzx
      access.size = 16;
      break;
    case 375:  // lhaux
      update = true;
      [[fallthrough]];
    case 343:  // lhax
      access.size = 16;
      access.sign_extend = true;
      break;
    case 534:  // lwbrx
      access.size = 32;
      access.reverse = true;
      break;
    case 790:  // lhbrx
      access.size = 16;
      access.reverse = true;
      break;
    default:
      ASSERT_MSG(DYNA_REC, false, "Unexpected load {:#010x}", inst.hex);
      break;
    }
    break;
  case 33:  // lwzu
    update = true;
    [[fallthrough]];
  case 32:  // lwz
    access.size = 32;
    break;
  case 35:  // lbzu
    update = true;
    [[fallthrough]];
  case 34:  // lbz
    access.size = 8;
    break;
  case 41:  // lhzu
    update = true;
    [[fallthrough]];
  case 40:  // lhz
    access.size = 16;
    break;
  case 43:  // lhau
    update = true;
    [[fallthrough]];
  case 42:  // lha
    access.size = 16;
    access.sign_extend = true;
    break;
  default:
    ASSERT_MSG(DYNA_REC, false, "Unexpected load {:#010x}", inst.hex);
    break;
  }

  // The invalid forms are left to the interpreter.
  FALLBACK_IF(update && (inst.RA == 0 || inst.RA == inst.RD));

  const auto addr = gpr.GetScopedReg();
  ComputeAddress(addr, inst, indexed);
  const GPR a = update ? gpr.RW(inst.RA, true) : GPR::R0;
  const GPR d = gpr.RW(inst.RD);

  EmitMemoryAccess(access, d, addr);

  if (update)
    MR(a, addr);
}

void JitPPC64::stX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(jo.memcheck);

  MemAccess access{.store = true};
  bool indexed = false;
  bool update = false;
  switch (inst.OPCD)
  {
  case 31:
    indexed = true;
    switch (inst.SUBOP10)
    {
    case 183:  // stwux
      update = true;
      [[fallthrough]];
    case 151:  // stwx
      access.size = 32;
      break;
    case 247:  // stbux
      update = true;
      [[fallthrough]];
    case 215:  // stbx
      access.size = 8;
      break;
    case 439:  // sthux
      update = true;
      [[fallthrough]];
    case 407:  // sthx
      access.size = 16;
      break;
    case 662:  // stwbrx
      access.size = 32;
      access.reverse = true;
      break;
    case 918:  // sthbrx
      access.size = 16;
      access.reverse = true;
      break;
    default:
      ASSERT_MSG(DYNA_REC, false, "Unexpected store {:#010x}", inst.hex);
      break;
    }
    break;
  case 37:  // stwu
    update = true;
    [[fallthrough]];
  case 36:  // stw
    access.size = 32;
    break;
  case 39:  // stbu
    update = true;
    [[fallthrough]];
  case 38:  // stb
    access.size = 8;
    break;
  case 45:  // sthu
    update = true;
    [[fallthrough]];
  case 44:  // sth
    access.size = 16;
    break;
  default:
    ASSERT_MSG(DYNA_REC, false, "Unexpected store {:#010x}", inst.hex);
    break;
  }

  // The invalid form is left to the interpreter.
  FALLBACK_IF(update && inst.RA == 0);

  const auto addr = gpr.GetScopedReg();
  ComputeAddress(addr, inst, indexed);
  const GPR s = gpr.R(inst.RS);
  const GPR a = update ? gpr.RW(inst.RA, true) : GPR::R0;

  EmitMemoryAccess(access, s, addr);

  if (update)
    MR(a, addr);
}
