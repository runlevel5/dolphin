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
#include "Core/PowerPC/JitPPC64/JitPPC64_RegCache.h"
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
    _Pragma("GCC diagnostic push") _Pragma(                                                        \
        "GCC diagnostic ignored \"-Winvalid-offsetof\"") constexpr size_t ppcstate_offset =        \
        offsetof(PowerPC::PowerPCState, elem);                                                     \
    _Pragma("GCC diagnostic pop") static_assert(ppcstate_offset < 0x8000);                         \
    return static_cast<s16>(ppcstate_offset);                                                      \
  }())
#else
#define PPCSTATE_OFF(elem) (static_cast<s16>(offsetof(PowerPC::PowerPCState, elem)))
#endif

#define PPCSTATE_OFF_ARRAY(elem, i)                                                                \
  (static_cast<s16>(PPCSTATE_OFF(elem) + sizeof(PowerPC::PowerPCState::elem[0]) * (i)))
#define PPCSTATE_OFF_GPR(i) PPCSTATE_OFF_ARRAY(gpr, i)
#define PPCSTATE_OFF_CR(i) PPCSTATE_OFF_ARRAY(cr.fields, i)
#define PPCSTATE_OFF_SPR(i) PPCSTATE_OFF_ARRAY(spr, i)

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

  // OPCODES
  using Instruction = void (JitPPC64::*)(UGeckoInstruction);
  void FallBackToInterpreter(UGeckoInstruction inst);
  void DoNothing(UGeckoInstruction inst);

  void DynaRunTable4(UGeckoInstruction inst);
  void DynaRunTable19(UGeckoInstruction inst);
  void DynaRunTable31(UGeckoInstruction inst);
  void DynaRunTable59(UGeckoInstruction inst);
  void DynaRunTable63(UGeckoInstruction inst);

  // Branch
  void sc(UGeckoInstruction inst);
  void bx(UGeckoInstruction inst);
  void bcx(UGeckoInstruction inst);
  void bcctrx(UGeckoInstruction inst);
  void bclrx(UGeckoInstruction inst);

  // Integer
  void arith_imm(UGeckoInstruction inst);
  void boolX(UGeckoInstruction inst);
  void addx(UGeckoInstruction inst);
  void addix(UGeckoInstruction inst);
  void addic(UGeckoInstruction inst);
  void addcx(UGeckoInstruction inst);
  void addex(UGeckoInstruction inst);
  void addzex(UGeckoInstruction inst);
  void subfx(UGeckoInstruction inst);
  void subfex(UGeckoInstruction inst);
  void subfzex(UGeckoInstruction inst);
  void subfic(UGeckoInstruction inst);
  void negx(UGeckoInstruction inst);
  void mulli(UGeckoInstruction inst);
  void mullwx(UGeckoInstruction inst);
  void mulhwx(UGeckoInstruction inst);
  void mulhwux(UGeckoInstruction inst);
  void divwx(UGeckoInstruction inst);
  void divwux(UGeckoInstruction inst);
  void extsXx(UGeckoInstruction inst);
  void cntlzwx(UGeckoInstruction inst);
  void slwx(UGeckoInstruction inst);
  void srwx(UGeckoInstruction inst);
  void srawx(UGeckoInstruction inst);
  void srawix(UGeckoInstruction inst);
  void rlwinmx(UGeckoInstruction inst);
  void rlwnmx(UGeckoInstruction inst);
  void rlwimix(UGeckoInstruction inst);
  void cmp(UGeckoInstruction inst);
  void cmpl(UGeckoInstruction inst);
  void cmpi(UGeckoInstruction inst);
  void cmpli(UGeckoInstruction inst);

  // System registers
  void mfspr(UGeckoInstruction inst);
  void mtspr(UGeckoInstruction inst);
  void mfcr(UGeckoInstruction inst);
  void mtcrf(UGeckoInstruction inst);
  void mcrf(UGeckoInstruction inst);
  void crXXX(UGeckoInstruction inst);

private:
  void HLEFunction(u32 hook_index);
  bool HandleFunctionHooking(u32 address);
  void SetOptimizationEnabled(bool enabled);
  void CompileInstruction(PPCAnalyst::CodeOp& op);

  bool DoJit(u32 em_address, JitBlock* b, u32 nextPC);

  bool SetEmitterStateToFreeCodeRegion();
  void FreeRanges();
  void ResetFreeMemoryRanges(size_t routines_size);
  void GenerateAsmAndResetFreeMemoryRanges();
  void GenerateAsm();

  // Exits from a block. The guest registers must be flushed first. All of them subtract the block's
  // cycles from the downcount, which leaves CR0 set for the dispatcher.
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

  // Sets CR0 from the 32-bit result in value, with SO copied from XER, as the interpreter does.
  void ComputeRC0(PPC64Gen::GPR value);
  // Sets a CR field from the host CR field that a comparison set, with SO copied from XER.
  void SetCRFieldFromHostCompare(u32 field, PPC64Gen::CRF host_field);
  // Converts between a CR field and its four PowerPC bits (LT, GT, EQ, SO), in the low bits.
  void GetCRField(PPC64Gen::GPR out, u32 field);
  void SetCRField(u32 field, PPC64Gen::GPR value);
  // Branches if the CR bit (field, bit) is set (or clear).
  [[nodiscard]] PPC64Gen::FixupBranch JumpIfCRFieldBit(u32 field, u32 bit, bool jump_if_set);

  // d = x + y + carry_in, in 32 bits, setting XER[CA] to the carry out.
  enum class CarryIn
  {
    Zero,
    One,
    XER,
  };
  void AddWithCarry(PPC64Gen::GPR d, PPC64Gen::GPR x, PPC64Gen::GPR y, CarryIn carry_in);
  // Sets XER[CA] from the host's, after an algebraic shift.
  void StoreHostCarry();

  using FlushMode = PPC64GPRCache::FlushMode;
  PPC64GPRCache gpr;

  JitPPC64BlockCache blocks{*this};

  Common::RangeSizeSet<u8*> m_free_ranges;

  std::unique_ptr<HostDisassembler> m_disassembler;
};
