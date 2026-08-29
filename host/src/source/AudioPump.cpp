#include "source/AudioPump.h"
#include "util/Log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h> // CLSID_CResamplerMediaObject
#include <wrl/client.h>
#include <mmreg.h>
#include <initguid.h>

#include <chrono>
#include <cstring>
#include <thread>
// CLSID_CResamplerMediaObject is declared in wmcodecdsp.h but its storage
// lives in wmcodecdspuuid.lib, which the MinGW toolchain does not link.
// Define it locally so we can pass it to CoCreateInstance without dragging
// in another import lib.
//   {f447b69e-1884-4a7e-8055-346f74d6edb3}
DEFINE_GUID(CR_CLSID_CResamplerMediaObject, 0xf447b69e, 0x1884, 0x4a7e, 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3);

using Microsoft::WRL::ComPtr;

namespace cr::source
{

namespace
{

void emit(const cr::device::DeployCallback& cb, cr::device::DeployStatus::Kind k, std::string step, std::string detail = {})
{
  if (cb)
    cb({k, std::move(step), std::move(detail)});
}

// Microsoft AAC decoder MFT expects MF_MT_USER_DATA to be the trailing
// 12 bytes of HEAACWAVEINFO followed by the raw AudioSpecificConfig.
std::vector<uint8_t> make_heaac_user_data(const uint8_t* csd, size_t csd_len)
{
  std::vector<uint8_t> u(12, 0); // wPayloadType=0 + reserved fields
  u.insert(u.end(), csd, csd + csd_len);
  return u;
}

// Build a PCM IMFMediaType describing S16LE @ sr / channels.
ComPtr<IMFMediaType> make_pcm_type(uint32_t sr, uint32_t ch)
{
  ComPtr<IMFMediaType> t;
  if (FAILED(MFCreateMediaType(t.GetAddressOf())))
    return {};
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sr);
  t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
  t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2u * ch);
  t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sr * 2u * ch);
  t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
  return t;
}

} // namespace

