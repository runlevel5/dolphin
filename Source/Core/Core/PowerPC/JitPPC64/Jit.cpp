// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/Jit.h"

#include <cstdlib>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/GekkoDisassembler.h"
#include "Common/HostDisassembler.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/Host.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace PPC64Gen;

namespace
{
// Unconditional branches reach +/- 32 MiB, so with this much code space, any code can branch
// directly to any other code, including the dispatcher.
constexpr size_t CODE_SIZE = 32 * 1024 * 1024;
}  // namespace

JitPPC64::JitPPC64(Core::System& system)
    : JitBase(system),
      m_disassembler(HostDisassembler::Factory(HostDisassembler::Platform::ppc64le))
{
}

JitPPC64::~JitPPC64() = default;

bool JitPPC64::Init()
{
  if (!CheckValidity())
    return false;

  RefreshConfig();

  AllocCodeSpace(CODE_SIZE);

  jo.enableBlocklink = !SConfig::GetInstance().bJITNoBlockLinking;
  // All loads and stores go through the interpreter, which checks the gather pipe itself.
  jo.optimizeGatherPipe = false;
  SetOptimizationEnabled(true);
  gpr.Init(this);

  if (!blocks.Init())
    return false;

  code_block.m_stats = &js.st;
  code_block.m_gpa = &js.gpa;
  code_block.m_fpa = &js.fpa;

  GenerateAsmAndResetFreeMemoryRanges();

  return true;
}

void JitPPC64::SetOptimizationEnabled(bool enabled)
{
  if (enabled)
  {
    analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE);
    analyzer.SetOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_FOLLOW);
  }
  else
  {
    analyzer.ClearOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE);
    analyzer.ClearOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_FOLLOW);
  }
}

void JitPPC64::Shutdown()
{
  FreeCodeSpace();
  blocks.Shutdown();
}

bool JitPPC64::HandleFault(uintptr_t access_address, SContext* ctx)
{
  // We can't handle any fault from other threads.
  if (!Core::IsCPUThread())
  {
    ERROR_LOG_FMT(DYNA_REC, "Exception handler - Not on CPU thread");
    return false;
  }

  // Handle stack faults, which may happen in C++ code.
  const uintptr_t stack_guard = reinterpret_cast<uintptr_t>(m_stack_guard);
  if (access_address >= stack_guard && access_address < stack_guard + GUARD_SIZE)
    return HandleStackFault();

  ERROR_LOG_FMT(DYNA_REC, "Exception handler - Unhandled fault at {:#018x}, PC {:#018x}",
                access_address, ctx->CTX_PC);
  return false;
}

void JitPPC64::ClearCache()
{
  blocks.Clear();
  blocks.ClearRangesToFree();
  {
    const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
    ClearCodeSpace();
  }
  RefreshConfig();

  GenerateAsmAndResetFreeMemoryRanges();
}

void JitPPC64::GenerateAsmAndResetFreeMemoryRanges()
{
  ResetCodePtr();
  const u8* routines_start = GetCodePtr();
  GenerateAsm();
  ResetFreeMemoryRanges(GetCodePtr() - routines_start);

  Host_JitCacheInvalidation();
}

void JitPPC64::FreeRanges()
{
  // Check if any code blocks have been freed in the block cache and transfer this information to
  // the local rangeset to allow overwriting them with new code.
  for (const auto& [from, to] : blocks.GetRangesToFree())
    m_free_ranges.insert(from, to);
  blocks.ClearRangesToFree();
}

void JitPPC64::ResetFreeMemoryRanges(size_t routines_size)
{
  m_free_ranges.clear();
  m_free_ranges.insert(region + routines_size, region + region_size);
}

bool JitPPC64::SetEmitterStateToFreeCodeRegion()
{
  const auto free = m_free_ranges.by_size_begin();
  if (free == m_free_ranges.by_size_end())
  {
    WARN_LOG_FMT(DYNA_REC, "Failed to find free memory region in code region.");
    return false;
  }
  SetCodePtr(free.from(), free.to());
  return true;
}

