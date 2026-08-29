#include "util/Log.h"
#include "util/SessionLog.h"

#include <algorithm>
#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "util/WinApiDyn.h"

namespace cr::log
{

namespace
{

const char* level_name(cr::util::LogKind k)
{
  switch (k)
  {
  case cr::util::LogKind::Info:
    return "INFO";
  case cr::util::LogKind::Warn:
    return "WARN";
  case cr::util::LogKind::Error:
    return "ERROR";
  case cr::util::LogKind::Ok:
    return "OK";
  case cr::util::LogKind::Device:
    return "DEV";
  }
  return "?";
}

// (No padding — `[component]` is variable-width on purpose so
// `[phone]` doesn't read as `[phone     ]`. Grep/parse tools key on
// the brackets, not on a fixed column.)

// Extract a "Word: rest" component prefix. Returns the component view
// and trims `body` to the rest. If no recognisable prefix exists,
// `component` is empty and `body` is unchanged.
// Recognised: prefix word [A-Za-z0-9_] up to 24 chars followed by
// ": ". Whitespace before the colon is fine. The colon is required —
// otherwise we'd false-match on URLs, paths, version numbers, etc.
void split_auto_component(std::string_view& component, std::string_view& body)
{
  component = {};

  if (body.empty())
    return;
  std::size_t i = 0;
  while (i < body.size() && i < 24 && (std::isalnum((unsigned char) body[i]) || body[i] == '_'))
  {
    ++i;
  }
  if (i == 0)
    return;
  // Trim optional space.
  std::size_t j = i;
  while (j < body.size() && body[j] == ' ')
    ++j;
  if (j >= body.size() || body[j] != ':')
    return;
  ++j;
  while (j < body.size() && body[j] == ' ')
    ++j;
  component = body.substr(0, i);
  body = body.substr(j);
}

void emit(cr::util::LogKind kind, std::string_view component, std::string_view msg)
{
  if (component.empty())
  {
    // Auto-extract from message body for legacy call sites that
    // already follow the "Foo: bar" convention.
    split_auto_component(component, msg);
  }

  // Pre-format with component column so SessionLog's file sink writes
  // aligned text. The same string lands in the UI ring.
  std::string composed;
  composed.reserve(msg.size() + 24);
  if (!component.empty())
  {
    composed.push_back('[');
    composed.append(component);
    composed.append("] ");
  }
  composed.append(msg);

  // Mirror to OutputDebugString and stderr — preserved for native
  // dev workflows. Format: "[CR] LEVEL  [comp     ] body".
  std::string dbg;
  dbg.reserve(composed.size() + 16);
  dbg.append("[CR] ");
  dbg.append(level_name(kind));
  dbg.push_back(' ');
  dbg.append(composed);
  dbg.push_back('\n');
  cr::winapi::OutputDebugStringA(dbg.c_str());
  std::fputs(dbg.c_str(), stderr);

  cr::util::SessionLog::instance().push(kind, std::move(composed));
}

} // namespace

void info(std::string_view m)
{
  emit(cr::util::LogKind::Info, {}, m);
}
void warn(std::string_view m)
{
  emit(cr::util::LogKind::Warn, {}, m);
}
void error(std::string_view m)
{
  emit(cr::util::LogKind::Error, {}, m);
}

void info(std::string_view c, std::string_view m)
{
  emit(cr::util::LogKind::Info, c, m);
}
void warn(std::string_view c, std::string_view m)
{
  emit(cr::util::LogKind::Warn, c, m);
}
void error(std::string_view c, std::string_view m)
{
  emit(cr::util::LogKind::Error, c, m);
}
void ok(std::string_view c, std::string_view m)
{
  emit(cr::util::LogKind::Ok, c, m);
}

void section(std::string_view title, std::string_view detail)
{
  SYSTEMTIME st;
  GetLocalTime(&st);
  char head[256];
  if (detail.empty())
  {
    std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d  >>> %.*s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (int) title.size(), title.data());
  }
  else
  {
    std::snprintf(head, sizeof(head), "%02d:%02d:%02d.%03d  >>> %.*s  (%.*s)", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (int) title.size(), title.data(), (int) detail.size(), detail.data());
  }
  std::string banner;
  banner.reserve(256);
  banner.append("============================================================\n");
  banner.append(head);
  banner.push_back('\n');
  banner.append("============================================================\n");
  cr::util::SessionLog::instance().write_block(std::move(banner));
}

} // namespace cr::log
