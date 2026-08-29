/*
 * Назначение файла:
 *   ABI shared memory /data/cr/feed для video/photo pipeline. cr_feed_proc
 *   пишет NV21 кадры в triple-buffered slots, camhook читает последний
 *   опубликованный slot внутри cameraserver и подменяет YUV/BLOB buffers.
 *
 * ABI/совместимость:
 *   cr_feed_header обязан оставаться 64 байта. Wire/shm формат v1 оставляет
 *   фиксированный per-slot reserve под 1920x1080 NV21; это намеренный cap,
 *   а не динамический allocator. Более крупные потоки должны завершаться
 *   диагностируемой ошибкой.
 *
 * Pixel/Android ограничения:
 *   Pixel 6-10 могут отдавать preview/photo pipelines выше 1080p, но текущий
 *   v1 контракт не меняет размер slot на лету. Динамический slot sizing
 *   требует нового shm version и отдельной миграции.
 */

#pragma once

// Shared-memory layout for /data/cr/feed.
// One header, followed by N equally-sized NV21 slots. Writers advance
// write_index after filling a slot; readers pick the latest committed slot
// via the index (typical single-producer / multiple-reader ring).
// Kept tiny and POD — readers in other processes (cameraserver hook,
// debugger) map the same struct without any C++ runtime.

#include <stdatomic.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

// Magic: 'CRFD' little-endian.
#define CR_FEED_MAGIC 0x44465243u

  typedef struct cr_feed_header
  {
    uint32_t magic;              // CR_FEED_MAGIC
    uint32_t version;            // CR_FEED_SHM_VERSION
    uint32_t width;              // pixels
    uint32_t height;             // pixels
    uint32_t stride;             // == width for NV21
    uint32_t slot_size;          // bytes per slot = width*height*3/2
    uint32_t num_slots;          // >= 1
    uint32_t format;             // 0 = NV21
    atomic_uint write_index;     // last slot a writer fully committed
    atomic_uint generation;      // bumped on any feed-shape change
    atomic_ullong frame_counter; // monotonic, ++'d per commit
    // Set READY only after SCT2 verification, Finished exchange and
    // authenticated input. Hooks gate replacement on it as a fail-closed
    // availability signal; shared memory is not a root security boundary.
    atomic_uint channel_state;
    uint32_t reserved[3]; // keep total size = 64 bytes
  } cr_feed_header;

  _Static_assert(sizeof(cr_feed_header) == 64, "header must stay 64 bytes");

#ifndef CR_CHANNEL_STATE_NONE
#define CR_CHANNEL_STATE_NONE 0u
#define CR_CHANNEL_STATE_READY 1u
#endif

// Default path — /data/cr exists, u:object_r:system_data_file:s0 after the
// one-time setup we'll do in sprint 4. For sprint 3 the writer creates it.
#define CR_FEED_PATH "/data/cr/feed"

// Number of NV21 slots in the ring. Triple-buffered so the writer can
// memcpy a fresh frame into slot[(wi+1)%N] without ever racing the
// camera-server hook that's reading slot[wi]. Tearing was visible with
// the previous N=1 layout — half top old / half bottom new — even at
// ~3 MB/s sustained writes.
#define CR_FEED_NUM_SLOTS 3u

#define CR_FEED_MAX_WIDTH 1920u
#define CR_FEED_MAX_HEIGHT 1080u

// Reserved per-slot size. Sized for 1920×1080 NV21 (~3 MB) which is the
// substituted-camera output we actually use; the file ftruncates up to
// `sizeof(header) + CR_FEED_NUM_SLOTS * CR_FEED_MAX_NV21_BYTES` (~9 MB
// total). Older builds used a single 18 MB slot; the bigger reserve is
// fine on every Android we target but most of it sat unused.
#define CR_FEED_MAX_NV21_BYTES ((uint32_t) (CR_FEED_MAX_WIDTH * CR_FEED_MAX_HEIGHT * 3u / 2u))

#ifdef __cplusplus
}
#endif
