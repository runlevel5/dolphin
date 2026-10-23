// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "Common/PPC64Emitter.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

class JitPPC64BlockCache : public JitBaseBlockCache
{
public:
  explicit JitPPC64BlockCache(JitBase& jit);

  [[nodiscard]] bool Init() override;

  void DestroyBlock(JitBlock& block) override;

  const std::vector<std::pair<u8*, u8*>>& GetRangesToFree() const;

  void ClearRangesToFree();

  void WriteLinkBlock(PPC64Gen::PPC64XEmitter& emit, const JitBlock::LinkData& source,
                      const JitBlock* dest = nullptr);

  // An exit is a fixed number of instructions, so that it can be rewritten when blocks are linked
  // and unlinked:
  //
  //   Unlinked:                     Linked:
  //   nop                           ble cr0, 1f        # Out of cycles: go through the dispatcher
  //   nop                           b dest
  //   MOVI2R DISPATCHER_PC, exit    1: MOVI2R DISPATCHER_PC, exit
  //   b dispatcher                  b dispatcher
  //
  // MOVI2R of a 32-bit address takes up to three instructions, padded with nops. The dispatcher
  // goes to do_timing when CR0 says that the block ran out of cycles.
  static constexpr size_t BLOCK_LINK_SIZE = 6 * sizeof(u32);

private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override;
  void WriteDestroyBlock(const JitBlock& block) override;

  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen;
};
