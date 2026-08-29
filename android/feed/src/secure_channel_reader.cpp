#include "secure_channel_reader.h"

#include <cerrno>
#include <array>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <android/log.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C"
{
#include "secure_channel/vendor/monocypher/monocypher.h"
}

namespace cr::feed
{
namespace
{

constexpr std::uint32_t kPhoneHelloMagic = 0x324c4850u; // "PHL2"
constexpr std::size_t kPhoneHelloBytes = 64;

void put_le32(std::uint8_t* p, std::uint32_t value) noexcept
{
  for (int i = 0; i != 4; ++i)
    p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t get_le32(const std::uint8_t* p) noexcept
{
  return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

bool fill_random(void* output, std::size_t bytes) noexcept
{
  // API 21 has no getrandom(); /dev/urandom is the portable CSPRNG source.
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  auto* p = static_cast<std::uint8_t*>(output);
  bool ok = true;
  while (bytes)
  {
    const ssize_t n = ::read(fd, p, bytes);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
    {
      ok = false;
      break;
    }
    p += n;
    bytes -= static_cast<std::size_t>(n);
  }
  ::close(fd);
  return ok;
}

bool send_all(int fd, const void* data, std::size_t bytes) noexcept
{
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (bytes)
  {
    const ssize_t n = ::send(fd, p, bytes, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    p += n;
    bytes -= static_cast<std::size_t>(n);
  }
  return true;
}
bool recv_all(int fd, void* data, std::size_t bytes) noexcept
{
  auto* p = static_cast<std::uint8_t*>(data);
  while (bytes)
  {
    const ssize_t n = ::recv(fd, p, bytes, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    p += n;
    bytes -= static_cast<std::size_t>(n);
  }
  return true;
}

std::uint64_t unix_ms() noexcept
{
  timespec ts{};
  if (::clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0)
    return 0;
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000 + static_cast<std::uint64_t>(ts.tv_nsec / 1000000);
}
} // namespace

bool SecureChannelReader::handshake(int socket_fd, cr::secure_channel::v2::StreamKind stream) noexcept
{
  wipe();
  socket_fd_ = socket_fd;
  std::array<std::uint8_t, 32> private_key{};
  std::array<std::uint8_t, 32> public_key{};
  std::array<std::uint8_t, 24> nonce{};
  std::array<std::uint8_t, kPhoneHelloBytes> hello{};
  std::array<std::uint8_t, cr::secure_channel::v2::kTicketBytes> ticket{};
  bool ok = fill_random(private_key.data(), private_key.size()) && fill_random(nonce.data(), nonce.size());
  if (ok)
    crypto_x25519_public_key(public_key.data(), private_key.data());
  if (ok)
  {
    put_le32(hello.data(), kPhoneHelloMagic);
    hello[4] = cr::secure_channel::v2::kVersion;
    hello[5] = static_cast<std::uint8_t>(stream);
    std::memcpy(hello.data() + 8, public_key.data(), public_key.size());
    std::memcpy(hello.data() + 40, nonce.data(), nonce.size());
    ok = send_all(socket_fd_, hello.data(), hello.size()) && recv_all(socket_fd_, ticket.data(), ticket.size()) && session_.initialize(cr::secure_channel::v2::Role::Phone, {private_key.data(), private_key.size()}, {ticket.data(), ticket.size()}, stream, unix_ms());
  }
  crypto_wipe(private_key.data(), private_key.size());
  if (!ok)
  {
    wipe();
    return false;
  }

  std::vector<std::uint8_t> host_finished;
  if (!receive_record_(host_finished))
  {
    wipe();
    return false;
  }
  std::vector<std::uint8_t> ignored;
  cr::secure_channel::v2::RecordType type{};
  if (!session_.open({host_finished.data(), host_finished.size()}, ignored, type) || type != cr::secure_channel::v2::RecordType::Finished)
  {
    wipe();
    return false;
  }
  std::vector<std::uint8_t> phone_finished;
  if (!session_.make_finished(phone_finished) || !send_all(socket_fd_, phone_finished.data(), phone_finished.size()) || !session_.established())
  {
    wipe();
    return false;
  }
  return true;
}

bool SecureChannelReader::receive_record_(std::vector<std::uint8_t>& record) noexcept
{
  std::array<std::uint8_t, cr::secure_channel::v2::kRecordHeaderBytes> header{};
  if (socket_fd_ < 0 || !recv_all(socket_fd_, header.data(), header.size()))
    return false;
  const std::uint32_t ciphertext = get_le32(header.data() + 32);
  if (ciphertext > cr::secure_channel::v2::kMaxCiphertextBytes)
    return false;
  record.assign(header.begin(), header.end());
  record.resize(record.size() + ciphertext + cr::secure_channel::v2::kTagBytes);
  return recv_all(socket_fd_, record.data() + header.size(), record.size() - header.size());
}

bool SecureChannelReader::read_exact(void* output, std::size_t bytes) noexcept
{
  auto* out = static_cast<std::uint8_t*>(output);
  while (bytes)
  {
    if (plaintext_offset_ == plaintext_.size())
    {
      std::vector<std::uint8_t> record;
      std::vector<std::uint8_t> plaintext;
      cr::secure_channel::v2::RecordType type{};
      if (!receive_record_(record) || !session_.open({record.data(), record.size()}, plaintext, type) || type != cr::secure_channel::v2::RecordType::Application || plaintext.empty())
      {
        wipe();
        return false;
      }
      plaintext_ = std::move(plaintext);
      plaintext_offset_ = 0;
    }
    const std::size_t available = plaintext_.size() - plaintext_offset_;
    const std::size_t take = available < bytes ? available : bytes;
    std::memcpy(out, plaintext_.data() + plaintext_offset_, take);
    plaintext_offset_ += take;
    out += take;
    bytes -= take;
  }
  return true;
}

void SecureChannelReader::wipe() noexcept
{
  if (!plaintext_.empty())
    crypto_wipe(plaintext_.data(), plaintext_.size());
  plaintext_.clear();
  plaintext_offset_ = 0;
  socket_fd_ = -1;
  session_.wipe();
}

} // namespace cr::feed
