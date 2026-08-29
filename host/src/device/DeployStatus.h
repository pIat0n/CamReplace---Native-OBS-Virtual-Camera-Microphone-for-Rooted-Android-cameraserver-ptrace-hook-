#pragma once

// Status events emitted by FeedController as it walks through the deploy /
// inject / start steps. The UI panel collects them into the log and the
// final Ok/Err marker tells the user whether the action succeeded.

#include <functional>
#include <string>

namespace cr::device
{

struct DeployStatus
{
  enum class Kind
  {
    Info,
    Ok,
    Err
  } kind = Kind::Info;
  std::string step;   // e.g. "push cr_injector"
  std::string detail; // stderr or a short human sentence

  static DeployStatus info(std::string s, std::string d = {})
  {
    return {Kind::Info, std::move(s), std::move(d)};
  }
  static DeployStatus ok(std::string s, std::string d = {})
  {
    return {Kind::Ok, std::move(s), std::move(d)};
  }
  static DeployStatus err(std::string s, std::string d = {})
  {
    return {Kind::Err, std::move(s), std::move(d)};
  }
};

using DeployCallback = std::function<void(DeployStatus)>;

} // namespace cr::device
