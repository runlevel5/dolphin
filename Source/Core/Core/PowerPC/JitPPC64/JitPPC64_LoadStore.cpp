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
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
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

BitSet32 JitPPC64::SaveScratchRegs(BitSet32 except)
{
  // Scratch registers are volatile, so save those in use around a call; guest registers are
  // non-volatile. No scratch VSX registers may be in use.
  ASSERT(!fpr.AnyScratchRegsInUse());
  const BitSet32 saved = gpr.GetScratchRegsInUse() & ~except;
  for (const int i : saved)
    STD(static_cast<GPR>(i), GPR::SP, ScratchSaveOffset(static_cast<GPR>(i)));
  return saved;
}

void JitPPC64::RestoreScratchRegs(BitSet32 saved)
{
  for (const int i : saved)
    LD(static_cast<GPR>(i), GPR::SP, ScratchSaveOffset(static_cast<GPR>(i)));
}

void JitPPC64::EmitMemoryAccess(MemAccess access, GPR value, GPR addr)
{
  // Stores that wrote to the gather pipe before, through the slow path, write to it directly when
  // they're to its address, which fastmem can't do. Like GPFifo's writes, they burst the pipe as
  // soon as it's full.
  constexpr u32 gather_pipe_address = 0xCC008000;
  if (access.store && js.fifoWriteAddresses.TestBit(js.compilerPC / sizeof(UGeckoInstruction)) &&
      m_mmu.IsOptimizableGatherPipeWrite(gather_pipe_address))
  {
    const auto ptr = gpr.GetScopedReg();
    MOVI2R(ptr, gather_pipe_address);
    CMPLW(CRF::CR1, addr, ptr);
    const FixupBranch not_gather_pipe = BC(CCFlags::NE, CRF::CR1);

    LD(ptr, PPC_REG, PPCSTATE_OFF(gather_pipe_ptr));
    switch (access.size)
    {
    case 64:
      STDBRX(value, GPR::R0, ptr);
      break;
    case 32:
      access.reverse ? STWX(value, GPR::R0, ptr) : STWBRX(value, GPR::R0, ptr);
      break;
    case 16:
      access.reverse ? STHX(value, GPR::R0, ptr) : STHBRX(value, GPR::R0, ptr);
      break;
    default:
      STBX(value, GPR::R0, ptr);
      break;
    }
    ADDI(ptr, ptr, static_cast<s16>(access.size / 8));
    STD(ptr, PPC_REG, PPCSTATE_OFF(gather_pipe_ptr));
    {
      const auto base = gpr.GetScopedReg();
      LD(base, PPC_REG, PPCSTATE_OFF(gather_pipe_base_ptr));
      SUBF(ptr, base, ptr);
    }
    CMPLDI(CRF::CR1, ptr, GPFifo::GATHER_PIPE_SIZE);
    const FixupBranch full = BCFar(CCFlags::GE, CRF::CR1);
    const u8* written = GetCodePtr();

    SwitchToFarCode();
    SetJumpTarget(full);
    {
      const BitSet32 saved = SaveScratchRegs();
      ABI_CallFunction(&GPFifo::UpdateGatherPipe, &m_system.GetGPFifo());
      RestoreScratchRegs(saved);
    }
    B(written);
    SwitchToNearCode();

    const FixupBranch done = B();
    SetJumpTarget(not_gather_pipe);
    EmitMemoryAccessToAddress(access, value, addr);
    SetJumpTarget(done);
    return;
  }

  EmitMemoryAccessToAddress(access, value, addr);
}

void JitPPC64::EmitMemoryAccessToAddress(MemAccess access, GPR value, GPR addr)
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

u32 JitPPC64::SlowAccessRoutineIndex(MemAccess access)
{
  const u32 size_index = access.size == 64 ? 3 : access.size == 32 ? 2 : access.size == 16 ? 1 : 0;
  return (access.store ? 16 : 0) | (size_index << 2) | (access.sign_extend ? 2 : 0) |
         (access.reverse ? 1 : 0);
}

void JitPPC64::GenerateSlowAccessRoutines()
{
  // The slow path of each kind of access, shared by the accesses, which keeps far code small. They
  // take the address in R3, zero-extended, and a stored value in R4, zero-extended to its size,
  // return a loaded value in R3, and preserve the scratch registers.
  m_slow_access_routines = {};
  for (const bool store : {false, true})
  {
    for (const u32 size : {8u, 16u, 32u, 64u})
    {
      for (const bool sign_extend : {false, true})
      {
        for (const bool reverse : {false, true})
        {
          if ((sign_extend && (store || size != 16)) || (reverse && (size == 8 || size == 64)) ||
              (sign_extend && reverse))
          {
            continue;
          }
          const MemAccess access{store, size, sign_extend, reverse};
          m_slow_access_routines[SlowAccessRoutineIndex(access)] = GetCodePtr();
          GenerateSlowAccessRoutine(access);
        }
      }
    }
  }
}

