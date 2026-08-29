#include "source/RtmpServer.h"
#include "util/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>
namespace cr::transport
{

namespace
{

// --- Winsock init — ref-counted across this TU. ----------------------------
std::atomic<bool> g_wsa_inited{false};
void ensure_wsa()
{
  bool expected = false;
  if (g_wsa_inited.compare_exchange_strong(expected, true))
  {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  }
}

// --- recv_all / send_all on a blocking socket ------------------------------
bool recv_all(SOCKET s, void* out, size_t n, std::atomic<bool>& stop_flag)
{
  uint8_t* p = (uint8_t*) out;
  while (n > 0)
  {
    if (stop_flag.load(std::memory_order_relaxed))
      return false;
    int r = ::recv(s, (char*) p, (int) std::min<size_t>(n, 1 << 20), 0);
    if (r <= 0)
      return false;
    p += r;
    n -= (size_t) r;
  }
  return true;
}
bool send_all(SOCKET s, const void* buf, size_t n)
{
  const uint8_t* p = (const uint8_t*) buf;
  while (n > 0)
  {
    int w = ::send(s, (const char*) p, (int) std::min<size_t>(n, 1 << 20), 0);
    if (w <= 0)
      return false;
    p += w;
    n -= (size_t) w;
  }
  return true;
}

// --- Big-endian read helpers (RTMP + FLV are all BE) -----------------------
uint32_t be24(const uint8_t* p)
{
  return (p[0] << 16) | (p[1] << 8) | p[2];
}
uint32_t be32(const uint8_t* p)
{
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
void put_be24(uint8_t* p, uint32_t v)
{
  p[0] = (v >> 16) & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = v & 0xff;
}
void put_be32(uint8_t* p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

// --- AMF0 minimal reader ---------------------------------------------------
// We only parse enough to extract command name + transaction ID + object
// properties we care about (app, objectEncoding). Anything we can't parse
// is skipped over without erroring so OBS stays happy.
struct Amf0Reader
{
  const uint8_t* p;
  const uint8_t* end;

  bool eof() const noexcept
  {
    return p >= end;
  }
  bool has(size_t n) const noexcept
  {
    return (size_t) (end - p) >= n;
  }

  uint8_t peek_type()
  {
    return eof() ? 0xff : *p;
  }

  bool read_double(double& out)
  {
    if (!has(9) || *p != 0x00)
      return false;
    ++p;
    uint8_t be[8];
    std::memcpy(be, p, 8);
    p += 8;
    // Big-endian IEEE754 → host double
    uint8_t le[8];
    for (int i = 0; i < 8; ++i)
      le[i] = be[7 - i];
    std::memcpy(&out, le, 8);
    return true;
  }
  bool read_bool(bool& out)
  {
    if (!has(2) || *p != 0x01)
      return false;
    ++p;
    out = (*p++) != 0;
    return true;
  }
  bool read_string(std::string& out)
  {
    if (!has(3) || *p != 0x02)
      return false;
    ++p;
    uint16_t n = ((uint16_t) p[0] << 8) | p[1];
    p += 2;
    if (!has(n))
      return false;
    out.assign((const char*) p, n);
    p += n;
    return true;
  }
  bool skip_value();

  // Skip object / ecma-array until 00 00 09 terminator.
  bool skip_object_body()
  {
    while (!eof())
    {
      if (has(3) && p[0] == 0 && p[1] == 0 && p[2] == 0x09)
      {
        p += 3;
        return true;
      }
      if (!has(2))
        return false;
      uint16_t klen = ((uint16_t) p[0] << 8) | p[1];
      p += 2;
      if (!has(klen))
        return false;
      p += klen;
      if (!skip_value())
        return false;
    }
    return false;
  }
};

bool Amf0Reader::skip_value()
{
  if (eof())
    return false;
  uint8_t t = *p++;
  switch (t)
  {
  case 0x00:
    if (!has(8))
      return false;
    p += 8;
    return true; // Number
  case 0x01:
    if (!has(1))
      return false;
    p += 1;
    return true; // Boolean
  case 0x02:
  { // String
    if (!has(2))
      return false;
    uint16_t n = ((uint16_t) p[0] << 8) | p[1];
    p += 2;
    if (!has(n))
      return false;
    p += n;
    return true;
  }
  case 0x03:
    return skip_object_body(); // Object
  case 0x05:
    return true; // Null
  case 0x06:
    return true; // Undefined
  case 0x08:
  { // ECMA array
    if (!has(4))
      return false;
    p += 4;
    return skip_object_body();
  }
  case 0x0a:
  { // Strict array
    if (!has(4))
      return false;
    uint32_t n = be32(p);
    p += 4;
    for (uint32_t i = 0; i < n; ++i)
      if (!skip_value())
        return false;
    return true;
  }
  default:
    // Unknown type — bail. OBS mostly sends types 0/1/2/3/5/8.
    return false;
  }
}

// --- AMF0 writer for the 3-4 responses we need to send back ---------------
void amf_put_string(std::vector<uint8_t>& out, const std::string& s)
{
  out.push_back(0x02);
  out.push_back((s.size() >> 8) & 0xff);
  out.push_back(s.size() & 0xff);
  out.insert(out.end(), s.begin(), s.end());
}
void amf_put_number(std::vector<uint8_t>& out, double v)
{
  out.push_back(0x00);
  uint8_t le[8];
  std::memcpy(le, &v, 8);
  for (int i = 7; i >= 0; --i)
    out.push_back(le[i]); // to BE
}
void amf_put_null(std::vector<uint8_t>& out)
{
  out.push_back(0x05);
}
void amf_put_obj_key(std::vector<uint8_t>& out, const std::string& k)
{
  out.push_back((k.size() >> 8) & 0xff);
  out.push_back(k.size() & 0xff);
  out.insert(out.end(), k.begin(), k.end());
}
void amf_put_obj_end(std::vector<uint8_t>& out)
{
  out.push_back(0x00);
  out.push_back(0x00);
  out.push_back(0x09);
}

// --- Build a single-chunk RTMP message, fmt=0 header. Chunks over 128
// bytes get split into 128-byte pieces with fmt=3 continuation headers.
void build_rtmp_message(std::vector<uint8_t>& out, uint8_t cs_id, uint32_t timestamp, uint8_t msg_type, uint32_t msg_stream_id, const uint8_t* body, size_t body_len, uint32_t chunk_size)
{
  // fmt=0 basic header (assume cs_id fits in 6 bits).
  out.push_back((0 << 6) | (cs_id & 0x3f));
  // 11-byte message header.
  uint8_t hdr[11];
  put_be24(hdr + 0, timestamp);
  put_be24(hdr + 3, (uint32_t) body_len);
  hdr[6] = msg_type;
  hdr[7] = msg_stream_id & 0xff; // LE
  hdr[8] = (msg_stream_id >> 8) & 0xff;
  hdr[9] = (msg_stream_id >> 16) & 0xff;
  hdr[10] = (msg_stream_id >> 24) & 0xff;
  out.insert(out.end(), hdr, hdr + 11);

  size_t pos = 0;
  bool first = true;
  while (pos < body_len)
  {
    if (!first)
    {
      // fmt=3 continuation header — just the basic byte.
      out.push_back((3 << 6) | (cs_id & 0x3f));
    }
    size_t take = std::min((size_t) chunk_size, body_len - pos);
    out.insert(out.end(), body + pos, body + pos + take);
    pos += take;
    first = false;
  }
}

// Extract SPS/PPS out of AVCDecoderConfigurationRecord and emit each as
// an Annex-B NALU (with 00 00 00 01 prefix) into `out`.
void avc_config_to_annexb(const uint8_t* data, size_t len, std::vector<uint8_t>& out)
{
  if (len < 7)
    return;
  size_t p = 5; // skip version, profile, compat, level, lengthSizeMinusOne
  uint8_t num_sps = data[p++] & 0x1f;
  for (int i = 0; i < num_sps && p + 2 <= len; ++i)
  {
    uint16_t n = ((uint16_t) data[p] << 8) | data[p + 1];
    p += 2;
    if (p + n > len)
      return;
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), data + p, data + p + n);
    p += n;
  }
  if (p >= len)
    return;
  uint8_t num_pps = data[p++];
  for (int i = 0; i < num_pps && p + 2 <= len; ++i)
  {
    uint16_t n = ((uint16_t) data[p] << 8) | data[p + 1];
    p += 2;
    if (p + n > len)
      return;
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), data + p, data + p + n);
    p += n;
  }
}

