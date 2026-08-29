#pragma once

#include "secure_channel/SecureSession.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cr::feed
{

// Authenticates the phone-first SCT2 handshake and exposes only plaintext
// from verified Application records. Any malformed/bootstrap/record failure
// is terminal for the owning TCP connection.
class SecureChannelReader
{
public:
  bool handshake(int socket_fd, cr::secure_channel::v2::StreamKind stream) noexcept;
  bool read_exact(void* output, std::size_t bytes) noexcept;
  void wipe() noexcept;

private:
  bool receive_record_(std::vector<std::uint8_t>& record) noexcept;

  int socket_fd_{-1};
  cr::secure_channel::v2::SecureSession session_{};
  std::vector<std::uint8_t> plaintext_{};
  std::size_t plaintext_offset_{0};
};

} // namespace cr::feed