bool AudioPump::start(std::string serial, const Config& cfg, cr::device::DeployCallback cb)
{
  if (running_.exchange(true))
    return false;
  serial_ = std::move(serial);
  cfg_ = cfg;
  cb_ = std::move(cb);
  csd_seen_ = false;
  tx_header_sent_ = false;
  bytes_sent_.store(0);

  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
  {
    emit(cb_, cr::device::DeployStatus::Kind::Err, "audio: MFStartup failed");
    running_ = false;
    return false;
  }
  mf_init_ = true;

  // Connect to cr_feed_proc's tcp_pcm listener. Retry briefly because the
  // phone may still be coming up after start_software.
  bool connected = false;
  for (int i = 0; i < 20; ++i)
  {
    if (tx_.open(cfg_.adb_port, cr::secure_channel::v2::StreamKind::Audio))
    {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  if (!connected)
  {
    emit(cb_, cr::device::DeployStatus::Kind::Err, "audio: connect to cr_feed_proc (PCM) failed", "port " + std::to_string(cfg_.adb_port));
    teardown_decoder_();
    teardown_resampler_();
    if (mf_init_)
    {
      MFShutdown();
      mf_init_ = false;
    }
    running_ = false;
    return false;
  }

  emit(cb_, cr::device::DeployStatus::Kind::Ok, "audio: PCM tunnel ready (waits for OBS audio)");
  return true;
}

void AudioPump::stop()
{
  if (!running_.exchange(false))
    return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    teardown_decoder_();
    teardown_resampler_();
  }
  tx_.close();
  tx_header_sent_ = false;
  if (mf_init_)
  {
    MFShutdown();
    mf_init_ = false;
  }
  emit(cb_, cr::device::DeployStatus::Kind::Ok, "audio: stopped", "sent " + std::to_string(bytes_sent_.load()) + " PCM bytes");
}

bool AudioPump::ensure_decoder_(const uint8_t* csd, size_t csd_len)
{
  if (!csd || csd_len < 2)
    return false;

  // Parse AudioSpecificConfig: object_type (5b), sr_idx (4b), channel_cfg (4b).
  const uint8_t b0 = csd[0];
  const uint8_t b1 = csd[1];
  int obj_type = (b0 >> 3) & 0x1f;
  int sr_idx = ((b0 & 0x07) << 1) | (b1 >> 7);
  int channels = (b1 >> 3) & 0x0f;
  static const int kSrTable[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};
  int sample_rate = (sr_idx >= 0 && sr_idx < 13) ? kSrTable[sr_idx] : 0;
  if (sample_rate <= 0 || channels < 1 || channels > 2)
  {
    cr::log::warn("audio", "unsupported AAC csd obj=" + std::to_string(obj_type) + " sr_idx=" + std::to_string(sr_idx) + " ch=" + std::to_string(channels));
    return false;
  }

  teardown_decoder_();

  // Find the Microsoft AAC decoder via MFTEnumEx.
  MFT_REGISTER_TYPE_INFO ininfo = {MFMediaType_Audio, MFAudioFormat_AAC};
  MFT_REGISTER_TYPE_INFO outinfo = {MFMediaType_Audio, MFAudioFormat_PCM};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, flags, &ininfo, &outinfo, &activates, &count)) || count == 0)
  {
    cr::log::warn("audio", "no AAC decoder MFT available");
    if (activates)
      CoTaskMemFree(activates);
    return false;
  }
  for (UINT32 i = 0; i < count && !dec_; ++i)
  {
    if (FAILED(activates[i]->ActivateObject(IID_IMFTransform, (void**) &dec_)))
    {
      dec_ = nullptr;
    }
  }
  for (UINT32 i = 0; i < count; ++i)
    activates[i]->Release();
  CoTaskMemFree(activates);
  if (!dec_)
  {
    cr::log::warn("audio", "AAC decoder ActivateObject failed");
    return false;
  }

  // --- Input type: AAC at the rate / channels declared in the CSD -------
  auto user_data = make_heaac_user_data(csd, csd_len);
  {
    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(in.GetAddressOf())))
      return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    in->SetBlob(MF_MT_USER_DATA, user_data.data(), (UINT32) user_data.size());
    if (FAILED(dec_->SetInputType(0, in.Get(), 0)))
    {
      cr::log::warn("audio", "SetInputType failed");
      return false;
    }
  }

  // --- Output type: 16-bit PCM at decoder native rate / channels -------
  {
    auto ot = make_pcm_type(sample_rate, channels);
    if (!ot || FAILED(dec_->SetOutputType(0, ot.Get(), 0)))
    {
      cr::log::warn("audio", "SetOutputType failed");
      return false;
    }
  }

  dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  dec_sr_ = sample_rate;
  dec_ch_ = channels;
  out_sr_ = cfg_.target_sr > 0 ? cfg_.target_sr : dec_sr_;
  out_ch_ = cfg_.target_ch > 0 ? cfg_.target_ch : dec_ch_;
  cr::log::info("audio", "decoder ready " + std::to_string(sample_rate) + " Hz / " + std::to_string(channels) + " ch");

  // Re-init the resampler so its input type matches the new decoder
  // output. Drop any half-converted samples held inside it.
  teardown_resampler_();
  if (!ensure_resampler_())
  {
    cr::log::warn("audio", "resampler init failed");
    return false;
  }

  return send_pcm_header_();
}

bool AudioPump::ensure_resampler_()
{
  if (dec_sr_ <= 0 || dec_ch_ <= 0 || out_sr_ <= 0 || out_ch_ <= 0)
  {
    return false;
  }
  if (out_sr_ == dec_sr_ && out_ch_ == dec_ch_)
  {
    rs_ = nullptr;
    cr::log::info("audio", "resampler bypass " + std::to_string(out_sr_) + "/" + std::to_string(out_ch_));
    return true;
  }

  HRESULT hr = CoCreateInstance(CR_CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rs_));
  if (FAILED(hr) || !rs_)
  {
    cr::log::warn("audio", "CoCreateInstance(Resampler) failed hr=0x" + std::to_string((unsigned long) hr));
    rs_ = nullptr;
    return false;
  }
  auto in_t = make_pcm_type((uint32_t) dec_sr_, (uint32_t) dec_ch_);
  auto out_t = make_pcm_type((uint32_t) out_sr_, (uint32_t) out_ch_);
  if (!in_t || !out_t || FAILED(rs_->SetInputType(0, in_t.Get(), 0)) || FAILED(rs_->SetOutputType(0, out_t.Get(), 0)))
  {
    cr::log::warn("audio", "resampler SetType failed");
    rs_->Release();
    rs_ = nullptr;
    return false;
  }
  rs_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  rs_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  cr::log::info("audio", "resampler " + std::to_string(dec_sr_) + "/" + std::to_string(dec_ch_) + " -> " + std::to_string(out_sr_) + "/" + std::to_string(out_ch_));
  return true;
}