// Convert AVCC length-prefixed NALUs → Annex-B in place.
void avcc_to_annexb(const uint8_t* data, size_t len, std::vector<uint8_t>& out)
{
  size_t i = 0;
  while (i + 4 <= len)
  {
    uint32_t n = be32(data + i);
    i += 4;
    if (n == 0 || i + n > len)
      break;
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), data + i, data + i + n);
    i += n;
  }
}

} // namespace

// RtmpServer lifecycle

RtmpServer::~RtmpServer()
{
  stop();
}

bool RtmpServer::start(int port, MetaCb on_meta, NaluCb on_nalu, ConnCb on_conn, VideoCb on_video, AacCsdCb on_aac_csd, AacFrameCb on_aac_frame)
{
  if (running_.load())
    return false;
  ensure_wsa();

  port_ = port;
  on_meta_ = std::move(on_meta);
  on_nalu_ = std::move(on_nalu);
  on_conn_ = std::move(on_conn);
  on_video_ = std::move(on_video);
  on_aac_csd_ = std::move(on_aac_csd);
  on_aac_frame_ = std::move(on_aac_frame);
  stop_flag_.store(false);

  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
  {
    cr::log::error("rtmp", "socket() failed");
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*) &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short) port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(s, (sockaddr*) &addr, sizeof(addr)) != 0)
  {
    cr::log::error("rtmp", "bind failed err=" + std::to_string(WSAGetLastError()));
    closesocket(s);
    return false;
  }
  if (::listen(s, 1) != 0)
  {
    cr::log::error("rtmp", "listen failed err=" + std::to_string(WSAGetLastError()));
    closesocket(s);
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    srv_sock_ = (std::uintptr_t) s;
  }
  running_.store(true);
  worker_ = std::thread([this] { accept_loop_(); });

  cr::log::info("rtmp", "listening on 127.0.0.1:" + std::to_string(port));
  return true;
}

