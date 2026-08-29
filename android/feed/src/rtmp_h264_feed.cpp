// RTMP H.264 feed mode (sprint B).
// Why this exists:
//   The legacy path was OBS → PC RtmpServer → PC parses chunks → PC packs
//   each NAL into a 16-byte-prefixed TCP frame → cr_feed_proc on phone
//   reads frames → MediaCodec.
//   The PC-side parser was an extra place latency (and bugs) could land.
//   This file moves the RTMP listener onto the phone itself: OBS pushes
//   straight to 127.0.0.1:1935, adb forward bridges the PC's loopback to
//   the phone's loopback, and we feed Annex-B NALs directly into the
//   shared H.264 decode pipe. One TCP connection less, one binary parser
//   less.
// Wire compatibility:
//   We accept the same simple RTMP 1.0 handshake + chunk-stream + AMF0
//   command set the host-side server already supported:
//     connect / releaseStream / FCPublish / createStream / publish
//   And the same FLV-tag types:
//     0x09 video  (AVC sequence header + coded frames)
//     0x12 data   (onMetaData / @setDataFrame for resolution/fps hint)
//   Audio (0x08) is intentionally ignored — the audio side-channel still
//   uses tcp_pcm via cr_feed_run_tcp_pcm. AAC support could be added by
//   forwarding tags into a phone-side AAC decoder, but that's out of
//   scope for the sprint that's just removing the PC parser.
// Code lineage:
//   This is a near-line-for-line port of host/src/source/RtmpServer.cpp.
//   Original PC source is preserved verbatim under
//   .backup_20260426_ABCE/ if a side-by-side diff is ever needed.

#include "../include/cr_feed.h"
#include "h264_decode_pipe.h"
#include "aac_decode_pipe.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "util/Obf.h"

#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop;

