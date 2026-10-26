// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"

class JitPPC64;

enum class FlushMode
{
  // Writes back the guest registers that were written, and releases all of them.
  All,
  // Writes back the guest registers that were written, but leaves the cache as it was, for a
  // conditional exit: the code that doesn't exit continues with the same cache.
  MaintainState,
};

// Gives instructions host registers for guest GPRs and CR fields, and scratch registers.
//
// Guest registers live in the non-volatile registers R14-R27, so that they survive calls. A host
// register holds a guest GPR's value in its low 32 bits; the upper 32 bits are undefined. It holds
// a CR field as a PowerPCState::cr.fields entry does, in all 64 bits.
//
// A guest register is loaded when an instruction first uses it, and stays in its host register
// until it is flushed, or evicted to make room. Instructions must get all their guest registers
// before they branch, so that the cache is in the same state on every path through them. Code
// that leaves the block or calls into C++ code which uses PowerPCState must flush first.
class PPC64GPRCache
{
public:
  class ScopedReg
  {
  public:
    ScopedReg() = default;
    ScopedReg(PPC64GPRCache* cache, PPC64Gen::GPR reg) : m_cache(cache), m_reg(reg) {}
    ScopedReg(const ScopedReg&) = delete;
    ScopedReg& operator=(const ScopedReg&) = delete;
    ScopedReg(ScopedReg&& other) noexcept { *this = std::move(other); }
    ScopedReg& operator=(ScopedReg&& other) noexcept
    {
      Unlock();
      m_cache = std::exchange(other.m_cache, nullptr);
      m_reg = other.m_reg;
      return *this;
    }
    ~ScopedReg() { Unlock(); }

    operator PPC64Gen::GPR() const { return m_reg; }
    void Unlock()
    {
      if (m_cache)
        std::exchange(m_cache, nullptr)->UnlockReg(m_reg);
    }

  private:
    PPC64GPRCache* m_cache = nullptr;
    PPC64Gen::GPR m_reg = PPC64Gen::GPR::R0;
  };

  void Init(JitPPC64* jit);
  // Starts a block, with no guest registers in host registers.
  void Start();
  // Starts an instruction: the guest registers that it gets can't be evicted until the next one.
  void NextInstruction() { m_instruction++; }

  // Returns a host register holding guest register preg.
  PPC64Gen::GPR R(u32 preg);
  // Returns a host register for writing guest register preg. If load, it holds the current value
  // first, for instructions that also read it.
  PPC64Gen::GPR RW(u32 preg, bool load = false);

  // The same for CR field field.
  PPC64Gen::GPR CR(u32 field) { return R(CR_INDEX + field); }
  PPC64Gen::GPR CRW(u32 field, bool load = false) { return RW(CR_INDEX + field, load); }

  void Flush(FlushMode mode);
  // Writes guest register preg back if it was written, keeping it in its host register.
  void StoreIfDirty(u32 preg);
  // Forgets guest register preg without writing it back, after C++ code wrote it in
  // PowerPCState.
  void Discard(u32 preg);
  void StoreCRIfDirty(u32 field) { StoreIfDirty(CR_INDEX + field); }
  void DiscardCR(u32 field) { Discard(CR_INDEX + field); }

  // Scratch registers, from the volatile registers R5-R11.
  ScopedReg GetScopedReg();
  BitSet32 GetScratchRegsInUse() const;

private:
  // The guest registers are GPRs 0-31, then CR fields 0-7 from CR_INDEX.
  static constexpr u32 CR_INDEX = 32;

  struct GuestReg
  {
    bool bound = false;
    bool dirty = false;
    PPC64Gen::GPR host = PPC64Gen::GPR::R0;
    u32 last_used = 0;
  };

  PPC64Gen::GPR Bind(u32 preg, bool load);
  PPC64Gen::GPR AllocateGuestHostReg();
  void Release(u32 preg);
  void Load(PPC64Gen::GPR host, u32 preg);
  void Store(PPC64Gen::GPR host, u32 preg);
  void UnlockReg(PPC64Gen::GPR reg);

  JitPPC64* m_jit = nullptr;
  std::array<GuestReg, CR_INDEX + 8> m_guest{};
  BitSet32 m_host_in_use{};
  u32 m_instruction = 0;
};