void RtmpServer::stop()
{
  if (!running_.exchange(false))
    return;
  stop_flag_.store(true);

  // Shutdown the listen socket so accept() returns immediately.
  SOCKET s = INVALID_SOCKET;
  {
    std::lock_guard<std::mutex> lk(mu_);
    s = (SOCKET) srv_sock_;
    srv_sock_ = ~std::uintptr_t{0};
  }
  if (s != INVALID_SOCKET)
  {
    ::shutdown(s, SD_BOTH);
    closesocket(s);
  }
  if (worker_.joinable())
    worker_.join();
}

void RtmpServer::accept_loop_()
{
  while (running_.load())
  {
    SOCKET srv = INVALID_SOCKET;
    {
      std::lock_guard<std::mutex> lk(mu_);
      srv = (SOCKET) srv_sock_;
    }
    if (srv == INVALID_SOCKET)
      break;

    sockaddr_in ca{};
    int cal = sizeof(ca);
    SOCKET client = accept(srv, (sockaddr*) &ca, &cal);
    if (client == INVALID_SOCKET)
    {
      if (!running_.load())
        break;
      cr::log::warn("rtmp", "accept err=" + std::to_string(WSAGetLastError()));
      continue;
    }
    cr::log::info("rtmp", "publisher connected");
    if (on_conn_)
      on_conn_(true, "publisher connected");
    serve_client_((int) client);
    closesocket(client);
    if (on_conn_)
      on_conn_(false, "publisher gone");
    cr::log::info("rtmp", "publisher gone");
  }
}

