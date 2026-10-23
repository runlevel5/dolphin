// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitPPC64/JitPPC64_RegCache.h"

#include "Common/Assert.h"
#include "Core/PowerPC/JitPPC64/Jit.h"

using namespace PPC64Gen;

namespace
{
constexpr u32 FIRST_GUEST_HOST_REG = 14;
constexpr u32 LAST_GUEST_HOST_REG = 29;
constexpr u32 FIRST_SCRATCH_REG = 5;
constexpr u32 LAST_SCRATCH_REG = 11;
}  // namespace

void PPC64GPRCache::Init(JitPPC64* jit)
{
  m_jit = jit;
  m_guest = {};
  m_host_in_use = {};
}

GPR PPC64GPRCache::Bind(u32 preg, bool load)
{
  GuestReg& guest = m_guest[preg];
  if (guest.bound)
    return guest.host;

  for (u32 i = FIRST_GUEST_HOST_REG; i <= LAST_GUEST_HOST_REG; i++)
  {
    if (m_host_in_use[i])
      continue;

    m_host_in_use[i] = true;
    guest.bound = true;
    guest.dirty = false;
    guest.host = static_cast<GPR>(i);
    if (load)
      m_jit->LWZ(guest.host, PPC_REG, PPCSTATE_OFF_GPR(preg));
    return guest.host;
  }

  ASSERT_MSG(DYNA_REC, false, "Out of host registers for guest registers");
  return GPR::R0;
}

GPR PPC64GPRCache::R(u32 preg)
{
  return Bind(preg, true);
}

GPR PPC64GPRCache::RW(u32 preg, bool load)
{
  const GPR host = Bind(preg, load);
  m_guest[preg].dirty = true;
  return host;
}

void PPC64GPRCache::Flush()
{
  for (u32 preg = 0; preg < m_guest.size(); preg++)
  {
    GuestReg& guest = m_guest[preg];
    if (!guest.bound)
      continue;

    if (guest.dirty)
      m_jit->STW(guest.host, PPC_REG, PPCSTATE_OFF_GPR(preg));
    m_host_in_use[DecodeReg(guest.host)] = false;
    guest = {};
  }
}

PPC64GPRCache::ScopedReg PPC64GPRCache::GetScopedReg()
{
  for (u32 i = FIRST_SCRATCH_REG; i <= LAST_SCRATCH_REG; i++)
  {
    if (m_host_in_use[i])
      continue;

    m_host_in_use[i] = true;
    return ScopedReg(this, static_cast<GPR>(i));
  }

  ASSERT_MSG(DYNA_REC, false, "Out of scratch registers");
  return {};
}

void PPC64GPRCache::UnlockReg(GPR reg)
{
  ASSERT(m_host_in_use[DecodeReg(reg)]);
  m_host_in_use[DecodeReg(reg)] = false;
}
