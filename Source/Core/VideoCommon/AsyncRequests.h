// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <concepts>
#include <functional>
#include <future>

#include "Common/CommonTypes.h"
#include "Common/Functional.h"
#include "Common/SPSCQueue.h"

struct EfbPokeData;
class PointerWrap;

class AsyncRequests
{
public:
  AsyncRequests();

  // Called from the Video thread. Runs the events that were queued once no more than `fifo_done`
  // bytes had been written to the FIFO, so that they see everything the CPU thread sent before
  // them. With `all`, runs every queued event.
  void PullEvents(u64 fifo_done, bool all);

  // The following are called from the CPU thread.
  void WaitForEmptyQueue();

  template <std::invocable<> F>
  void PushEvent(F&& callback)
  {
    if (m_passthrough)
    {
      std::invoke(std::forward<F>(callback));
      return;
    }

    QueueEvent(std::forward<F>(callback));
  }

  template <std::invocable<> F>
  auto PushBlockingEvent(F&& callback) -> std::invoke_result_t<F>
  {
    if (m_passthrough)
      return std::invoke(std::forward<F>(callback));

    std::packaged_task task{std::forward<F>(callback)};
    QueueEvent([&] { task(); });

    return task.get_future().get();
  }

  // Not thread-safe. Only set during initialization.
  void SetPassthrough(bool enable);

  static AsyncRequests* GetInstance() { return &s_singleton; }

private:
  struct Event
  {
    // How much had been written to the FIFO when the event was queued.
    u64 fifo_position;
    Common::MoveOnlyFunction<void()> callback;
  };

  void QueueEvent(Common::MoveOnlyFunction<void()>&& callback);

  static AsyncRequests s_singleton;

  Common::WaitableSPSCQueue<Event> m_queue;

  bool m_passthrough = true;
};
