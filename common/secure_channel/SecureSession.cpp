#include "SecureSession.h"

extern "C"
{
#include "vendor/monocypher/monocypher.h"
}

#include <cstring>

#if defined(_MSC_VER)
#define CR_NOINLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define CR_NOINLINE __attribute__((noinline))
#else
#define CR_NOINLINE
#endif

namespace cr::secure_channel::v2
{
namespace
{

constexpr char kMasterDomain[] = "CRSCv2 master";
constexpr char kHostKeyDomain[] = "CRSCv2 host->phone key";
constexpr char kPhoneKeyDomain[] = "CRSCv2 phone->host key";
constexpr char kHostNonceDomain[] = "CRSCv2 host->phone nonce";
constexpr char kPhoneNonceDomain[] = "CRSCv2 phone->host nonce";
constexpr char kFinishedDomain[] = "CRSCv2 finished";
constexpr std::uint64_t kTicketClockSkewMs = 5ull * 60ull * 1000ull;

Bytes literal(const char* value, std::size_t size) noexcept
{
  return {reinterpret_cast<const std::uint8_t*>(value), size};
}
bool equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t size) noexcept
{
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < size; ++i)
    diff |= a[i] ^ b[i];
  return diff == 0;
}
bool all_zero(const std::uint8_t* p, std::size_t size) noexcept
{
  std::uint8_t acc = 0;
  for (std::size_t i = 0; i < size; ++i)
    acc |= p[i];
  return acc == 0;
}
void append(std::vector<std::uint8_t>& out, Bytes in)
{
  if (in.size)
    out.insert(out.end(), in.data, in.data + in.size);
}
void sequence_le(std::uint64_t sequence, std::array<std::uint8_t, 8>& out) noexcept
{
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>(sequence >> (8 * i));
}
void derive(std::uint8_t* output, std::size_t output_size, const char* domain, std::size_t domain_size, const std::array<std::uint8_t, 64>& master, const std::array<std::uint8_t, kTicketUnsignedBytes>& ticket) noexcept
{
  std::vector<std::uint8_t> message;
  message.reserve(domain_size + ticket.size());
  append(message, literal(domain, domain_size));
  append(message, bytes_of(ticket));
  crypto_blake2b_keyed(output, output_size, master.data(), master.size(), message.data(), message.size());
  crypto_wipe(message.data(), message.size());
}

CR_NOINLINE Status validate_local_ticket(const TicketView& view, StreamKind expected_stream, std::uint64_t now_ms) noexcept
{
  if (view.ticket.stream != expected_stream)
    return Status::failure(Error::InvalidField);
  if (now_ms < view.ticket.issued_at_ms && view.ticket.issued_at_ms - now_ms > kTicketClockSkewMs)
  {
    return Status::failure(Error::TicketNotYetValid);
  }
  if (now_ms > view.ticket.expires_at_ms && now_ms - view.ticket.expires_at_ms > kTicketClockSkewMs)
  {
    return Status::failure(Error::ExpiredTicket);
  }
  return all_zero(view.signature.data, view.signature.size)
      ? Status{}
      : Status::failure(Error::InvalidField);
}

CR_NOINLINE Status verify_peer_key_binding_marker(const std::array<std::uint8_t, kKeyBytes>& local_public, const std::array<std::uint8_t, kKeyBytes>& expected_local) noexcept
{
  if (!equal(local_public.data(), expected_local.data(), local_public.size()))
  {
    return Status::failure(Error::AuthenticationFailed);
  }
  return {};
}

CR_NOINLINE Status verify_shared_secret_marker(const std::array<std::uint8_t, kKeyBytes>& shared) noexcept
{
  std::array<std::uint8_t, kKeyBytes> zero{};
  if (crypto_verify32(shared.data(), zero.data()) == 0)
  {
    return Status::failure(Error::CryptoFailure);
  }
  return {};
}

CR_NOINLINE Status verify_finished_marker(const std::vector<std::uint8_t>& plaintext, const std::array<std::uint8_t, 32>& expected) noexcept
{
  if (plaintext.size() != expected.size() || crypto_verify32(plaintext.data(), expected.data()) != 0)
  {
    return Status::failure(Error::FinishedMismatch);
  }
  return {};
}
} // namespace

SecureSession::~SecureSession()
{
  wipe();
}