void AudioPump::teardown_decoder_()
{
  if (dec_)
  {
    dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    dec_->Release();
    dec_ = nullptr;
  }
  dec_sr_ = dec_ch_ = 0;
  out_sr_ = out_ch_ = 0;
}

void AudioPump::teardown_resampler_()
{
  if (rs_)
  {
    rs_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    rs_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    rs_->Release();
    rs_ = nullptr;
  }
}

void AudioPump::on_aac_csd(const uint8_t* csd, size_t len)
{
  if (!running_.load())
    return;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_decoder_(csd, len))
    return;
  csd_seen_ = true;
}

void AudioPump::on_aac_frame(const uint8_t* aac, size_t len, int64_t pts_us)
{
  if (!running_.load() || !aac || !len)
    return;
  std::lock_guard<std::mutex> lk(mu_);
  if (!csd_seen_ || !dec_)
    return; // wait for CSD
  process_(aac, len, pts_us);
}

bool AudioPump::send_pcm_header_()
{
  if (tx_header_sent_)
    return true;
  if (out_sr_ <= 0 || out_ch_ <= 0)
    return false;
  if (!tx_.send_pcm_header((uint32_t) out_sr_, (uint16_t) out_ch_, /*bps*/ 2))
  {
    cr::log::warn("audio", "send_pcm_header failed");
    return false;
  }
  tx_header_sent_ = true;
  return true;
}

bool AudioPump::resample_and_send_(const uint8_t* pcm, size_t bytes)
{
  if (!pcm || bytes == 0)
    return false;
  if (!send_pcm_header_())
    return false;

  if (!rs_)
  {
    if (tx_.is_open() && tx_.send_pcm(pcm, bytes))
    {
      bytes_sent_.fetch_add(bytes, std::memory_order_relaxed);
      return true;
    }
    cr::log::warn("audio", "send_pcm failed");
    return false;
  }

  ComPtr<IMFMediaBuffer> in_buf;
  if (FAILED(MFCreateMemoryBuffer((DWORD) bytes, in_buf.GetAddressOf())))
    return false;
  BYTE* p = nullptr;
  DWORD ml = 0;
  if (SUCCEEDED(in_buf->Lock(&p, &ml, nullptr)))
  {
    std::memcpy(p, pcm, bytes);
    in_buf->Unlock();
  }
  in_buf->SetCurrentLength((DWORD) bytes);

  ComPtr<IMFSample> in_smp;
  if (FAILED(MFCreateSample(in_smp.GetAddressOf())))
    return false;
  in_smp->AddBuffer(in_buf.Get());

  HRESULT hr = rs_->ProcessInput(0, in_smp.Get(), 0);
  if (FAILED(hr))
  {
    cr::log::info("audio", "rs ProcessInput hr=0x" + std::to_string((unsigned long) hr));
    return false;
  }

  bool any_sent = false;
  while (true)
  {
    MFT_OUTPUT_STREAM_INFO si{};
    rs_->GetOutputStreamInfo(0, &si);

    DWORD sz = si.cbSize > 0 ? si.cbSize : 8192;
    ComPtr<IMFSample> out_smp;
    ComPtr<IMFMediaBuffer> out_buf;
    if (FAILED(MFCreateSample(out_smp.GetAddressOf())))
      break;
    if (FAILED(MFCreateMemoryBuffer(sz, out_buf.GetAddressOf())))
      break;
    out_smp->AddBuffer(out_buf.Get());

    MFT_OUTPUT_DATA_BUFFER db{};
    db.dwStreamID = 0;
    db.pSample = out_smp.Get();
    DWORD status = 0;
    hr = rs_->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents)
    {
      db.pEvents->Release();
      db.pEvents = nullptr;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      break;
    if (FAILED(hr))
    {
      cr::log::info("audio", "rs ProcessOutput hr=0x" + std::to_string((unsigned long) hr));
      break;
    }

    ComPtr<IMFMediaBuffer> mbuf;
    if (FAILED(out_smp->ConvertToContiguousBuffer(mbuf.GetAddressOf())) || !mbuf)
    {
      continue;
    }
    BYTE* src = nullptr;
    DWORD cl = 0, mlen = 0;
    if (FAILED(mbuf->Lock(&src, &mlen, &cl)))
      continue;
    if (cl > 0 && tx_.is_open())
    {
      if (tx_.send_pcm(src, cl))
      {
        bytes_sent_.fetch_add(cl, std::memory_order_relaxed);
        any_sent = true;
      }
      else
      {
        cr::log::warn("audio", "send_pcm failed");
      }
    }
    mbuf->Unlock();
  }
  return any_sent;
}