void JitPPC64::GenerateSlowAccessRoutine(MemAccess access)
{
  MFLR(GPR::R0);
  STD(GPR::R0, GPR::SP, ROUTINE_LR_SAVE);
  for (u32 i = 5; i <= 11; i++)
    STD(static_cast<GPR>(i), GPR::SP, ScratchSaveOffset(static_cast<GPR>(i)));

  if (access.store)
  {
    MR(GPR::R5, GPR::R3);
    switch (access.size)
    {
    case 64:
      ABI_CallFunction(&PowerPC::WriteFromJit<u64>, &m_mmu, GPR::R4, GPR::R5);
      break;
    case 32:
      ABI_CallFunction(access.reverse ? &PowerPC::WriteU32SwapFromJit : &PowerPC::WriteFromJit<u32>,
                       &m_mmu, GPR::R4, GPR::R5);
      break;
    case 16:
      ABI_CallFunction(access.reverse ? &PowerPC::WriteU16SwapFromJit : &PowerPC::WriteFromJit<u16>,
                       &m_mmu, GPR::R4, GPR::R5);
      break;
    default:
      ABI_CallFunction(&PowerPC::WriteFromJit<u8>, &m_mmu, GPR::R4, GPR::R5);
      break;
    }
  }
  else
  {
    MR(GPR::R4, GPR::R3);
    switch (access.size)
    {
    case 64:
      ABI_CallFunction(&PowerPC::ReadFromJit<u64>, &m_mmu, GPR::R4);
      break;
    case 32:
      ABI_CallFunction(&PowerPC::ReadFromJit<u32>, &m_mmu, GPR::R4);
      if (access.reverse)
      {
        ROTLWI(GPR::R4, GPR::R3, 8);
        RLWIMI(GPR::R4, GPR::R3, 24, 0, 7);
        RLWIMI(GPR::R4, GPR::R3, 24, 16, 23);
        MR(GPR::R3, GPR::R4);
      }
      break;
    case 16:
      ABI_CallFunction(&PowerPC::ReadFromJit<u16>, &m_mmu, GPR::R4);
      if (access.reverse)
      {
        RLWINM(GPR::R4, GPR::R3, 24, 24, 31);
        RLWIMI(GPR::R4, GPR::R3, 8, 16, 23);
        MR(GPR::R3, GPR::R4);
      }
      else if (access.sign_extend)
      {
        EXTSH(GPR::R3, GPR::R3);
      }
      break;
    default:
      ABI_CallFunction(&PowerPC::ReadFromJit<u8>, &m_mmu, GPR::R4);
      break;
    }
  }

  for (u32 i = 5; i <= 11; i++)
    LD(static_cast<GPR>(i), GPR::SP, ScratchSaveOffset(static_cast<GPR>(i)));
  LD(GPR::R0, GPR::SP, ROUTINE_LR_SAVE);
  MTLR(GPR::R0);
  BLR();
}

void JitPPC64::EmitSlowAccess(MemAccess access, GPR value, GPR addr)
{
  // The routine saves the scratch registers, which are volatile; guest registers are
  // non-volatile. No scratch VSX registers may be in use.
  ASSERT(!fpr.AnyScratchRegsInUse());

  // The PC is used by memory checks, by GPFifo to find stores to the gather pipe, and in logs.
  MOVI2R(GPR::R12, js.compilerPC);
  STW(GPR::R12, PPC_REG, PPCSTATE_OFF(pc));

  CLRLDI(GPR::R3, addr, 32);
  if (access.store)
  {
    if (access.size == 64)
      MR(GPR::R4, value);
    else
      CLRLDI(GPR::R4, value, 32);
  }
  BL(m_slow_access_routines[SlowAccessRoutineIndex(access)]);
  // With memory checks, an access the MMU can't translate raises a DSI exception, which is taken
  // before the instruction writes any register: instructions bind the registers they write after
  // their access.
  if (jo.memcheck)
    WriteConditionalExceptionExit(EXCEPTION_DSI, GPR::R12);
  if (!access.store)
    MR(value, GPR::R3);
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

  if (jo.memcheck)
  {
    const auto value = gpr.GetScopedReg();
    EmitMemoryAccess(access, value, addr);
    MR(gpr.RW(inst.RD), value);
  }
  else
  {
    EmitMemoryAccess(access, gpr.RW(inst.RD), addr);
  }

  if (update)
    MR(a, addr);
}

void JitPPC64::stX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

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