void JitPPC64::Run()
{
  ProtectStack();
  m_system.GetJitInterface().UpdateMembase();

  const auto enter = reinterpret_cast<void (*)()>(const_cast<u8*>(enter_code));
  enter();

  UnprotectStack();
}

void JitPPC64::SingleStep()
{
  // Jit() compiles a single instruction at a time while stepping, so this is the same as Run().
  Run();
}

void JitPPC64::FallBackToInterpreter(UGeckoInstruction inst)
{
  // The interpreter works on PowerPCState.
  gpr.Flush(FlushMode::All);

  if (js.op->canEndBlock)
  {
    // The interpreter decides where to go next by writing npc.
    MOVI2R(GPR::R3, js.compilerPC);
    STW(GPR::R3, PPC_REG, PPCSTATE_OFF(pc));
    ADDI(GPR::R3, GPR::R3, 4);
    STW(GPR::R3, PPC_REG, PPCSTATE_OFF(npc));
  }

  const Interpreter::Instruction instr = Interpreter::GetInterpreterOp(inst);
  ABI_CallFunction(instr, &m_system.GetInterpreter(), inst.hex);

  if (js.op->branchAction == PPCAnalyst::BranchAction::IdleLoop)
  {
    // A branch back to the start of a block which does nothing but wait for an interrupt.
    LWZ(GPR::R3, PPC_REG, PPCSTATE_OFF(npc));
    MOVI2R(GPR::R4, js.blockStart);
    CMPLW(CRF::CR1, GPR::R3, GPR::R4);
    const FixupBranch not_idle = BC(CCFlags::NE, CRF::CR1);
    ABI_CallFunction(&CoreTiming::GlobalIdle);
    SetJumpTarget(not_idle);
  }

  if (js.op->canEndBlock)
  {
    LWZ(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));
    if (js.isLastInstruction)
    {
      WriteExceptionExit(DISPATCHER_PC);
    }
    else
    {
      // Only exit if the instruction branched.
      MOVI2R(GPR::R3, js.compilerPC + 4);
      CMPLW(CRF::CR1, DISPATCHER_PC, GPR::R3);
      const FixupBranch not_taken = BC(CCFlags::EQ, CRF::CR1);
      WriteExceptionExit(DISPATCHER_PC);
      SetJumpTarget(not_taken);
    }
  }
  else if (ShouldHandleFPExceptionForInstruction(js.op))
  {
    WriteConditionalExceptionExit(EXCEPTION_PROGRAM);
  }

  if (jo.memcheck && (js.op->opinfo->flags & FL_LOADSTORE))
    WriteConditionalExceptionExit(EXCEPTION_DSI);
}

void JitPPC64::HLEFunction(u32 hook_index)
{
  ABI_CallFunction(&HLE::ExecuteFromJIT, js.compilerPC, hook_index, &m_system);
}

bool JitPPC64::HandleFunctionHooking(u32 address)
{
  const auto result = HLE::TryReplaceFunction(m_ppc_symbol_db, address, PowerPC::CoreMode::JIT);
  if (!result)
    return false;

  gpr.Flush(FlushMode::All);
  HLEFunction(result.hook_index);

  if (result.type != HLE::HookType::Replace)
    return false;

  LWZ(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));
  js.downcountAmount += js.st.numCycles;
  WriteExit(DISPATCHER_PC);
  return true;
}

void JitPPC64::DoNothing(UGeckoInstruction inst)
{
  // Yup, just don't do anything.
}

void JitPPC64::Cleanup()
{
  if (m_ppc_state.feature_flags & FEATURE_FLAG_PERFMON)
  {
    ABI_CallFunction(&PowerPC::UpdatePerformanceMonitor, js.downcountAmount, js.numLoadStoreInst,
                     js.numFloatingPointInst, &m_ppc_state);
  }
}