namespace
{

bool recv_all(int s, void* out, size_t n)
{
  uint8_t* p = (uint8_t*) out;
  while (n > 0)
  {
    if (g_stop.load(std::memory_order_relaxed))
      return false;
    ssize_t r = ::recv(s, p, n > (1u << 20) ? (1u << 20) : n, 0);
    if (r == 0)
      return false;
    if (r < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return false;
    }
    p += r;
    n -= (size_t) r;
  }
  return true;
}

bool send_all(int s, const void* buf, size_t n)
{
  const uint8_t* p = (const uint8_t*) buf;
  while (n > 0)
  {
    ssize_t w = ::send(s, p, n > (1u << 20) ? (1u << 20) : n, 0);
    if (w <= 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }
    p += w;
    n -= (size_t) w;
  }
  return true;
}

inline uint32_t be24(const uint8_t* p)
{
  return (p[0] << 16) | (p[1] << 8) | p[2];
}
inline uint32_t be32(const uint8_t* p)
{
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}
inline void put_be24(uint8_t* p, uint32_t v)
{
  p[0] = (v >> 16) & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = v & 0xff;
}
inline void put_be32(uint8_t* p, uint32_t v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

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
    memcpy(be, p, 8);
    p += 8;
    uint8_t le[8];
    for (int i = 0; i < 8; ++i)
      le[i] = be[7 - i];
    memcpy(&out, le, 8);
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
    return true;
  case 0x01:
    if (!has(1))
      return false;
    p += 1;
    return true;
  case 0x02:
  {
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
    return skip_object_body();
  case 0x05:
    return true;
  case 0x06:
    return true;
  case 0x08:
  {
    if (!has(4))
      return false;
    p += 4;
    return skip_object_body();
  }
  case 0x0a:
  {
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
    return false;
  }
}

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
  memcpy(le, &v, 8);
  for (int i = 7; i >= 0; --i)
    out.push_back(le[i]);
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

void build_rtmp_message(std::vector<uint8_t>& out, uint8_t cs_id, uint32_t timestamp, uint8_t msg_type, uint32_t msg_stream_id, const uint8_t* body, size_t body_len, uint32_t chunk_size)
{
  out.push_back((0 << 6) | (cs_id & 0x3f));
  uint8_t hdr[11];
  put_be24(hdr + 0, timestamp);
  put_be24(hdr + 3, (uint32_t) body_len);
  hdr[6] = msg_type;
  hdr[7] = msg_stream_id & 0xff;
  hdr[8] = (msg_stream_id >> 8) & 0xff;
  hdr[9] = (msg_stream_id >> 16) & 0xff;
  hdr[10] = (msg_stream_id >> 24) & 0xff;
  out.insert(out.end(), hdr, hdr + 11);

  size_t pos = 0;
  bool first = true;
  while (pos < body_len)
  {
    if (!first)
      out.push_back((3 << 6) | (cs_id & 0x3f));
    size_t take = (body_len - pos < (size_t) chunk_size) ? (body_len - pos) : (size_t) chunk_size;
    out.insert(out.end(), body + pos, body + pos + take);
    pos += take;
    first = false;
  }
}

void avc_config_to_annexb(const uint8_t* data, size_t len, std::vector<uint8_t>& out)
{
  if (len < 7)
    return;
  size_t p = 5;
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

// Per-connection RTMP session: handshake → chunks → FLV → push into pipe.
bool serve_client(int client)
{
  int no_delay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));

  // ---- Handshake (simple, no digest) -----------------------------------
  uint8_t c0c1[1 + 1536];
  if (!recv_all(client, c0c1, sizeof(c0c1)))
    return false;
  if (c0c1[0] != 0x03)
  {
    LOGW(OBF("rtmp: unexpected C0 version=0x%02x").c_str(), (unsigned) c0c1[0]);
    return false;
  }
  std::vector<uint8_t> s0s1s2;
  s0s1s2.reserve(1 + 1536 + 1536);
  s0s1s2.push_back(0x03);
  uint8_t s1[1536] = {0};
  std::random_device rd;
  for (int i = 8; i < 1536; ++i)
    s1[i] = (uint8_t) rd();
  s0s1s2.insert(s0s1s2.end(), s1, s1 + 1536);
  s0s1s2.insert(s0s1s2.end(), c0c1 + 1, c0c1 + 1 + 1536);
  if (!send_all(client, s0s1s2.data(), s0s1s2.size()))
    return false;

  uint8_t c2[1536];
  if (!recv_all(client, c2, sizeof(c2)))
    return false;
  LOGI(OBF("rtmp: handshake done").c_str());

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

  std::vector<uint8_t> sps_pps_annexb;
  int stream_w = 0, stream_h = 0;
  int stream_fps_hint = 30;
  bool pipe_started = false;
  cr_feed::H264DecodePipe pipe;

  // AAC audio sibling. Lazily configured when the first FLV audio
  // tag arrives with AACPacketType=0 (AudioSpecificConfig). Until
  // then it stays inert and ignores raw AAC frames. This is what
  // lets a "Start replace sound" session work with no pre-existing
  // PC AudioPump in direct-RTMP mode.
  cr_feed::AacDecodePipe audio_pipe;
  bool audio_pipe_started = false;

  auto try_start_pipe = [&]()
  {
    if (pipe_started)
      return true;
    if (stream_w <= 0 || stream_h <= 0)
      return false;
    if (!pipe.start(stream_w, stream_h))
    {
      LOGE(OBF("rtmp: H264DecodePipe.start(%dx%d) failed").c_str(), stream_w, stream_h);
      return false;
    }
    LOGI(OBF("rtmp: pipe started %dx%d (decoder=%s)").c_str(), stream_w, stream_h, pipe.codec_name());
    pipe_started = true;
    return true;
  };

  // Initial protocol-control bursts — Window Ack / Peer BW / Set Chunk
  // Size. Same as PC server.
  {
    uint8_t body[4];
    put_be32(body, 5'000'000);
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x05, 0, body, 4, out_chunk_size);
    if (!send_all(client, msg.data(), msg.size()))
      return false;
  }
  {
    uint8_t body[5];
    put_be32(body, 5'000'000);
    body[4] = 2;
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x06, 0, body, 5, out_chunk_size);
    if (!send_all(client, msg.data(), msg.size()))
      return false;
  }
  {
    uint8_t body[4];
    put_be32(body, out_chunk_size);
    std::vector<uint8_t> msg;
    build_rtmp_message(msg, 2, 0, 0x01, 0, body, 4, out_chunk_size);
    if (!send_all(client, msg.data(), msg.size()))
      return false;
  }

  while (!g_stop.load(std::memory_order_relaxed))
  {
    // --- Basic header -------------------------------------------------
    uint8_t bh0;
    if (!recv_all(client, &bh0, 1))
      break;
    uint32_t fmt = bh0 >> 6;
    uint32_t cs_id = bh0 & 0x3f;
    if (cs_id == 0)
    {
      uint8_t ext;
      if (!recv_all(client, &ext, 1))
        break;
      cs_id = 64u + ext;
    }
    else if (cs_id == 1)
    {
      uint8_t ext[2];
      if (!recv_all(client, ext, 2))
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
      if (!recv_all(client, h, 11))
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
      if (!recv_all(client, h, 7))
        break;
      ts_field = be24(h);
      last.length = be24(h + 3);
      last.type = h[6];
      used_delta = true;
    }
    else if (fmt == 2)
    {
      uint8_t h[3];
      if (!recv_all(client, h, 3))
        break;
      ts_field = be24(h);
      used_delta = true;
    }
    else
    {
      if (partial[cs_id].empty())
      {
        ts_field = 0;
        used_delta = last.have_delta;
      }
    }
    if (ts_field == 0xffffff)
    {
      uint8_t h[4];
      if (!recv_all(client, h, 4))
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
    uint32_t take = remaining < in_chunk_size ? remaining : in_chunk_size;
    size_t old_size = buf.size();
    buf.resize(old_size + take);
    if (take > 0)
    {
      if (!recv_all(client, buf.data() + old_size, take))
        goto done;
    }

    if (buf.size() < last.length)
      continue;

    // --- Full message — dispatch -------------------------------------
    {
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
      case 0x04:
      case 0x05:
      case 0x06:
        break;

      case 0x11:
      case 0x14:
      { // AMF command
        const uint8_t* p0 = body.data();
        const uint8_t* pe = body.data() + body.size();
        if (type == 0x11 && p0 < pe)
          ++p0;
        Amf0Reader r{p0, pe};
        std::string cmd;
        double tid = 0;
        if (!r.read_string(cmd) || !r.read_double(tid))
          break;

        if (cmd == OBF("connect").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("_result").c_str());
          amf_put_number(resp, tid);
          resp.push_back(0x03);
          amf_put_obj_key(resp, OBF("fmsVer").c_str());
          amf_put_string(resp, OBF("FMS/3,5,3,888").c_str());
          amf_put_obj_key(resp, OBF("capabilities").c_str());
          amf_put_number(resp, 31.0);
          amf_put_obj_key(resp, OBF("mode").c_str());
          amf_put_number(resp, 1.0);
          amf_put_obj_end(resp);
          resp.push_back(0x03);
          amf_put_obj_key(resp, OBF("level").c_str());
          amf_put_string(resp, OBF("status").c_str());
          amf_put_obj_key(resp, OBF("code").c_str());
          amf_put_string(resp, OBF("NetConnection.Connect.Success").c_str());
          amf_put_obj_key(resp, OBF("description").c_str());
          amf_put_string(resp, OBF("Connection succeeded.").c_str());
          amf_put_obj_key(resp, OBF("objectEncoding").c_str());
          amf_put_number(resp, 0.0);
          amf_put_obj_end(resp);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
          if (!send_all(client, msg.data(), msg.size()))
            goto done;
        }
        else if (cmd == OBF("releaseStream").c_str() || cmd == OBF("FCPublish").c_str() || cmd == OBF("FCUnpublish").c_str() || cmd == OBF("deleteStream").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("_result").c_str());
          amf_put_number(resp, tid);
          amf_put_null(resp);
          amf_put_null(resp);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
          if (!send_all(client, msg.data(), msg.size()))
            goto done;
        }
        else if (cmd == OBF("createStream").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("_result").c_str());
          amf_put_number(resp, tid);
          amf_put_null(resp);
          amf_put_number(resp, 1.0);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 3, 0, 0x14, 0, resp.data(), resp.size(), out_chunk_size);
          if (!send_all(client, msg.data(), msg.size()))
            goto done;
        }
        else if (cmd == OBF("publish").c_str())
        {
          std::vector<uint8_t> resp;
          amf_put_string(resp, OBF("onStatus").c_str());
          amf_put_number(resp, 0.0);
          amf_put_null(resp);
          resp.push_back(0x03);
          amf_put_obj_key(resp, OBF("level").c_str());
          amf_put_string(resp, OBF("status").c_str());
          amf_put_obj_key(resp, OBF("code").c_str());
          amf_put_string(resp, OBF("NetStream.Publish.Start").c_str());
          amf_put_obj_key(resp, OBF("description").c_str());
          amf_put_string(resp, OBF("Start publishing.").c_str());
          amf_put_obj_end(resp);
          std::vector<uint8_t> msg;
          build_rtmp_message(msg, 4, 0, 0x14, 1, resp.data(), resp.size(), out_chunk_size);
          if (!send_all(client, msg.data(), msg.size()))
            goto done;
        }
        break;
      }

      case 0x12:
      case 0x0f:
      { // AMF data (onMetaData)
        const uint8_t* p0 = body.data();
        const uint8_t* pe = body.data() + body.size();
        if (type == 0x0f && p0 < pe)
          ++p0;
        Amf0Reader r{p0, pe};
        std::string s;
        if (r.read_string(s) && (s == OBF("@setDataFrame").c_str() || s == OBF("onMetaData").c_str()))
        {
          std::string inner;
          if (s == OBF("@setDataFrame").c_str())
            r.read_string(inner);
          while (!r.eof())
          {
            uint8_t t = r.peek_type();
            if (t == 0x08)
            {
              ++r.p;
              r.p += 4;
            }
            else if (t == 0x03)
            {
              ++r.p;
            }
            else
              break;

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
                if (key == OBF("width").c_str())
                  stream_w = (int) d;
                else if (key == OBF("height").c_str())
                  stream_h = (int) d;
                else if (key == OBF("framerate").c_str() || key == "fps")
                  stream_fps_hint = (int) (d + 0.5);
              }
              else
              {
                if (!r.skip_value())
                  break;
              }
            }
            break;
          }
          if (stream_w > 0 && stream_h > 0)
          {
            LOGI(OBF("rtmp meta: %dx%d @%d fps").c_str(), stream_w, stream_h, stream_fps_hint);
            try_start_pipe();
          }
        }
        break;
      }

      case 0x08:
      { // audio (FLV)
        if (body.size() < 2)
          break;
        const uint8_t flv0 = body[0];
        const uint8_t sound_fmt = flv0 >> 4;
        if (sound_fmt != 10)
          break; // only AAC handled
        const uint8_t aac_pkt_type = body[1];
        const uint8_t* payload = body.data() + 2;
        size_t pay_len = body.size() - 2;
        if (aac_pkt_type == 0)
        { // AudioSpecificConfig
          if (audio_pipe.on_csd(payload, pay_len))
          {
            audio_pipe_started = true;
          }
        }
        else if (aac_pkt_type == 1 && audio_pipe_started)
        {
          int64_t pts_us = (int64_t) last.timestamp * 1000;
          audio_pipe.push(payload, pay_len, pts_us);
        }
        break;
      }

      case 0x09:
      { // video (H.264 / AVC)
        if (body.size() < 2)
          break;
        uint8_t frame_type = (body[0] >> 4) & 0x0f;
        uint8_t codec_id = body[0] & 0x0f;
        if (codec_id != 7)
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
          avc_config_to_annexb(payload, pay_len, sps_pps_annexb);
          // Some encoders publish the AVC sequence header BEFORE
          // onMetaData. If we still don't know the resolution,
          // we can't start the pipe yet — defer until the first
          // keyframe path with a meta-derived resolution.
          if (pipe_started && !sps_pps_annexb.empty())
          {
            pipe.push(sps_pps_annexb.data(), sps_pps_annexb.size(), pts_us);
          }
        }
        else if (avc_packet_type == 1)
        {
          // First video sample without onMetaData? Try to start
          // the pipe with a sane default; the framework will
          // reconfigure on first OUTPUT_FORMAT_CHANGED.
          if (!pipe_started)
          {
            if (stream_w <= 0 || stream_h <= 0)
            {
              stream_w = 1280;
              stream_h = 720;
              LOGW(OBF("rtmp: no meta yet, defaulting to %dx%d").c_str(), stream_w, stream_h);
            }
            try_start_pipe();
            if (!pipe_started)
              break;
          }
          std::vector<uint8_t> annexb;
          const bool is_key = (frame_type == 1);
          if (is_key && !sps_pps_annexb.empty())
          {
            annexb.insert(annexb.end(), sps_pps_annexb.begin(), sps_pps_annexb.end());
          }
          avcc_to_annexb(payload, pay_len, annexb);
          if (!annexb.empty())
          {
            pipe.push(annexb.data(), annexb.size(), pts_us);
          }
        }
        break;
      }

      default:
        break;
      }
    }
  }
