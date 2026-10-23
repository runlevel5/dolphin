// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <cstddef>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Common/MemoryUtil.h"
#include "Common/PPC64Emitter.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace PPC64Gen;

namespace
{
// The stack frame of the JIT, below the minimal ELFv2 frame header. The parameter save area isn't
// needed by the functions the JIT calls, but costs nothing.
constexpr s32 PARAMETER_SAVE_AREA = STACK_FRAME_MIN_SIZE;
static_assert(JitPPC64::SCRATCH_SAVE == PARAMETER_SAVE_AREA + 8 * 8);
constexpr s32 TOC_SAVE = JitPPC64::SCRATCH_SAVE + JitPPC64::SCRATCH_SAVE_SIZE;
// The non-volatile registers of the ELFv2 ABI: r14-r31, f14-f31 and v20-v31.
constexpr u32 FIRST_SAVED_GPR = 14;
constexpr u32 FIRST_SAVED_FPR = 14;
constexpr u32 FIRST_SAVED_VR = 20;
constexpr s32 GPR_SAVE = TOC_SAVE + 8;
constexpr s32 FPR_SAVE = GPR_SAVE + 8 * (32 - FIRST_SAVED_GPR);
constexpr s32 VR_SAVE = (FPR_SAVE + 8 * (32 - FIRST_SAVED_FPR) + 15) & ~15;
constexpr s32 FRAME_SIZE = VR_SAVE + 16 * (32 - FIRST_SAVED_VR);
static_assert(FRAME_SIZE % 16 == 0, "The stack pointer must stay 16-byte aligned");

// CR2-CR4 are non-volatile.
constexpr u32 SAVED_CR_FIELDS = 0x38;
// The CR save word, in the caller's frame.
constexpr s32 CR_SAVE = 8;
}  // namespace

