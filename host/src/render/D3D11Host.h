#pragma once

// Minimal D3D11 wrapper for ImGui + our preview surfaces.
// One D3D11Host per window. Owns the device, context, swap chain and back-
// buffer RTV. Resize is driven by WM_SIZE.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace cr::app
{
// Accessor for the app's shared D3D11 device — implemented in App.cpp so
// UI-side code (TexturePreview, future renderers) can upload textures
// without plumbing the host through every function call.
ID3D11Device* d3d_device() noexcept;
} // namespace cr::app

namespace cr::render
{

class D3D11Host
{
public:
  bool init(HWND hwnd);
  void shutdown();

  // Call from WM_SIZE. Safe to call before init() — no-op in that case.
  void on_resize(UINT width, UINT height);

  // Begin/end frame: clear to theme background, swap.
  void begin_frame(const float clear_rgba[4]);
  void end_frame(bool vsync = true);

  ID3D11Device* device() const noexcept
  {
    return device_;
  }
  ID3D11DeviceContext* context() const noexcept
  {
    return context_;
  }

private:
  void create_rtv_from_backbuffer();
  void release_rtv();

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  IDXGISwapChain* swapchain_ = nullptr;
  ID3D11RenderTargetView* rtv_ = nullptr;
};

} // namespace cr::render
