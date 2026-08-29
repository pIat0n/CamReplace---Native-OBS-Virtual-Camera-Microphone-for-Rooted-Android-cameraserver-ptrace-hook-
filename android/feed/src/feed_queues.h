#pragma once

// Bounded packet/frame queues for the threaded feed pipeline (sprint C).
// The single-threaded path (tcp_h264_feed.cpp / opt-in rtmp_h264_feed.cpp via
// H264DecodePipe) reads → decodes → publishes synchronously per packet.
// On a slow keyframe burst the decoder back-pressures the network thread,
// which back-pressures OBS, which spikes its own send buffer — visible
// as "every IDR causes a 1-frame freeze" in the camera preview.
// Splitting the loop into:
//     network thread   → PacketQueue<NalPacket>  → decoder thread
//     decoder thread   → FrameQueue<Nv21Frame>   → publish thread (PTS-paced)
// decouples those steps. Each queue is bounded — when full, the producer
// drops the oldest non-key item (PacketQueue) or the oldest frame
// (FrameQueue). That drop policy preserves real-time responsiveness:
// we'd rather see a 33-ms gap than a 200-ms backed-up burst when the
// stream is misbehaving.
// This file is intentionally header-only: the threaded transport that
// uses it (tcp_h264_feed_v2.cpp, also new in sprint C) is the only TU
// that includes it for now. Pre-existing single-threaded code keeps
// using H264DecodePipe directly.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace cr_feed
{

struct NalPacket
{
  std::vector<uint8_t> data; // Annex-B with 00 00 00 01 prefix
  int64_t pts_us = 0;
  bool is_key = false;
};

struct Nv21Frame
{
  std::vector<uint8_t> data; // Y plane + interleaved VU
  int width = 0;
  int height = 0;
  int64_t pts_us = 0;
};

// Small, MPSC-friendly bounded ring with three knobs:
//   - max items
//   - on-full policy: kDropOldest (frame queue) or kDropOldestNonKey
//                     (packet queue — protect IDRs)
//   - close() unblocks waiters so the consumer can join cleanly on
//     shutdown.
// Move-only T avoids the per-frame buffer copy when popping.
template <typename T> class BoundedQueue
{
public:
  enum DropPolicy
  {
    kDropOldest,
    kDropOldestNonKey
  };

  explicit BoundedQueue(size_t cap, DropPolicy pol = kDropOldest) : cap_(cap), policy_(pol) {}

  // Push moves `item` into the queue. Returns true if accepted, false if
  // we had to drop something to make room (caller may want a stat bump).
  bool push(T&& item)
  {
    std::unique_lock<std::mutex> lk(mu_);
    bool dropped = false;
    if (q_.size() >= cap_)
    {
      dropped = true;
      evict_locked();
    }
    q_.emplace_back(std::move(item));
    cv_.notify_one();
    return !dropped;
  }

  // Pop blocks until an item is available OR the queue is closed. On
  // close+empty, returns false.
  bool pop(T& out)
  {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return !q_.empty() || closed_; });
    if (q_.empty())
      return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  // Try-pop with a timeout. Returns false on timeout or close+empty.
  bool pop_for(T& out, std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, timeout, [this] { return !q_.empty() || closed_; }))
      return false;
    if (q_.empty())
      return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  void close()
  {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    cv_.notify_all();
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }

private:
  // Caller holds mu_.
  void evict_locked()
  {
    if (q_.empty())
      return;
    if constexpr (std::is_same_v<T, NalPacket>)
    {
      if (policy_ == kDropOldestNonKey)
      {
        // Walk front-to-back, drop first non-key. Falls back to
        // kDropOldest if everything we hold is a key (shouldn't
        // happen in practice but never throw away the only IDR
        // we have).
        for (auto it = q_.begin(); it != q_.end(); ++it)
        {
          if (!it->is_key)
          {
            q_.erase(it);
            return;
          }
        }
      }
    }
    q_.pop_front();
  }

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> q_;
  size_t cap_;
  DropPolicy policy_;
  bool closed_ = false;
};

} // namespace cr_feed