// Gives instructions host registers for guest FPRs, and scratch registers, as PPC64GPRCache does
// for GPRs.
//
// A host register holds both halves of a guest register, as a PowerPCState::ps entry does:
// doubleword 0, which scalar VSX instructions work on, holds ps0, and doubleword 1 holds ps1.
// Paired single instructions and quantized loads can also hold guest registers as singles, which
// the vector single precision instructions work on: ps0 in words 0 and 1, and ps1 in words 2 and 3,
// as xvcvdpsp leaves them. Each half is a single then, which stands for the double that it
// converts to exactly, and is converted to doubles for PowerPCState and for the instructions that
// want doubles.
// Guest registers live in V20-V31 (VS52-VS63), whose 128 bits are all non-volatile.
class PPC64FPRCache
{
public:
  class ScopedReg
  {
  public:
    ScopedReg() = default;
    ScopedReg(PPC64FPRCache* cache, PPC64Gen::VSR reg) : m_cache(cache), m_reg(reg) {}
    ScopedReg(const ScopedReg&) = delete;
    ScopedReg& operator=(const ScopedReg&) = delete;
    ScopedReg(ScopedReg&& other) noexcept { *this = std::move(other); }
    ScopedReg& operator=(ScopedReg&& other) noexcept
    {
      Unlock();
      m_cache = std::exchange(other.m_cache, nullptr);
      m_reg = other.m_reg;
      return *this;
    }
    ~ScopedReg() { Unlock(); }

    operator PPC64Gen::VSR() const { return m_reg; }
    void Unlock()
    {
      if (m_cache)
        std::exchange(m_cache, nullptr)->UnlockReg(m_reg);
    }

  private:
    PPC64FPRCache* m_cache = nullptr;
    PPC64Gen::VSR m_reg = PPC64Gen::VSR::VS0;
  };

  void Init(JitPPC64* jit);
  void Start();
  void NextInstruction() { m_instruction++; }

  // Returns a host register holding guest register preg as doubles. Singles are converted, in
  // the host register.
  PPC64Gen::VSR R(u32 preg);
  // As R, for instructions that only compute with the value: singles are converted into tmp
  // instead, which quiets signaling NaNs, as computing with them does anyway.
  PPC64Gen::VSR RForArith(u32 preg, ScopedReg& tmp);
  // Returns a host register for writing guest register preg as doubles. If load, it holds the
  // current value first, for instructions that write only one half, or also read it.
  PPC64Gen::VSR RW(u32 preg, bool load = false);

  // Whether guest register preg is in a host register as singles; RSingle returns it then.
  bool HoldsSingles(u32 preg) const { return m_guest[preg].bound && m_guest[preg].single; }
  PPC64Gen::VSR RSingle(u32 preg);
  // Whether the singles of guest register preg can't be signaling NaNs, as for the results of
  // arithmetic: xvcvspdp converts those exactly.
  bool IsQuiet(u32 preg) const { return m_guest[preg].quiet; }
  // Returns a host register for writing guest register preg, which the instruction then writes as
  // singles with MarkSingles. Until then, the host register holds what it held before, as it did,
  // so that far code can still store guest register preg if the instruction reads it.
  PPC64Gen::VSR BindForSingles(u32 preg);
  void MarkSingles(u32 preg, bool quiet);
  // Guest register preg, which was held as doubles, has been converted to singles in its host
  // register, having been checked to be singles, which aren't signaling NaNs.
  void KeepAsSingles(u32 preg);

  void Flush(FlushMode mode);
  // Writes guest register preg back if it was written, keeping it in its host register.
  void StoreIfDirty(u32 preg);
  // Forgets guest register preg without writing it back, after C++ code wrote it in
  // PowerPCState.
  void Discard(u32 preg);
  // For far code, which mustn't change the cache's state: write guest register preg to
  // PowerPCState, or load it again from there, if it's in a host register. single says whether
  // the host register holds it as singles, for an instruction that changed that.
  void EmitStoreValue(u32 preg);
  void EmitStoreValue(u32 preg, bool single);
  void EmitLoadValue(u32 preg);

  // Scratch registers, from the volatile registers V0-V15 (VS32-VS47).
  ScopedReg GetScopedReg();
  bool AnyScratchRegsInUse() const { return m_scratch_in_use != 0; }

private:
  struct GuestReg
  {
    bool bound = false;
    bool dirty = false;
    // Held as singles, and if so, whether they can't be signaling NaNs.
    bool single = false;
    bool quiet = false;
    PPC64Gen::VSR host = PPC64Gen::VSR::VS0;
    u32 last_used = 0;
  };

  PPC64Gen::VSR Bind(u32 preg, bool load);
  PPC64Gen::VSR AllocateGuestHostReg();
  void Release(u32 preg);
  void Load(PPC64Gen::VSR host, u32 preg);
  void Store(PPC64Gen::VSR host, u32 preg, bool single, bool quiet);
  void UnlockReg(PPC64Gen::VSR reg);

  JitPPC64* m_jit = nullptr;
  std::array<GuestReg, 32> m_guest{};
  u64 m_host_in_use = 0;
  u32 m_scratch_in_use = 0;
  u32 m_instruction = 0;
};