void JitPPC64::EndProfiling()
{
  if (IsProfilingEnabled())
  {
    ABI_CallFunction(&JitBlock::ProfileData::EndProfiling, js.curBlock->profile_data.get(),
                     js.downcountAmount);
  }
}

void JitPPC64::DoDownCount()
{
  // The downcount is signed, so load it sign-extended: then the 64-bit result, which sets CR0,
  // has the right sign.
  LWA(GPR::R3, PPC_REG, PPCSTATE_OFF(downcount));
  if (js.downcountAmount <= 0x8000)
  {
    ADDIC(GPR::R3, GPR::R3, static_cast<s16>(-static_cast<s32>(js.downcountAmount)), true);
  }
  else
  {
    MOVI2R(GPR::R4, js.downcountAmount);
    SUBF(GPR::R3, GPR::R4, GPR::R3, true);
  }
  STW(GPR::R3, PPC_REG, PPCSTATE_OFF(downcount));
}

void JitPPC64::WriteExit(u32 destination)
{
  Cleanup();
  EndProfiling();
  DoDownCount();

  JitBlock::LinkData link_data{};
  link_data.exitAddress = destination;
  link_data.exitPtrs = GetWritableCodePtr();
  link_data.linkStatus = false;
  link_data.call = false;
  js.curBlock->linkData.push_back(link_data);

  blocks.WriteLinkBlock(*this, link_data);
}

void JitPPC64::WriteExit(GPR dest)
{
  if (dest != DISPATCHER_PC)
    MR(DISPATCHER_PC, dest);

  Cleanup();
  EndProfiling();
  DoDownCount();

  B(dispatcher);
}

void JitPPC64::WriteExceptionExit(u32 destination, bool only_external, bool always_exception)
{
  MOVI2R(DISPATCHER_PC, destination);
  WriteExceptionExit(DISPATCHER_PC, only_external, always_exception);
}

void JitPPC64::WriteExceptionExit(GPR dest, bool only_external, bool always_exception)
{
  if (dest != DISPATCHER_PC)
    MR(DISPATCHER_PC, dest);

  Cleanup();

  FixupBranch no_exceptions;
  if (!always_exception)
  {
    LWZ(GPR::R3, PPC_REG, PPCSTATE_OFF(Exceptions));
    CMPWI(CRF::CR1, GPR::R3, 0);
    no_exceptions = BC(CCFlags::EQ, CRF::CR1);
  }

  STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));
  STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));
  if (only_external)
    ABI_CallFunction(&PowerPC::CheckExternalExceptionsFromJIT, &m_system.GetPowerPC());
  else
    ABI_CallFunction(&PowerPC::CheckExceptionsFromJIT, &m_system.GetPowerPC());
  LWZ(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));

  if (!always_exception)
    SetJumpTarget(no_exceptions);

  EndProfiling();
  DoDownCount();

  B(dispatcher);
}

void JitPPC64::WriteConditionalExceptionExit(u32 exception)
{
  ASSERT(exception <= 0xFFFF);
  LWZ(GPR::R3, PPC_REG, PPCSTATE_OFF(Exceptions));
  ANDI_(GPR::R3, GPR::R3, static_cast<u16>(exception));
  const FixupBranch no_exception = BC(CCFlags::EQ, CRF::CR0);
  gpr.Flush(FlushMode::MaintainState);
  WriteExceptionExit(js.compilerPC, false, true);
  SetJumpTarget(no_exception);
}

void JitPPC64::Jit(u32 em_address)
{
  Jit(em_address, true);
}