done:
  LOGI(OBF("rtmp: client gone (decoded in=%llu out=%llu, audio=%s)").c_str(), (unsigned long long) pipe.input_count(), (unsigned long long) pipe.output_count(), audio_pipe_started ? "on" : OBF("off").c_str());
  pipe.stop();
  audio_pipe.stop();
  return true;
}

} // namespace

extern "C" int cr_feed_run_rtmp(int port);

int cr_feed_run_rtmp(int port)
{
  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0)
  {
    LOGE(OBF("rtmp: socket: %s").c_str(), strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t) port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(srv, (sockaddr*) &addr, sizeof(addr)) != 0)
  {
    LOGE(OBF("rtmp: bind :%d: %s").c_str(), port, strerror(errno));
    close(srv);
    return -1;
  }
  if (listen(srv, 1) != 0)
  {
    LOGE(OBF("rtmp: listen: %s").c_str(), strerror(errno));
    close(srv);
    return -1;
  }
  LOGI(OBF("rtmp listener on 127.0.0.1:%d").c_str(), port);

  while (!g_stop.load(std::memory_order_relaxed))
  {
    int client = accept(srv, nullptr, nullptr);
    if (client < 0)
    {
      if (errno == EINTR)
        continue;
      LOGW(OBF("rtmp: accept: %s").c_str(), strerror(errno));
      break;
    }
    LOGI(OBF("rtmp: publisher connected").c_str());
    serve_client(client);
    close(client);
    LOGI(OBF("rtmp: publisher gone").c_str());
  }

  close(srv);
  return 0;
}
