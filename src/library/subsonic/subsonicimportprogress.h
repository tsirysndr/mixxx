#pragma once

// This header is included by the cxx-generated bridge code of
// lib/subsonic-rs, which is compiled without Qt include paths.
// It must remain self-contained plain C++.

#include <atomic>
#include <cstddef>
#include <functional>
#include <utility>

namespace subsonic {

/// Progress/cancellation sink for the Rust library import. The Rust side
/// calls onProgress() from the import worker thread after every album; a
/// false return value cancels the running import.
class ImportProgress {
  public:
    using Callback =
            std::function<void(std::size_t, std::size_t, std::size_t)>;

    explicit ImportProgress(
            const std::atomic<bool>* pCancelFlag,
            Callback onProgressFn = {})
            : m_pCancelFlag(pCancelFlag),
              m_onProgressFn(std::move(onProgressFn)) {
    }

    bool onProgress(
            std::size_t tracksLoaded,
            std::size_t albumsDone,
            std::size_t albumsTotal);

  private:
    const std::atomic<bool>* m_pCancelFlag;
    Callback m_onProgressFn;
};

} // namespace subsonic