void JitPPC64::Jit(u32 em_address, bool clear_cache_and_retry_on_failure)
{
  CleanUpAfterStackFault();

  if (SConfig::GetInstance().bJITNoBlockCache)
    ClearCache();
  FreeRanges();

  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;

  std::size_t block_size = m_code_buffer.size();

  auto& cpu = m_system.GetCPU();

  if (IsDebuggingEnabled())
  {
    // We can link blocks as long as we are not single stepping
    jo.enableBlocklink = !SConfig::GetInstance().bJITNoBlockLinking;
    SetOptimizationEnabled(true);

    if (!IsProfilingEnabled() && cpu.IsStepping())
    {
      block_size = 1;

      // Do not link this block to other blocks while single stepping
      jo.enableBlocklink = false;
      SetOptimizationEnabled(false);
    }
  }

  // Analyze the block, collect all instructions it is made of (including inlining,
  // if that is enabled), reorder instructions for optimal performance, and join joinable
  // instructions.
  const u32 nextPC = analyzer.Analyze(em_address, &code_block, &m_code_buffer, block_size);

  if (code_block.m_memory_exception)
  {
    // Address of instruction could not be translated
    m_ppc_state.npc = nextPC;
    m_ppc_state.Exceptions |= EXCEPTION_ISI;
    m_system.GetPowerPC().CheckExceptions();
    m_system.GetJitInterface().UpdateMembase();
    WARN_LOG_FMT(POWERPC, "ISI exception at {:#010x}", nextPC);
    return;
  }

  if (SetEmitterStateToFreeCodeRegion())
  {
    u8* near_start = GetWritableCodePtr();

    JitBlock* b = blocks.AllocateBlock(em_address);
    if (DoJit(em_address, b, nextPC))
    {
      // Code generation succeeded.

      // Mark the memory region that this code block uses as used in the local rangeset.
      u8* near_end = GetWritableCodePtr();
      if (near_start != near_end)
        m_free_ranges.erase(near_start, near_end);

      // Store the used memory region in the block so we know what to mark as unused when the
      // block gets invalidated.
      b->near_begin = near_start;
      b->near_end = near_end;
      b->far_begin = b->far_end = nullptr;

      blocks.FinalizeBlock(*b, jo.enableBlocklink, code_block, m_code_buffer);

#ifdef JIT_LOG_GENERATED_CODE
      LogGeneratedCode();
#endif
      return;
    }
  }

  if (clear_cache_and_retry_on_failure)
  {
    // Code generation failed due to not enough free space in the code region.
    // Clear the entire JIT cache and retry.
    WARN_LOG_FMT(DYNA_REC, "flushing code caches, please report if this happens a lot");
    ClearCache();
    Jit(em_address, false);
    return;
  }

  PanicAlertFmtT("JIT failed to find code space after a cache clear. This should never happen. "
                 "Please report this incident on the bug tracker. Dolphin will now exit.");
  std::exit(-1);
}