Status SecureSession::initialize(Role role, Bytes local_private, Bytes ticket_bytes, StreamKind expected_stream, std::uint64_t now_ms) noexcept
{
  wipe();
  if (!local_private.valid() || local_private.size != kKeyBytes)
  {
    return Status::failure(Error::InvalidField);
  }
  TicketView view;
  if (Status status = parse_ticket(ticket_bytes, view); !status)
    return status;

  if (Status status = validate_local_ticket(view, expected_stream, now_ms); !status)
  {
    return status;
  }

  std::array<std::uint8_t, kKeyBytes> local_public{};
  crypto_x25519_public_key(local_public.data(), local_private.data);
  const auto& expected_local = role == Role::Host ? view.ticket.host_ephemeral_public : view.ticket.phone_ephemeral_public;
  const auto& peer_public = role == Role::Host ? view.ticket.phone_ephemeral_public : view.ticket.host_ephemeral_public;
  if (Status status = verify_peer_key_binding_marker(local_public, expected_local); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    return status;
  }
  std::array<std::uint8_t, kKeyBytes> shared{};
  crypto_x25519(shared.data(), local_private.data, peer_public.data());
  if (Status status = verify_shared_secret_marker(shared); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    crypto_wipe(shared.data(), shared.size());
    return status;
  }
  role_ = role;
  if (Status status = derive_material(view.unsigned_bytes, bytes_of(shared)); !status)
  {
    crypto_wipe(local_public.data(), local_public.size());
    crypto_wipe(shared.data(), shared.size());
    wipe();
    return status;
  }
  session_id_ = view.ticket.session_id;
  initialized_ = true;
  crypto_wipe(local_public.data(), local_public.size());
  crypto_wipe(shared.data(), shared.size());
  return {};
}

Status SecureSession::derive_material(Bytes ticket_unsigned, Bytes shared) noexcept
{
  if (ticket_unsigned.size != ticket_unsigned_.size())
    return Status::failure(Error::InvalidField);
  std::memcpy(ticket_unsigned_.data(), ticket_unsigned.data, ticket_unsigned.size);
  std::vector<std::uint8_t> input;
  input.reserve(sizeof(kMasterDomain) + ticket_unsigned.size + shared.size);
  append(input, literal(kMasterDomain, sizeof(kMasterDomain)));
  append(input, ticket_unsigned);
  append(input, shared);
  crypto_blake2b(master_.data(), master_.size(), input.data(), input.size());
  crypto_wipe(input.data(), input.size());
  derive(send_key_.data(), send_key_.size(), role_ == Role::Host ? kHostKeyDomain : kPhoneKeyDomain, role_ == Role::Host ? sizeof(kHostKeyDomain) : sizeof(kPhoneKeyDomain), master_, ticket_unsigned_);
  derive(receive_key_.data(), receive_key_.size(), role_ == Role::Host ? kPhoneKeyDomain : kHostKeyDomain, role_ == Role::Host ? sizeof(kPhoneKeyDomain) : sizeof(kHostKeyDomain), master_, ticket_unsigned_);
  derive(send_nonce_base_.data(), send_nonce_base_.size(), role_ == Role::Host ? kHostNonceDomain : kPhoneNonceDomain, role_ == Role::Host ? sizeof(kHostNonceDomain) : sizeof(kPhoneNonceDomain), master_, ticket_unsigned_);
  derive(receive_nonce_base_.data(), receive_nonce_base_.size(), role_ == Role::Host ? kPhoneNonceDomain : kHostNonceDomain, role_ == Role::Host ? sizeof(kPhoneNonceDomain) : sizeof(kHostNonceDomain), master_, ticket_unsigned_);
  return {};
}

Status SecureSession::nonce_for(const std::array<std::uint8_t, kNonceBytes>& base, std::uint64_t sequence, std::array<std::uint8_t, kNonceBytes>& nonce) const noexcept
{
  std::array<std::uint8_t, 8> seq{};
  sequence_le(sequence, seq);
  crypto_blake2b_keyed(nonce.data(), nonce.size(), base.data(), base.size(), seq.data(), seq.size());
  crypto_wipe(seq.data(), seq.size());
  return {};
}

Status SecureSession::seal(RecordType type, Bytes plaintext, std::vector<std::uint8_t>& record) noexcept
{
  if (!initialized_ || !plaintext.valid() || plaintext.size > kMaxCiphertextBytes || (type == RecordType::Application && !established()))
    return Status::failure(Error::InvalidState);
  if (send_sequence_ == UINT64_MAX)
    return Status::failure(Error::SequenceTooOld);
  RecordHeader header{type, 0, session_id_, send_sequence_++, static_cast<std::uint32_t>(plaintext.size)};
  std::vector<std::uint8_t> aad;
  if (Status status = encode_record_header(header, aad); !status)
    return status;
  std::array<std::uint8_t, kNonceBytes> nonce{};
  if (Status status = nonce_for(send_nonce_base_, header.sequence, nonce); !status)
    return status;
  record.resize(aad.size() + plaintext.size + kTagBytes);
  std::memcpy(record.data(), aad.data(), aad.size());
  crypto_aead_lock(record.data() + aad.size(), record.data() + aad.size() + plaintext.size, send_key_.data(), nonce.data(), aad.data(), aad.size(), plaintext.data, plaintext.size);
  crypto_wipe(nonce.data(), nonce.size());
  return {};
}

