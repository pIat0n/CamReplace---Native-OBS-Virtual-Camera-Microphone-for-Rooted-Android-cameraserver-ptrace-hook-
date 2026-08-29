#pragma once

// Logging facade. Two flavours:
//   * Untagged:   cr::log::info("message")
//                 → emits with auto-detected component if "message"
//                   starts with "Word: ..." (e.g. "RtmpServer: ..."),
//                   otherwise component is left blank.
//   * Tagged:     cr::log::info("rtmp", "listening on tcp:1935")
//                 → emits with component="rtmp" explicitly.
// Plus session markers that print a visible banner block:
//   cr::log::section("Start replace camera", "serial=ABC123")
// All sinks merge into util::SessionLog (UI ring + session log artifact).
// Format on disk is column-aligned so a maintainer can scan a log dump
// and spot the component / level columns without parsing.

#include <string_view>

namespace cr::log
{

// Untagged (component auto-extracted from "Comp: msg" prefix if present).
void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);

// Component-tagged. `component` is short (under ~10 chars works best
// for the column width) and identifies the subsystem: "scan", "deploy",
// "inject", "hook", "rtmp", "audio", "ui", "device".
void info(std::string_view component, std::string_view msg);
void warn(std::string_view component, std::string_view msg);
void error(std::string_view component, std::string_view msg);
void ok(std::string_view component, std::string_view msg);

// Visual section marker in the log file (ignored by the UI ring).
// Use for top-level user actions and lifecycle events that future
// maintainers will want to navigate to instantly:
//   cr::log::section("Start replace camera", "serial=ABC123");
//   → ============================================================
//     22:45:13.123  >>> Start replace camera  (serial=ABC123)
// `detail` is optional (pass empty/default when not needed).
void section(std::string_view title, std::string_view detail = {});

} // namespace cr::log