// One-connection session: handshake, chunk protocol, FLV extraction.
bool RtmpServer::serve_client_(int client_int)
{
  SOCKET client = (SOCKET) client_int;
  BOOL no_delay = TRUE;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*) &no_delay, sizeof(no_delay));
  int recv_buf = 512 * 1024;
  setsockopt(client, SOL_SOCKET, SO_RCVBUF, (const char*) &recv_buf, sizeof(recv_buf));

  // ---- Handshake (simple, no digest) -----------------------------------
  // C0/C1 arrive concatenated; we reply S0+S1+S2 in one shot. S2 echoes
  // C1 verbatim per the "simple handshake" spec — every release of OBS
  // from the last decade accepts this.
  uint8_t c0c1[1 + 1536];
  if (!recv_all(client, c0c1, sizeof(c0c1), stop_flag_))
    return false;
  if (c0c1[0] != 0x03)
  {
    cr::log::warn("rtmp", "unexpected C0 version=0x" + std::to_string((unsigned) c0c1[0]));
    return false;
  }
  std::vector<uint8_t> s0s1s2;
  s0s1s2.reserve(1 + 1536 + 1536);
  s0s1s2.push_back(0x03);
  // S1: zero timestamp + zero + random filler. Use c1 to match `time+0+rand`
  // shape (most clients don't validate the random field).
  uint8_t s1[1536] = {0};
  // Random filler
  std::random_device rd;
  for (int i = 8; i < 1536; ++i)
    s1[i] = (uint8_t) rd();
  s0s1s2.insert(s0s1s2.end(), s1, s1 + 1536);
  // S2 = echo of C1
  s0s1s2.insert(s0s1s2.end(), c0c1 + 1, c0c1 + 1 + 1536);
  if (!send_all(client, s0s1s2.data(), s0s1s2.size()))
    return false;

  uint8_t c2[1536];
  if (!recv_all(client, c2, sizeof(c2), stop_flag_))
    return false;

  // ---- Chunk stream loop -----------------------------------------------
  uint32_t in_chunk_size = 128;
  uint32_t out_chunk_size = 4096;

  struct MsgHdr
  {
    uint32_t timestamp = 0;
    uint32_t length = 0;
    uint8_t type = 0;
    uint32_t stream_id = 0;
    bool have_delta = false;
  };
  std::map<uint32_t, MsgHdr> last_hdrs;
  std::map<uint32_t, std::vector<uint8_t>> partial;

  // Cached H.264 SPS/PPS (from seq header) — prepended to every keyframe
  // we emit to the phone so the remote decoder can start at any IDR.
  std::vector<uint8_t> sps_pps_annexb;
  RtmpStreamMeta meta{};
  bool meta_sent = false;
  bool seen_video_keyframe = false;

  // Early handshake-response messages. We bundle a bigger out chunk size
  // so later audio chunks don't show up as micro-writes.
  // Window Acknowledgement Size (type 5, body u32 BE)
  {
    uint8_t body[4];
    put_be32(body, 5'000'000);
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x05, 0, body, 4, out_chunk_size);
    if (!send_all(client, msg.data(), msg.size()))
      return false;
  }
  // Set Peer Bandwidth (type 6, body u32 BE + u8 type=2 dynamic)
  {
    uint8_t body[5];
    put_be32(body, 5'000'000);
    body[4] = 2;
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x06, 0, body, 5, out_chunk_size);
    if (!send_all(client, msg.data(), msg.size()))
      return false;
  }
  // Set Chunk Size (type 1, body = new max BE)
  {
    uint8_t body[4];
    put_be32(body, out_chunk_size);
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x01, 0, body, 4, out_chunk_size);
    if (!send_all(client, msg.data(), msg.size()))
      return false;
  }

  while (!stop_flag_.load(std::memory_order_relaxed))
  {
    // --- Basic header -------------------------------------------------
    uint8_t bh0;
    if (!recv_all(client, &bh0, 1, stop_flag_))
      break;
    uint32_t fmt = bh0 >> 6;
    uint32_t cs_id = bh0 & 0x3f;
    if (cs_id == 0)
    {
      uint8_t ext;
      if (!recv_all(client, &ext, 1, stop_flag_))
        break;
      cs_id = 64u + ext;
    }
    else if (cs_id == 1)
    {
      uint8_t ext[2];
      if (!recv_all(client, ext, 2, stop_flag_))
        break;
      cs_id = 64u + (uint32_t) ext[0] + ((uint32_t) ext[1] << 8);
    }

    // --- Message header ----------------------------------------------
    MsgHdr& last = last_hdrs[cs_id];
    uint32_t ts_field = 0;
    bool used_delta = false;

    if (fmt == 0)
    {
      uint8_t h[11];
      if (!recv_all(client, h, 11, stop_flag_))
        break;
      last.timestamp = ts_field = be24(h);
      last.length = be24(h + 3);
      last.type = h[6];
      last.stream_id = (uint32_t) h[7] | ((uint32_t) h[8] << 8) | ((uint32_t) h[9] << 16) | ((uint32_t) h[10] << 24);
      last.have_delta = false;
    }
    else if (fmt == 1)
    {
      uint8_t h[7];
      if (!recv_all(client, h, 7, stop_flag_))
        break;
      ts_field = be24(h);
      last.length = be24(h + 3);
      last.type = h[6];
      used_delta = true;
    }
    else if (fmt == 2)
    {
      uint8_t h[3];
      if (!recv_all(client, h, 3, stop_flag_))
        break;
      ts_field = be24(h);
      used_delta = true;
    }
    else
    {
      // fmt == 3: no header. If this is the FIRST chunk of a new
      // message (no partial in flight) we re-apply the saved delta.
      if (partial[cs_id].empty())
      {
        ts_field = 0; // unchanged / implicit
        used_delta = last.have_delta;
      }
    }
    // Extended timestamp
    if (ts_field == 0xffffff)
    {
      uint8_t h[4];
      if (!recv_all(client, h, 4, stop_flag_))
        break;
      ts_field = be32(h);
    }
    if (used_delta && partial[cs_id].empty())
    {
      last.timestamp += ts_field;
      last.have_delta = true;
    }

    // --- Chunk payload ------------------------------------------------
    auto& buf = partial[cs_id];
    uint32_t remaining = last.length - (uint32_t) buf.size();
    uint32_t take = std::min<uint32_t>(remaining, in_chunk_size);
    size_t old_size = buf.size();
    buf.resize(old_size + take);
    if (take > 0)
    {
      if (!recv_all(client, buf.data() + old_size, take, stop_flag_))
        break;
    }

    if (buf.size() < last.length)
      continue; // more chunks to come

    // --- Full message assembled. Dispatch. ---------------------------
    const uint8_t type = last.type;
    std::vector<uint8_t> body = std::move(buf);
    buf.clear();

    switch (type)
    {
    case 0x01:
    { // Set Chunk Size
      if (body.size() >= 4)
      {
        uint32_t ns = be32(body.data()) & 0x7fffffff;
        if (ns > 0 && ns <= (1u << 24))
          in_chunk_size = ns;
      }
      break;
    }
    case 0x03:
      break; // Acknowledgement, ignore
    case 0x04:
      break; // User Control, ignore
    case 0x05:
      break; // Window Ack Size from client, ignore
    case 0x06:
      break; // Peer Bandwidth from client, ignore

    case 0x11: // AMF3 command (just skip 1 byte prefix → AMF0)
    case 0x14:
    { // AMF0 command
      const uint8_t* p0 = body.data();
      const uint8_t* pe = body.data() + body.size();
      if (type == 0x11 && p0 < pe)
        ++p0; // skip AMF3 zero byte
      Amf0Reader r{p0, pe};
      std::string cmd;
      double tid = 0;
      if (!r.read_string(cmd) || !r.read_double(tid))
        break;

      if (cmd == "connect")
      {
        // Skip command object + optional args.
        // Respond with _result carrying server properties.
        std::vector<uint8_t> resp;
        amf_put_string(resp, "_result");
        amf_put_number(resp, tid);
        // Properties object
        resp.push_back(0x03);
        amf_put_obj_key(resp, "fmsVer");
        amf_put_string(resp, "FMS/3,5,3,888");
        amf_put_obj_key(resp, "capabilities");
        amf_put_number(resp, 31.0);
        amf_put_obj_key(resp, "mode");
        amf_put_number(resp, 1.0);
        amf_put_obj_end(resp);
        // Information object
        resp.push_back(0x03);
        amf_put_obj_key(resp, "level");
        amf_put_string(resp, "status");
        amf_put_obj_key(resp, "code");
        amf_put_string(resp, "NetConnection.Connect.Success");
        amf_put_obj_key(resp, "description");
        amf_put_string(resp, "Connection succeeded.");
        amf_put_obj_key(resp, "objectEncoding");
        amf_put_number(resp, 0.0);
        amf_put_obj_end(resp);

        std::vector<uint8_t> msg;
        build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
        if (!send_all(client, msg.data(), msg.size()))
          return false;
      }
      else if (cmd == "releaseStream" || cmd == "FCPublish" || cmd == "FCUnpublish" || cmd == "deleteStream")
      {
        // Ack with _result/null so OBS doesn't stall waiting.
        std::vector<uint8_t> resp;
        amf_put_string(resp, "_result");
        amf_put_number(resp, tid);
        amf_put_null(resp);
        amf_put_null(resp);
        std::vector<uint8_t> msg;
        build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
        if (!send_all(client, msg.data(), msg.size()))
          return false;
      }
      else if (cmd == "createStream")
      {
        std::vector<uint8_t> resp;
        amf_put_string(resp, "_result");
        amf_put_number(resp, tid);
        amf_put_null(resp);
        amf_put_number(resp, 1.0); // stream id we hand back
        std::vector<uint8_t> msg;
        build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
        if (!send_all(client, msg.data(), msg.size()))
          return false;
      }
      else if (cmd == "publish")
      {
        // Respond with onStatus/NetStream.Publish.Start on stream 1.
        std::vector<uint8_t> resp;
        amf_put_string(resp, "onStatus");
        amf_put_number(resp, 0.0);
        amf_put_null(resp);
        resp.push_back(0x03);
        amf_put_obj_key(resp, "level");
        amf_put_string(resp, "status");
        amf_put_obj_key(resp, "code");
        amf_put_string(resp, "NetStream.Publish.Start");
        amf_put_obj_key(resp, "description");
        amf_put_string(resp, "Start publishing.");
        amf_put_obj_end(resp);
        std::vector<uint8_t> msg;
        build_rtmp_message(msg, 4, 0, 0x14, 1, resp.data(), resp.size(), out_chunk_size);
        if (!send_all(client, msg.data(), msg.size()))
          return false;
      }
      break;
    }

    case 0x12: // AMF0 data (onMetaData) — optional hint
    case 0x0f:
    { // AMF3 data
      const uint8_t* p0 = body.data();
      const uint8_t* pe = body.data() + body.size();
      if (type == 0x0f && p0 < pe)
        ++p0;
      Amf0Reader r{p0, pe};
      std::string s;
      if (r.read_string(s) && (s == "@setDataFrame" || s == "onMetaData"))
      {
        std::string inner;
        if (s == "@setDataFrame")
          r.read_string(inner);
        // Remaining is an ECMA/object with properties.
        while (!r.eof())
        {
          uint8_t t = r.peek_type();
          if (t == 0x08)
          { // ECMA array
            ++r.p;
            r.p += 4; // type + u32 assoc count
          }
          else if (t == 0x03)
          { // Object
            ++r.p;
          }
          else
            break;

          // Walk keys.
          while (!r.eof())
          {
            if (r.has(3) && r.p[0] == 0 && r.p[1] == 0 && r.p[2] == 9)
            {
              r.p += 3;
              break;
            }
            if (!r.has(2))
              break;
            uint16_t klen = ((uint16_t) r.p[0] << 8) | r.p[1];
            r.p += 2;
            if (!r.has(klen))
              break;
            std::string key((const char*) r.p, klen);
            r.p += klen;

            if (r.peek_type() == 0x00)
            {
              double d;
              if (!r.read_double(d))
                break;
              if (key == "width")
                meta.width = (int) d;
              else if (key == "height")
                meta.height = (int) d;
              else if (key == "framerate" || key == "fps")
                meta.framerate = d;
              else if (key == "videodatarate")
                meta.bitrate_kbps = (int) d;
            }
            else
            {
              if (!r.skip_value())
                break;
            }
          }
          break;
        }
        if (!meta_sent && on_meta_)
        {
          on_meta_(meta);
          meta_sent = true;
        }
      }
      break;
    }

    case 0x08:
    { // Audio
      if (body.size() < 2)
        break;
      // FLV AudioTagHeader byte 0:
      //   bits 7..4 = SoundFormat (10 = AAC)
      //   bits 3..2 = SoundRate   (informational; AAC carries rate
      //                            in its AudioSpecificConfig)
      //   bit  1    = SoundSize   (0=8-bit, 1=16-bit)
      //   bit  0    = SoundType   (0=mono, 1=stereo)
      const uint8_t flv0 = body[0];
      const uint8_t sound_fmt = flv0 >> 4;
      if (sound_fmt != 10)
        break; // only AAC handled
      const uint8_t aac_pkt_type = body[1];
      const uint8_t* payload = body.data() + 2;
      size_t pay_len = body.size() - 2;
      if (aac_pkt_type == 0)
      { // AudioSpecificConfig
        if (on_aac_csd_)
          on_aac_csd_(payload, pay_len);
      }
      else if (aac_pkt_type == 1)
      { // raw AAC frame
        int64_t pts_us = (int64_t) last.timestamp * 1000;
        if (on_aac_frame_)
          on_aac_frame_(payload, pay_len, pts_us);
      }
      break;
    }

    case 0x09:
    { // Video
      if (body.size() < 2)
        break;
      uint8_t frame_type = (body[0] >> 4) & 0x0f;
      uint8_t codec_id = body[0] & 0x0f;
      RtmpVideoEvent video{};
      video.frame_type = frame_type;
      video.codec_id = codec_id;
      video.keyframe = (frame_type == 1);
      if (codec_id == 7 && body.size() >= 5)
      {
        video.avc_packet_type = body[1];
        video.payload_bytes = body.size() - 5;
      }
      else
      {
        video.payload_bytes = body.size() > 1 ? body.size() - 1 : 0;
      }
      if (on_video_)
      {
        on_video_(video);
      }
      if (codec_id != 7)
        break; // only H.264 / AVC
      if (body.size() < 5)
        break;

      uint8_t avc_packet_type = body[1];
      int32_t cts_ms = (int32_t) ((body[2] << 16) | (body[3] << 8) | body[4]);
      if (cts_ms & 0x800000)
        cts_ms |= ~0xffffff;
      int64_t dts_ms = (int64_t) last.timestamp;
      int64_t pts_us = (dts_ms + cts_ms) * 1000;

      const uint8_t* payload = body.data() + 5;
      size_t pay_len = body.size() - 5;

      if (avc_packet_type == 0)
      {
        sps_pps_annexb.clear();
        seen_video_keyframe = false;
        avc_config_to_annexb(payload, pay_len, sps_pps_annexb);
        // Forward the CSD as a keyframe-like NALU blob so the
        // receiver can init its decoder right away.
        if (on_nalu_ && !sps_pps_annexb.empty())
        {
          on_nalu_(sps_pps_annexb.data(), sps_pps_annexb.size(), pts_us,
                   /*is_keyframe*/ true);
        }
      }
      else if (avc_packet_type == 1)
      {
        std::vector<uint8_t> annexb;
        // Prepend cached SPS/PPS on every keyframe so a listener
        // that joined mid-stream can still sync.
        const bool is_key = (frame_type == 1);
        if (!is_key && !seen_video_keyframe)
        {
          break;
        }
        if (is_key && !sps_pps_annexb.empty())
        {
          annexb.insert(annexb.end(), sps_pps_annexb.begin(), sps_pps_annexb.end());
        }
        avcc_to_annexb(payload, pay_len, annexb);
        if (on_nalu_ && !annexb.empty())
        {
          if (is_key)
          {
            seen_video_keyframe = true;
          }
          on_nalu_(annexb.data(), annexb.size(), pts_us, is_key);
        }
      }
      // avc_packet_type 2 = end-of-sequence, ignore.
      break;
    }

    default:
      break;
    }
  }

  return true;
}

} // namespace cr::transport
