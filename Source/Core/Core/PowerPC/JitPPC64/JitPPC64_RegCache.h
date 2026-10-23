// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/PPC64Emitter.h"

class JitPPC64;

// Gives instructions host registers for guest GPRs, and scratch registers.
//
// Guest registers live in the non-volatile registers R14-R29, so that they survive calls. A host
// register holds a guest register's value in its low 32 bits; the upper 32 bits are undefined.
// For now, guest registers are loaded when an instruction uses them, and written back by Flush()
// after it.
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

  // Returns a host register holding guest register preg.
  PPC64Gen::GPR R(u32 preg);
  // Returns a host register for writing guest register preg. If load, it holds the current value
  // first, for instructions that also read it.
  PPC64Gen::GPR RW(u32 preg, bool load = false);

  // Writes back the guest registers that were written, and releases all of them.
  void Flush();

  // Scratch registers, from the volatile registers R5-R11.
  ScopedReg GetScopedReg();

private:
  struct GuestReg
  {
    bool bound = false;
    bool dirty = false;
    PPC64Gen::GPR host = PPC64Gen::GPR::R0;
  };

  PPC64Gen::GPR Bind(u32 preg, bool load);
  void UnlockReg(PPC64Gen::GPR reg);

  JitPPC64* m_jit = nullptr;
  std::array<GuestReg, 32> m_guest{};
  BitSet32 m_host_in_use{};
};