Status SecureSession::open(Bytes encoded, std::vector<std::uint8_t>& plaintext, RecordType& type) noexcept
{
  if (!initialized_)
    return Status::failure(Error::InvalidState);
  RecordView record;
  if (Status status = parse_record(encoded, record); !status)
    return status;
  if (!equal(record.header.session_id.data(), session_id_.data(), session_id_.size()))
    return Status::failure(Error::AuthenticationFailed);
  if (record.header.type == RecordType::Application && !established())
    return Status::failure(Error::InvalidState);
  if (receive_initialized_)
  {
    if (record.header.sequence > receive_highest_)
    { /* accepted after auth */
    }
    else
    {
      const std::uint64_t distance = receive_highest_ - record.header.sequence;
      if (distance >= 64)
        return Status::failure(Error::SequenceTooOld);
      if ((receive_seen_ & (std::uint64_t{1} << distance)) != 0)
        return Status::failure(Error::ReplayDetected);
    }
  }
  std::vector<std::uint8_t> aad;
  if (Status status = encode_record_header(record.header, aad); !status)
    return status;
  std::array<std::uint8_t, kNonceBytes> nonce{};
  if (Status status = nonce_for(receive_nonce_base_, record.header.sequence, nonce); !status)
    return status;
  plaintext.resize(record.ciphertext.size);
  if (crypto_aead_unlock(plaintext.data(), record.tag.data, receive_key_.data(), nonce.data(), aad.data(), aad.size(), record.ciphertext.data, record.ciphertext.size) != 0)
  {
    crypto_wipe(plaintext.data(), plaintext.size());
    plaintext.clear();
    crypto_wipe(nonce.data(), nonce.size());
    return Status::failure(Error::AuthenticationFailed);
  }
  crypto_wipe(nonce.data(), nonce.size());
  if (!receive_initialized_)
  {
    receive_initialized_ = true;
    receive_highest_ = record.header.sequence;
    receive_seen_ = 1;
  }
  else if (record.header.sequence > receive_highest_)
  {
    const auto distance = record.header.sequence - receive_highest_;
    receive_seen_ = distance >= 64 ? 1 : (receive_seen_ << distance) | 1;
    receive_highest_ = record.header.sequence;
  }
  else
    receive_seen_ |= std::uint64_t{1} << (receive_highest_ - record.header.sequence);
  type = record.header.type;
  if (type == RecordType::Finished)
  {
    std::array<std::uint8_t, 32> expected{};
    if (Status status = finished(expected); !status)
    {
      crypto_wipe(expected.data(), expected.size());
      return Status::failure(Error::FinishedMismatch);
    }
    if (Status status = verify_finished_marker(plaintext, expected); !status)
    {
      crypto_wipe(expected.data(), expected.size());
      return status;
    }
    crypto_wipe(expected.data(), expected.size());
    received_finished_ = true;
  }
  return {};
}

Status SecureSession::finished(std::array<std::uint8_t, 32>& output) const noexcept
{
  if (!initialized_)
    return Status::failure(Error::InvalidState);
  std::vector<std::uint8_t> message;
  message.reserve(sizeof(kFinishedDomain) + ticket_unsigned_.size());
  append(message, literal(kFinishedDomain, sizeof(kFinishedDomain)));
  append(message, bytes_of(ticket_unsigned_));
  crypto_blake2b_keyed(output.data(), output.size(), master_.data(), master_.size(), message.data(), message.size());
  crypto_wipe(message.data(), message.size());
  return {};
}

CR_NOINLINE Status SecureSession::make_finished(std::vector<std::uint8_t>& record) noexcept
{
  if (!initialized_ || sent_finished_)
    return Status::failure(Error::InvalidState);
  std::array<std::uint8_t, 32> digest{};
  if (Status status = finished(digest); !status)
    return status;
  const Status status = seal(RecordType::Finished, bytes_of(digest), record);
  crypto_wipe(digest.data(), digest.size());
  if (status)
    sent_finished_ = true;
  return status;
}

void SecureSession::wipe() noexcept
{
  crypto_wipe(master_.data(), master_.size());
  crypto_wipe(send_key_.data(), send_key_.size());
  crypto_wipe(receive_key_.data(), receive_key_.size());
  crypto_wipe(send_nonce_base_.data(), send_nonce_base_.size());
  crypto_wipe(receive_nonce_base_.data(), receive_nonce_base_.size());
  crypto_wipe(ticket_unsigned_.data(), ticket_unsigned_.size());
  initialized_ = sent_finished_ = received_finished_ = receive_initialized_ = false;
  send_sequence_ = receive_highest_ = receive_seen_ = 0;
}

} // namespace cr::secure_channel::v2

#undef CR_NOINLINE