void JitPPC64::GenerateAsm()
{
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;

  const bool enable_debugging = Config::IsDebuggingEnabled();
  auto& cpu = m_system.GetCPU();

  enter_code = GetCodePtr();

  // Prologue: save LR, CR and all non-volatile registers, and the TOC pointer for our caller.
  MFLR(GPR::R0);
  STD(GPR::R0, GPR::SP, STACK_FRAME_LR_SAVE);
  MFCR(GPR::R0);
  STW(GPR::R0, GPR::SP, CR_SAVE);
  STDU(GPR::SP, GPR::SP, -FRAME_SIZE);
  STD(GPR::TOC, GPR::SP, TOC_SAVE);
  for (u32 i = FIRST_SAVED_GPR; i < 32; i++)
    STD(static_cast<GPR>(i), GPR::SP, GPR_SAVE + 8 * (i - FIRST_SAVED_GPR));
  for (u32 i = FIRST_SAVED_FPR; i < 32; i++)
    STFD(static_cast<FPR>(i), GPR::SP, FPR_SAVE + 8 * (i - FIRST_SAVED_FPR));
  for (u32 i = FIRST_SAVED_VR; i < 32; i++)
  {
    ADDI(GPR::R12, GPR::SP, VR_SAVE + 16 * (i - FIRST_SAVED_VR));
    STXVD2X(ToVSR(static_cast<VR>(i)), GPR::R0, GPR::R12);
  }

  MOVP2R(PPC_REG, &m_ppc_state);

  // The PC will be loaded into DISPATCHER_PC after the call to CoreTiming::Advance().
  // Advance() does an exception check so we don't know what PC to use until afterwards.
  const FixupBranch to_start_of_timing_slice = B();

  // do
  // {
  //   CoreTiming::Advance();  // <-- Checks for exceptions (changes PC)
  //   DISPATCHER_PC = PC;
  //   do
  //   {
  // dispatcher_no_check:
  //     ExecuteBlock(JitBase::Dispatch());
  // dispatcher:
  //   } while (m_ppc_state.downcount > 0);
  // do_timing:
  //   NPC = PC = DISPATCHER_PC;
  // } while (CPU::GetState() == CPU::State::Running);
  AlignCodePage();
  dispatcher = GetCodePtr();

  // Blocks exit to here with CR0 set from the downcount.
  const FixupBranch bail = BC(CCFlags::LE, CRF::CR0);

  dispatcher_no_timing_check = GetCodePtr();

  FixupBranch debug_exit;
  if (enable_debugging)
  {
    MOVP2R(GPR::R3, cpu.GetStatePtr());
    LWZ(GPR::R3, GPR::R3, 0);
    static_assert(std::to_underlying(CPU::State::Running) == 0);
    CMPWI(CRF::CR1, GPR::R3, 0);
    debug_exit = BC(CCFlags::NE, CRF::CR1);
  }

  dispatcher_no_check = GetCodePtr();

  // C++ code may have changed MSR[DR], and with it the arena.
  LD(MEM_REG, PPC_REG, PPCSTATE_OFF(mem_ptr));

  if (u8** const entry_points = GetBlockCache()->GetEntryPoints())
  {
    // The entry points map is indexed by ((feature_flags << 30) | (pc >> 2)), and holds 8-byte
    // pointers, so the byte offset is (feature_flags << 33) | (pc << 1).
    LWZ(GPR::R3, PPC_REG, PPCSTATE_OFF(feature_flags));
    MOVP2R(GPR::R4, entry_points);
    SLDI(GPR::R5, DISPATCHER_PC, 1);
    SLDI(GPR::R3, GPR::R3, 33);
    OR(GPR::R5, GPR::R5, GPR::R3);
    LDX(GPR::R6, GPR::R4, GPR::R5);
    CMPDI(CRF::CR1, GPR::R6, 0);
    const FixupBranch not_found = BC(CCFlags::EQ, CRF::CR1);
    MTCTR(GPR::R6);
    BCTR();
    SetJumpTarget(not_found);
  }
  else
  {
    // block = fast_block_map_fallback[(pc >> 2) & FAST_BLOCK_MAP_FALLBACK_MASK];
    static_assert(JitBaseBlockCache::FAST_BLOCK_MAP_FALLBACK_MASK == 0xFFFF);
    MOVP2R(GPR::R4, GetBlockCache()->GetFastBlockMapFallback());
    // ((pc >> 2) & 0xFFFF) * 8 == (pc << 1) & 0x7FFF8
    RLWINM(GPR::R5, DISPATCHER_PC, 1, 13, 28);
    LDX(GPR::R6, GPR::R4, GPR::R5);
    CMPDI(CRF::CR1, GPR::R6, 0);
    const FixupBranch not_found = BC(CCFlags::EQ, CRF::CR1);

    // if (block->effectiveAddress == pc && block->feature_flags == feature_flags)
    LWZ(GPR::R7, GPR::R6, offsetof(JitBlockData, effectiveAddress));
    CMPLW(CRF::CR1, GPR::R7, DISPATCHER_PC);
    const FixupBranch pc_mismatch = BC(CCFlags::NE, CRF::CR1);
    LWZ(GPR::R7, GPR::R6, offsetof(JitBlockData, feature_flags));
    LWZ(GPR::R8, PPC_REG, PPCSTATE_OFF(feature_flags));
    CMPLW(CRF::CR1, GPR::R7, GPR::R8);
    const FixupBranch feature_flags_mismatch = BC(CCFlags::NE, CRF::CR1);

    // return block->normalEntry;
    LD(GPR::R7, GPR::R6, offsetof(JitBlockData, normalEntry));
    MTCTR(GPR::R7);
    BCTR();

    SetJumpTarget(not_found);
    SetJumpTarget(pc_mismatch);
    SetJumpTarget(feature_flags_mismatch);
  }

  STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));

  // There is no point in calling the dispatcher in the fast lookup table case, since the assembly
  // dispatcher would already have found a block.
  if (!GetBlockCache()->GetEntryPoints())
  {
    // Call C version of Dispatch().
    ABI_CallFunction(&JitBase::Dispatch, static_cast<JitBase*>(this));
    CMPDI(CRF::CR1, GPR::R3, 0);
    const FixupBranch no_block_available = BC(CCFlags::EQ, CRF::CR1);
    MTCTR(GPR::R3);
    BCTR();
    SetJumpTarget(no_block_available);
  }

  // Call JIT
  ABI_CallFunction(&JitTrampoline, static_cast<JitBase*>(this), DISPATCHER_PC);
  LWZ(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));
  B(dispatcher_no_check);

  SetJumpTarget(bail);
  do_timing = GetCodePtr();
  // Write the current PC out to PPCSTATE
  STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));
  STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));

  // Check the state pointer to see if we are exiting
  // Gets checked on at the end of every slice
  MOVP2R(GPR::R3, cpu.GetStatePtr());
  LWZ(GPR::R3, GPR::R3, 0);
  CMPWI(CRF::CR1, GPR::R3, 0);
  const FixupBranch exit = BC(CCFlags::NE, CRF::CR1);

  SetJumpTarget(to_start_of_timing_slice);
  ABI_CallFunction(&CoreTiming::GlobalAdvance);

  // Load the PC back into DISPATCHER_PC (the exception handler might have changed it)
  LWZ(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));

  // We can safely assume that downcount >= 1
  B(dispatcher_no_check);

  if (enable_debugging)
  {
    SetJumpTarget(debug_exit);
    STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));
    STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));
  }

  dispatcher_exit = GetCodePtr();
  SetJumpTarget(exit);

  // Epilogue
  for (u32 i = FIRST_SAVED_VR; i < 32; i++)
  {
    ADDI(GPR::R12, GPR::SP, VR_SAVE + 16 * (i - FIRST_SAVED_VR));
    LXVD2X(ToVSR(static_cast<VR>(i)), GPR::R0, GPR::R12);
  }
  for (u32 i = FIRST_SAVED_FPR; i < 32; i++)
    LFD(static_cast<FPR>(i), GPR::SP, FPR_SAVE + 8 * (i - FIRST_SAVED_FPR));
  for (u32 i = FIRST_SAVED_GPR; i < 32; i++)
    LD(static_cast<GPR>(i), GPR::SP, GPR_SAVE + 8 * (i - FIRST_SAVED_GPR));
  LD(GPR::TOC, GPR::SP, TOC_SAVE);
  ADDI(GPR::SP, GPR::SP, FRAME_SIZE);
  LWZ(GPR::R0, GPR::SP, CR_SAVE);
  MTCRF(SAVED_CR_FIELDS, GPR::R0);
  LD(GPR::R0, GPR::SP, STACK_FRAME_LR_SAVE);
  MTLR(GPR::R0);
  BLR();

  Common::JitRegister::Register(enter_code, GetCodePtr(), "JIT_Dispatcher");

  FlushIcache();
}
