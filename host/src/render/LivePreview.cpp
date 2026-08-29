#include "render/LivePreview.h"
#include <cstring>

namespace cr::render
{

LivePreview& LivePreview::instance()
{
  static LivePreview p;
  return p;
}

LivePreview::~LivePreview()
{
  if (srv_)
    srv_->Release();
  if (tex_)
    tex_->Release();
}

void LivePreview::publish_bgra(const uint8_t* data, int w, int h, int pitch)
{
  if (!data || w <= 0 || h <= 0 || pitch < w * 4)
    return;

  std::lock_guard<std::mutex> lk(mu_);
  pending_.resize((size_t) w * h * 4);
  const int row_bytes = w * 4;
  for (int y = 0; y < h; ++y)
  {
    std::memcpy(pending_.data() + (size_t) y * row_bytes, data + (size_t) y * pitch, (size_t) row_bytes);
  }
  pending_w_ = w;
  pending_h_ = h;
  has_pending_ = true;
}

bool LivePreview::ensure_texture_(ID3D11Device* dev, int w, int h)
{
  if (tex_ && tex_w_ == w && tex_h_ == h)
    return true;

  if (srv_)
  {
    srv_->Release();
    srv_ = nullptr;
  }
  if (tex_)
  {
    tex_->Release();
    tex_ = nullptr;
  }
  tex_w_ = tex_h_ = 0;

  D3D11_TEXTURE2D_DESC td{};
  td.Width = (UINT) w;
  td.Height = (UINT) h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex_)))
    return false;

  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = td.Format;
  sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sv.Texture2D.MipLevels = 1;
  if (FAILED(dev->CreateShaderResourceView(tex_, &sv, &srv_)))
  {
    tex_->Release();
    tex_ = nullptr;
    return false;
  }

  tex_w_ = w;
  tex_h_ = h;
  return true;
}

void* LivePreview::srv(ID3D11Device* dev)
{
  if (!dev)
    return nullptr;

  // Snapshot pending under lock, then do the GPU upload outside it.
  std::vector<uint8_t> upload;
  int up_w = 0, up_h = 0;
  bool have = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (has_pending_)
    {
      upload = std::move(pending_);
      up_w = pending_w_;
      up_h = pending_h_;
      has_pending_ = false;
      pending_.clear();
      pending_w_ = pending_h_ = 0;
      have = true;
    }
  }

  if (have)
  {
    if (ensure_texture_(dev, up_w, up_h))
    {
      ID3D11DeviceContext* ctx = nullptr;
      dev->GetImmediateContext(&ctx);
      if (ctx)
      {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(tex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
          const int row_bytes = up_w * 4;
          for (int y = 0; y < up_h; ++y)
          {
            std::memcpy((uint8_t*) m.pData + (size_t) y * m.RowPitch, upload.data() + (size_t) y * row_bytes, (size_t) row_bytes);
          }
          ctx->Unmap(tex_, 0);
        }
        ctx->Release();
      }
    }
  }

  return srv_;
}

void LivePreview::reset()
{
  std::lock_guard<std::mutex> lk(mu_);
  if (srv_)
  {
    srv_->Release();
    srv_ = nullptr;
  }
  if (tex_)
  {
    tex_->Release();
    tex_ = nullptr;
  }
  tex_w_ = tex_h_ = 0;
  pending_.clear();
  pending_w_ = pending_h_ = 0;
  has_pending_ = false;
}

} // namespace cr::render
