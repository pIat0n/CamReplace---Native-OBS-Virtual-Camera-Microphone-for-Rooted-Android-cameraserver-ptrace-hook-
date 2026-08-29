#include "render/D3D11Host.h"
#include "util/Log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <string>
namespace cr::render
{

bool D3D11Host::init(HWND hwnd)
{
  DXGI_SWAP_CHAIN_DESC sd = {};
  RECT rc{};
  ::GetClientRect(hwnd, &rc);
  const UINT client_w = static_cast<UINT>(rc.right > rc.left ? rc.right - rc.left : 1280);
  const UINT client_h = static_cast<UINT>(rc.bottom > rc.top ? rc.bottom - rc.top : 760);
  sd.BufferCount = 2;
  sd.BufferDesc.Width = client_w;
  sd.BufferDesc.Height = client_h;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT create_flags = 0;
#if !defined(NDEBUG)
  // Leave the debug layer off by default — it pulls d3d11_sdklayers.dll which
  // isn't installed on every machine. Flip on when debugging locally.
  // create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
  };
  D3D_FEATURE_LEVEL got_level = {};

  auto create_with_driver = [&](D3D_DRIVER_TYPE driver_type) -> HRESULT { return D3D11CreateDeviceAndSwapChain(nullptr, driver_type, nullptr, create_flags, feature_levels, _countof(feature_levels), D3D11_SDK_VERSION, &sd, &swapchain_, &device_, &got_level, &context_); };

  bool using_warp = false;
  HRESULT hr = create_with_driver(D3D_DRIVER_TYPE_HARDWARE);
  if (FAILED(hr))
  {
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), "hardware D3D11CreateDeviceAndSwapChain failed hr=0x%08lx size=%ux%u; retrying WARP", static_cast<unsigned long>(hr), static_cast<unsigned>(client_w), static_cast<unsigned>(client_h));
    cr::log::warn("d3d", buf);
    using_warp = true;
    hr = create_with_driver(D3D_DRIVER_TYPE_WARP);
  }

  if (FAILED(hr))
  {
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "D3D11CreateDeviceAndSwapChain failed hr=0x%08lx size=%ux%u", static_cast<unsigned long>(hr), static_cast<unsigned>(client_w), static_cast<unsigned>(client_h));
    cr::log::error("d3d", buf);
    return false;
  }

  {
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "D3D11 device ready driver=%s size=%ux%u", using_warp ? "warp" : "hardware", static_cast<unsigned>(client_w), static_cast<unsigned>(client_h));
    cr::log::info("d3d", buf);
  }

  create_rtv_from_backbuffer();
  return true;
}

void D3D11Host::shutdown()
{
  release_rtv();
  if (swapchain_)
  {
    swapchain_->Release();
    swapchain_ = nullptr;
  }
  if (context_)
  {
    context_->Release();
    context_ = nullptr;
  }
  if (device_)
  {
    device_->Release();
    device_ = nullptr;
  }
}

void D3D11Host::on_resize(UINT width, UINT height)
{
  if (!swapchain_ || width == 0 || height == 0)
    return;
  release_rtv();
  swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  create_rtv_from_backbuffer();
}

void D3D11Host::begin_frame(const float clear_rgba[4])
{
  context_->OMSetRenderTargets(1, &rtv_, nullptr);
  context_->ClearRenderTargetView(rtv_, clear_rgba);
}

void D3D11Host::end_frame(bool vsync)
{
  swapchain_->Present(vsync ? 1 : 0, 0);
}

void D3D11Host::create_rtv_from_backbuffer()
{
  ID3D11Texture2D* backbuf = nullptr;
  swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuf));
  if (!backbuf)
    return;
  device_->CreateRenderTargetView(backbuf, nullptr, &rtv_);
  backbuf->Release();
}

void D3D11Host::release_rtv()
{
  if (rtv_)
  {
    rtv_->Release();
    rtv_ = nullptr;
  }
}

} // namespace cr::render
