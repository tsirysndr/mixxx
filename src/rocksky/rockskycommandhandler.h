#pragma once

// This header is included by the cxx-generated bridge code of
// lib/rocksky-rs, which is compiled without Qt include paths.
// It must remain self-contained plain C++.

#include <cstdint>

#include "rust/cxx.h"

namespace rocksky {

struct QueueItemData;

/// Receives remote-control commands from the Rocksky command loop.
/// Methods are invoked from a Rust runtime worker thread — implementations
/// must be thread-safe (marshal to their own thread). No calls occur
/// after rocksky::stop_remote_player() has returned.
class CommandHandler {
  public:
    virtual ~CommandHandler() = default;

    virtual void onPlay() = 0;
    virtual void onPause() = 0;
    virtual void onNext() = 0;
    virtual void onPrevious() = 0;
    virtual void onSeek(uint64_t positionMs) = 0;
    // Queue indices are 0-based into the queue as last advertised.
    virtual void onQueueJump(uint32_t index) = 0;
    virtual void onQueueRemove(uint32_t index) = 0;
    virtual void onQueueMove(uint32_t from, uint32_t to) = 0;
    /// mode: 0 = now, 1 = next, 2 = last.
    virtual void onEnqueue(rust::Vec<QueueItemData> items, uint8_t mode) = 0;
};

} // namespace rocksky
