#pragma once

#include "V2Protocol.h"

#include <array>
#include <vector>

namespace cr::secure_channel::v2
{

class SecureSession
{
public:
  SecureSession() = default;
  ~SecureSession();
  SecureSession(const SecureSession&) = delete;
  SecureSession& operator=(const SecureSession&) = delete;

  [[nodiscard]] Status initialize(Role role, Bytes local_x25519_private, Bytes ticket_bytes, StreamKind expected_stream, std::uint64_t now_ms) noexcept;

  [[nodiscard]] Status seal(RecordType type, Bytes plaintext, std::vector<std::uint8_t>& record) noexcept;
  [[nodiscard]] Status open(Bytes record, std::vector<std::uint8_t>& plaintext, RecordType& type) noexcept;

  [[nodiscard]] Status make_finished(std::vector<std::uint8_t>& record) noexcept;
  [[nodiscard]] bool established() const noexcept
  {
    return sent_finished_ && received_finished_;
  }
  void wipe() noexcept;

private:
  [[nodiscard]] Status derive_material(Bytes ticket_unsigned, Bytes shared_secret) noexcept;
  [[nodiscard]] Status nonce_for(const std::array<std::uint8_t, kNonceBytes>& base, std::uint64_t sequence, std::array<std::uint8_t, kNonceBytes>& nonce) const noexcept;
  [[nodiscard]] Status finished(std::array<std::uint8_t, 32>& output) const noexcept;

  bool initialized_{false};
  bool sent_finished_{false};
  bool received_finished_{false};
  Role role_{Role::Host};
  std::uint64_t send_sequence_{0};
  bool receive_initialized_{false};
  std::uint64_t receive_highest_{0};
  std::uint64_t receive_seen_{0};
  std::array<std::uint8_t, kSessionIdBytes> session_id_{};
  std::array<std::uint8_t, 64> master_{};
  std::array<std::uint8_t, kKeyBytes> send_key_{};
  std::array<std::uint8_t, kKeyBytes> receive_key_{};
  std::array<std::uint8_t, kNonceBytes> send_nonce_base_{};
  std::array<std::uint8_t, kNonceBytes> receive_nonce_base_{};
  std::array<std::uint8_t, kTicketUnsignedBytes> ticket_unsigned_{};
};

} // namespace cr::secure_channel::v2
