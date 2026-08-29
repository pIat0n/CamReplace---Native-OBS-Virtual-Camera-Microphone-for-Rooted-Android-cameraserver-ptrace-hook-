#pragma once

// Cross-thread BGRA frame sink for "what's currently being streamed".
// The decoder worker (VideoSource, and later ScreenSource/WebcamSource) calls
// publish_bgra() after converting a frame. The UI thread calls srv() once
// per draw; if a new frame is waiting it's uploaded into a D3D11 dynamic
// texture and the SRV is returned. ImGui::Image displays it, so the preview
// panel shows exactly the last frame we handed to the phone — same
// resolution, same colours, same frame-rate as the phone sees.
// Single producer (the active source worker) / single consumer (UI) but the
// class is thread-safe anyway via a short mutex around the pending buffer.

#include <cstdint>
#include <d3d11.h>
#include <mutex>
#include <vector>

namespace cr::render
{

class LivePreview
{
public:
  static LivePreview& instance();

  // Worker side. Copies w*h*4 BGRA bytes out of `data` under the lock.
  // `pitch` is the source row length in bytes (may include padding).
  void publish_bgra(const uint8_t* data, int w, int h, int pitch);

  // UI side. Uploads any pending frame to the internal D3D11 texture and
  // returns its SRV. Returns nullptr while there's no valid frame.
  void* srv(ID3D11Device* dev);

  int width() const noexcept
  {
    return tex_w_;
  }
  int height() const noexcept
  {
    return tex_h_;
  }

  // Drop pending buffer + tear down the GPU texture. Called when the
  // active stream stops so the UI falls back to the first-frame preview.
  void reset();

private:
  LivePreview() = default;
  ~LivePreview();
  LivePreview(const LivePreview&) = delete;
  LivePreview& operator=(const LivePreview&) = delete;

  bool ensure_texture_(ID3D11Device* dev, int w, int h);

  mutable std::mutex mu_;
  std::vector<uint8_t> pending_;
  int pending_w_ = 0;
  int pending_h_ = 0;
  bool has_pending_ = false;

  ID3D11Texture2D* tex_ = nullptr;
  ID3D11ShaderResourceView* srv_ = nullptr;
  int tex_w_ = 0;
  int tex_h_ = 0;
};

} // namespace cr::render
