#pragma once

// This header is included by the cxx-generated bridge code of
// lib/mixxx-rust, which is compiled without Qt include paths.
// It must remain self-contained plain C++.

#include <cstdint>

#include "rust/cxx.h"

namespace mixxxmcp {

/// Receives JSON-RPC requests from the MCP server's runtime.
///
/// `onRequest` is invoked from a Rust worker thread, so implementations
/// must be thread-safe: marshal the request to whichever thread owns the
/// Mixxx objects and answer later — exactly once per id — with
/// mixxxmcp::respond() or mixxxmcp::respond_error(). Returning without
/// answering is not fatal; the caller times out after 15 seconds. No
/// calls occur after mixxxmcp::stop_server() has returned.
class RequestHandler {
  public:
    virtual ~RequestHandler() = default;

    /// @param id       token to answer with
    /// @param method   RPC method name, e.g. "mixxx.get_state"
    /// @param params   JSON object of arguments, serialized
    virtual void onRequest(uint64_t id, rust::String method, rust::String params) = 0;
};

} // namespace mixxxmcp
