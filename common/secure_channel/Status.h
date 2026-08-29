#pragma once

#include <cstddef>
#include <cstdint>

namespace cr::secure_channel
{

// A stable, allocation-free result for wire parsing and provider calls.
enum class Error : std::uint8_t
{
  Ok = 0,
  Truncated,
  TrailingData,
  BadMagic,
  UnsupportedVersion,
  UnexpectedMessage,
  InvalidField,
  LengthOutOfRange,
  NonCanonicalEncoding,
  ExpiredTicket,
  TicketNotYetValid,
  TicketAudienceMismatch,
  TicketLifetimeExceeded,
  AuthenticationFailed,
  FinishedMismatch,
  InvalidState,
  ReplayDetected,
  SequenceTooOld,
  CryptoUnavailable,
  CryptoFailure,
};

struct Status
{
  Error code{Error::Ok};
  std::size_t offset{0};

  [[nodiscard]] constexpr bool ok() const noexcept
  {
    return code == Error::Ok;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept
  {
    return ok();
  }

  [[nodiscard]] static constexpr Status success() noexcept
  {
    return {};
  }

  [[nodiscard]] static constexpr Status failure(Error error, std::size_t error_offset = 0) noexcept
  {
    return Status{error, error_offset};
  }
};

} // namespace cr::secure_channel