void AudioPump::process_(const uint8_t* aac, size_t len, int64_t pts_us)
{
  // Push AAC frame into the decoder.
  ComPtr<IMFMediaBuffer> in_buf;
  if (FAILED(MFCreateMemoryBuffer((DWORD) len, in_buf.GetAddressOf())))
    return;
  BYTE* p = nullptr;
  DWORD ml = 0;
  if (SUCCEEDED(in_buf->Lock(&p, &ml, nullptr)))
  {
    std::memcpy(p, aac, len);
    in_buf->Unlock();
  }
  in_buf->SetCurrentLength((DWORD) len);

  ComPtr<IMFSample> in_smp;
  if (FAILED(MFCreateSample(in_smp.GetAddressOf())))
    return;
  in_smp->AddBuffer(in_buf.Get());
  in_smp->SetSampleTime(pts_us * 10);

  HRESULT hr = dec_->ProcessInput(0, in_smp.Get(), 0);
  if (FAILED(hr))
  {
    cr::log::info("audio", "ProcessInput hr=0x" + std::to_string((unsigned long) hr));
    return;
  }

  // Drain decoded PCM into the resampler.
  while (true)
  {
    MFT_OUTPUT_STREAM_INFO si{};
    dec_->GetOutputStreamInfo(0, &si);

    DWORD sz = si.cbSize > 0 ? si.cbSize : 8192;
    ComPtr<IMFSample> out_smp;
    ComPtr<IMFMediaBuffer> out_buf;
    if (FAILED(MFCreateSample(out_smp.GetAddressOf())))
      return;
    if (FAILED(MFCreateMemoryBuffer(sz, out_buf.GetAddressOf())))
      return;
    out_smp->AddBuffer(out_buf.Get());

    MFT_OUTPUT_DATA_BUFFER db{};
    db.dwStreamID = 0;
    db.pSample = out_smp.Get();
    DWORD status = 0;
    hr = dec_->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents)
    {
      db.pEvents->Release();
      db.pEvents = nullptr;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      return;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
      ComPtr<IMFMediaType> ot;
      if (SUCCEEDED(dec_->GetOutputAvailableType(0, 0, ot.GetAddressOf())))
        dec_->SetOutputType(0, ot.Get(), 0);
      continue;
    }
    if (FAILED(hr))
    {
      cr::log::info("audio", "ProcessOutput hr=0x" + std::to_string((unsigned long) hr));
      return;
    }

    ComPtr<IMFMediaBuffer> mbuf;
    if (FAILED(out_smp->ConvertToContiguousBuffer(mbuf.GetAddressOf())) || !mbuf)
    {
      continue;
    }
    BYTE* src = nullptr;
    DWORD cl = 0, mlen = 0;
    if (FAILED(mbuf->Lock(&src, &mlen, &cl)))
      continue;
    if (cl > 0)
    {
      resample_and_send_(src, cl);
    }
    mbuf->Unlock();
  }
}

} // namespace cr::source
