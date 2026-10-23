// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>

#include "Common/PPC64Emitter.h"
#include "Common/RangeSizeSet.h"
#include "Core/PowerPC/JitCommon/JitAsmCommon.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitPPC64/JitPPC64Cache.h"
#include "Core/PowerPC/PPCAnalyst.h"

class HostDisassembler;

// The PowerPCState pointer. Non-volatile, so that it survives calls.
constexpr PPC64Gen::GPR PPC_REG = PPC64Gen::GPR::R31;
// The guest PC when entering the dispatcher. Non-volatile, so that it survives calls.
constexpr PPC64Gen::GPR DISPATCHER_PC = PPC64Gen::GPR::R30;

// Offsets into PowerPCState, for D-form loads and stores relative to PPC_REG.
#ifdef __GNUC__
#define PPCSTATE_OFF(elem)                                                                         \
  ([]() consteval {                                                                                \
    _Pragma("GCC diagnostic push")                                                                 \
        _Pragma("GCC diagnostic ignored \"-Winvalid-offsetof\"") constexpr size_t offset =         \
            offsetof(PowerPC::PowerPCState, elem);                                                 \
    _Pragma("GCC diagnostic pop") static_assert(offset < 0x8000);                                  \
    return static_cast<s16>(offset);                                                               \
  }())
#else
#define PPCSTATE_OFF(elem) (static_cast<s16>(offsetof(PowerPC::PowerPCState, elem)))
#endif

class JitPPC64 : public JitBase, public PPC64Gen::PPC64CodeBlock, public CommonAsmRoutinesBase
{
public:
  explicit JitPPC64(Core::System& system);
  JitPPC64(const JitPPC64&) = delete;
  JitPPC64(JitPPC64&&) = delete;
  JitPPC64& operator=(const JitPPC64&) = delete;
  JitPPC64& operator=(JitPPC64&&) = delete;
  ~JitPPC64() override;

  [[nodiscard]] bool Init() override;
  void Shutdown() override;

  JitBaseBlockCache* GetBlockCache() override { return &blocks; }

  bool HandleFault(uintptr_t access_address, SContext* ctx) override;

  void ClearCache() override;

  CommonAsmRoutinesBase* GetAsmRoutines() override { return this; }

  void Run() override;
  void SingleStep() override;

  void Jit(u32 em_address) override;
  void Jit(u32 em_address, bool clear_cache_and_retry_on_failure);

  void EraseSingleBlock(const JitBlock& block) override;
  std::vector<MemoryStats> GetMemoryStats() const override;

  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override;
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override;

  const char* GetName() const override { return "JITPPC64"; }

private:
  void FallBackToInterpreter(UGeckoInstruction inst);
  void HLEFunction(u32 hook_index);
  bool HandleFunctionHooking(u32 address);
  void CompileInstruction(PPCAnalyst::CodeOp& op);

  bool DoJit(u32 em_address, JitBlock* b, u32 nextPC);

  bool SetEmitterStateToFreeCodeRegion();
  void FreeRanges();
  void ResetFreeMemoryRanges(size_t routines_size);
  void GenerateAsmAndResetFreeMemoryRanges();
  void GenerateAsm();

  // Exits from a block. All of them subtract the block's cycles from the downcount, which leaves
  // CR0 set for the dispatcher.
  void Cleanup();
  void DoDownCount();
  void EndProfiling();
  // Exits to a known address, which can be linked to the block there.
  void WriteExit(u32 destination);
  // Exits to the address in a register, through the dispatcher.
  void WriteExit(PPC64Gen::GPR dest);
  // Checks for exceptions, then exits through the dispatcher.
  void WriteExceptionExit(u32 destination, bool only_external = false,
                          bool always_exception = false);
  void WriteExceptionExit(PPC64Gen::GPR dest, bool only_external = false,
                          bool always_exception = false);
  // Exits at the current instruction if the given exception is pending.
  void WriteConditionalExceptionExit(u32 exception);

  void LogGeneratedCode() const;

  JitPPC64BlockCache blocks{*this};

  Common::RangeSizeSet<u8*> m_free_ranges;

  std::unique_ptr<HostDisassembler> m_disassembler;
};