void JitPPC64::dcbx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(m_accurate_cpu_cache_enabled);

  // dcbst, dcbf and dcbi invalidate the JIT's code for the cache line, as a heuristic for games
  // that don't use icbi. Check the block cache's bit set for the physical line first, as JitArm64
  // does, so that only lines with code call into the block cache.
  const auto addr = gpr.GetScopedReg();
  const auto physical = gpr.GetScopedReg();
  const auto tmp = gpr.GetScopedReg();
  if (inst.RA)
    ADD(addr, gpr.R(inst.RA), gpr.R(inst.RB));
  else
    MR(addr, gpr.R(inst.RB));
  CLRLDI(addr, addr, 32);

  FixupBranch not_mapped;
  const bool translate = (m_ppc_state.feature_flags & FEATURE_FLAG_MSR_IR) != 0;
  if (translate)
  {
    // physical = (ibat[addr >> 17] & ~0x1ffff) | (addr & 0x1ffff), if the entry is mapped.
    MOVP2R(tmp, m_mmu.GetIBATTable().data());
    RLWINM(physical, addr, 32 - PowerPC::BAT_INDEX_SHIFT + 2, PowerPC::BAT_INDEX_SHIFT - 2, 29);
    LWZX(physical, tmp, physical);
    ANDI_(tmp, physical, PowerPC::BAT_MAPPED_BIT);
    not_mapped = BCFar(CCFlags::EQ, CRF::CR0);
    RLWIMI(physical, addr, 0, 32 - PowerPC::BAT_INDEX_SHIFT, 31);
  }
  else
  {
    MR(physical, addr);
  }

  // Bit (physical >> 5) of the bit set, in 32-bit words.
  MOVP2R(tmp, GetBlockCache()->GetBlockBitSet());
  {
    const auto word = gpr.GetScopedReg();
    RLWINM(word, physical, 32 - 10 + 2, 10 - 2, 29);
    LWZX(word, tmp, word);
    RLWINM(physical, physical, 32 - 5, 27, 31);
    SRW(word, word, physical);
    ANDI_(word, word, 1);
  }
  const FixupBranch has_code = BCFar(CCFlags::NE, CRF::CR0);
  const u8* done = GetCodePtr();

  SwitchToFarCode();
  SetJumpTarget(has_code);
  if (translate)
    SetJumpTarget(not_mapped);
  {
    MR(GPR::R12, addr);
    const BitSet32 saved = SaveScratchRegs();
    ABI_CallFunction(&JitInterface::InvalidateICacheLineFromJIT, &m_system.GetJitInterface(),
                     GPR::R12);
    RestoreScratchRegs(saved);
  }
  B(done);
  SwitchToNearCode();
}

void JitPPC64::lmw_stmw(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  // These access up to 32 guest registers, more than fit in host registers at once, so they work on
  // PowerPCState: lmw flushes the registers it overwrites, and stmw writes back the ones it stores.
  // With memory checks, a DSI exception leaves the words before it loaded or stored, as the
  // interpreter does.
  const bool store = inst.OPCD == 47;
  const auto addr = gpr.GetScopedReg();
  if (inst.RA == 0)
    MOVI2R(addr, static_cast<u32>(static_cast<s32>(inst.SIMM_16)));
  else
    ADDI(addr, gpr.R(inst.RA), inst.SIMM_16);
  CLRLDI(addr, addr, 32);
  gpr.Flush(store ? FlushMode::MaintainState : FlushMode::All);

  // A misaligned address raises an alignment exception, which the interpreter does. (So does
  // MSR[LE], which games don't use.)
  ANDI_(GPR::R0, addr, 3);
  const FixupBranch misaligned = BCFar(CCFlags::NE, CRF::CR0);

  {
    const auto value = gpr.GetScopedReg();
    const auto word_addr = gpr.GetScopedReg();
    for (u32 i = inst.RD; i <= 31; i++)
    {
      ADDI(word_addr, addr, static_cast<s16>(4 * (i - inst.RD)));
      CLRLDI(word_addr, word_addr, 32);
      if (store)
      {
        LWZ(value, PPC_REG, PPCSTATE_OFF_GPR(i));
        EmitMemoryAccess({.store = true, .size = 32}, value, word_addr);
      }
      else
      {
        EmitMemoryAccess({.size = 32}, value, word_addr);
        STW(value, PPC_REG, PPCSTATE_OFF_GPR(i));
      }
    }
  }
  const u8* done = GetCodePtr();

  SwitchToFarCode();
  SetJumpTarget(misaligned);
  ABI_CallFunction(Interpreter::GetInterpreterOp(inst), &m_system.GetInterpreter(), inst.hex);
  B(done);
  SwitchToNearCode();
}