bool JitPPC64::DoJit(u32 em_address, JitBlock* b, u32 nextPC)
{
  auto& cpu = m_system.GetCPU();

  js.isLastInstruction = false;
  js.firstFPInstructionFound = false;
  js.blockStart = em_address;
  js.fifoBytesSinceCheck = 0;
  js.mustCheckFifo = false;
  js.downcountAmount = 0;
  js.skipInstructions = 0;
  js.curBlock = b;
  js.numLoadStoreInst = 0;
  js.numFloatingPointInst = 0;

  b->normalEntry = GetWritableCodePtr();

  gpr.Start();

  // Conditionally add profiling code.
  if (IsProfilingEnabled())
    ABI_CallFunction(&JitBlock::ProfileData::BeginProfiling, b->profile_data.get());

  // Translate instructions
  for (u32 i = 0; i < code_block.m_num_instructions; i++)
  {
    PPCAnalyst::CodeOp& op = m_code_buffer[i];

    js.compilerPC = op.address;
    js.op = &op;
    js.instructionsLeft = (code_block.m_num_instructions - 1) - i;
    const GekkoOPInfo* opinfo = op.opinfo;
    js.downcountAmount += opinfo->num_cycles;
    js.isLastInstruction = i == (code_block.m_num_instructions - 1);
    gpr.NextInstruction();

    if (HandleFunctionHooking(op.address))
      break;

    if (!op.skip)
    {
      // Count the instruction first, as it may exit the block.
      if (opinfo->flags & FL_LOADSTORE)
        ++js.numLoadStoreInst;

      if (opinfo->flags & FL_USE_FPU)
        ++js.numFloatingPointInst;

      if (IsDebuggingEnabled() && !cpu.IsStepping() &&
          m_system.GetPowerPC().GetBreakPoints().IsAddressBreakPoint(op.address))
      {
        gpr.Flush(FlushMode::All);
        MOVI2R(DISPATCHER_PC, op.address);
        STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(pc));
        STW(DISPATCHER_PC, PPC_REG, PPCSTATE_OFF(npc));
        ABI_CallFunction(&PowerPC::CheckAndHandleBreakPointsFromJIT, &m_system.GetPowerPC());

        MOVP2R(GPR::R3, cpu.GetStatePtr());
        LWZ(GPR::R3, GPR::R3, 0);
        static_assert(std::to_underlying(CPU::State::Running) == 0);
        CMPWI(CRF::CR1, GPR::R3, 0);
        const FixupBranch no_breakpoint = BC(CCFlags::EQ, CRF::CR1);

        Cleanup();
        EndProfiling();
        DoDownCount();
        B(dispatcher_exit);

        SetJumpTarget(no_breakpoint);
      }

      if ((opinfo->flags & FL_USE_FPU) && !js.firstFPInstructionFound)
      {
        // This instruction uses the FPU: raise an exception if it is disabled.
        LWZ(GPR::R3, PPC_REG, PPCSTATE_OFF(msr));
        ANDI_(GPR::R3, GPR::R3, 0x2000);  // MSR.FP
        const FixupBranch fpu_enabled = BC(CCFlags::NE, CRF::CR0);

        gpr.Flush(FlushMode::MaintainState);
        LWZ(GPR::R3, PPC_REG, PPCSTATE_OFF(Exceptions));
        ORI(GPR::R3, GPR::R3, EXCEPTION_FPU_UNAVAILABLE);
        STW(GPR::R3, PPC_REG, PPCSTATE_OFF(Exceptions));
        WriteExceptionExit(js.compilerPC, false, true);

        SetJumpTarget(fpu_enabled);

        js.firstFPInstructionFound = true;
      }

      CompileInstruction(op);
    }

    i += js.skipInstructions;
    js.skipInstructions = 0;
  }

  if (code_block.m_broken)
  {
    gpr.Flush(FlushMode::All);
    WriteExit(nextPC);
  }

  if (HasWriteFailed())
  {
    WARN_LOG_FMT(DYNA_REC, "JIT ran out of space in code region during code generation.");
    return false;
  }

  FlushIcache();

  return true;
}

void JitPPC64::EraseSingleBlock(const JitBlock& block)
{
  blocks.EraseSingleBlock(block);
  FreeRanges();
}

std::vector<JitBase::MemoryStats> JitPPC64::GetMemoryStats() const
{
  return {{"code", m_free_ranges.get_stats()}};
}

std::size_t JitPPC64::DisassembleNearCode(const JitBlock& block, std::ostream& stream) const
{
  return m_disassembler->Disassemble(block.normalEntry, block.near_end, stream);
}

std::size_t JitPPC64::DisassembleFarCode(const JitBlock& block, std::ostream& stream) const
{
  stream << "N/A\n";
  return 0;
}

void JitPPC64::LogGeneratedCode() const
{
  std::ostringstream stream;

  stream << "\nPPC Code Buffer:\n";
  for (const PPCAnalyst::CodeOp& op :
       std::span{m_code_buffer.data(), code_block.m_num_instructions})
  {
    fmt::print(stream, "0x{:08x}\t\t{}\n", op.address,
               Common::GekkoDisassembler::Disassemble(op.inst.hex, op.address));
  }

  const JitBlock* const block = js.curBlock;
  stream << "\nHost Code:\n";
  m_disassembler->Disassemble(block->normalEntry, block->near_end, stream);

  DEBUG_LOG_FMT(DYNA_REC, "{}", std::move(stream).str());
}
