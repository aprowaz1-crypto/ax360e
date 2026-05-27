/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xe_aaudio_audio_driver.h"

#include <cstring>

#include <android/log.h>

#include "ax360e_perf_log.h"
#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"

static ax360e::perf::AudioHealthTracker g_audio_health;

namespace xe {
namespace apu {
namespace aaudio {

AAudioAudioDriver::AAudioAudioDriver(Memory* memory,
                                     xe::threading::Semaphore* semaphore)
    : semaphore_(semaphore) {}

AAudioAudioDriver::~AAudioAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool AAudioAudioDriver::Initialize() {
  aaudio_result_t result;

  result = AAudio_createStreamBuilder(&builder_);
  if (result != AAUDIO_OK) {
    XELOGE("AAudio_createStreamBuilder failed: {}", result);
    return false;
  }

  AAudioStreamBuilder_setFormat(builder_, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setSampleRate(builder_, host_frame_frequency_);
  AAudioStreamBuilder_setChannelCount(builder_, host_frame_channels_);
  AAudioStreamBuilder_setFramesPerDataCallback(builder_, channel_samples_);
  //AAudioStreamBuilder_setBufferCapacityInFrames(builder_, channel_samples_ * 2);

  AAudioStreamBuilder_setDataCallback(builder_, AudioCallback, this);
  AAudioStreamBuilder_setErrorCallback(builder_, AudioErrorCallback, this);

  AAudioStreamBuilder_setPerformanceMode(builder_, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(builder_, AAUDIO_SHARING_MODE_EXCLUSIVE);

  result = AAudioStreamBuilder_openStream(builder_, &stream_);
  if (result != AAUDIO_OK) {
    XELOGE("AAudioStreamBuilder_openStream failed: {}", result);
    return false;
  }

  // Log actual negotiated stream parameters
  __android_log_print(ANDROID_LOG_INFO, "ax360e_audio",
      "AAudio stream opened: sampleRate=%d channels=%d format=%d "
      "framesPerBurst=%d bufferCapacity=%d bufferSize=%d sharingMode=%d performanceMode=%d",
      AAudioStream_getSampleRate(stream_),
      AAudioStream_getChannelCount(stream_),
      AAudioStream_getFormat(stream_),
      AAudioStream_getFramesPerBurst(stream_),
      AAudioStream_getBufferCapacityInFrames(stream_),
      AAudioStream_getBufferSizeInFrames(stream_),
      AAudioStream_getSharingMode(stream_),
      AAudioStream_getPerformanceMode(stream_));

  // Set buffer size to 2x burst size for stability
  int32_t burst = AAudioStream_getFramesPerBurst(stream_);
  AAudioStream_setBufferSizeInFrames(stream_, burst * 2);

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    for (int i = 0; i < 8; i++) {
      float* buffer = new float[x360_frame_samples_];
      frames_unused_.push(buffer);
    }
  }

  stream_initialized_ = true;

  result = AAudioStream_requestStart(stream_);
  if (result != AAUDIO_OK) {
    XELOGE("AAudioStream_requestStart failed: {}", result);
    return false;
  }

  return true;
}

void AAudioAudioDriver::Pause() {
  if (stream_initialized_) {
    AAudioStream_requestPause(stream_);
  }
}

void AAudioAudioDriver::Resume() {
  if (stream_initialized_) {
    AAudioStream_requestStart(stream_);
  }
}

void AAudioAudioDriver::SetVolume(float volume) {
  float clamped = volume < 0.0f ? 0.0f : volume;
  volume_.store(clamped, std::memory_order_relaxed);
  __android_log_print(ANDROID_LOG_INFO, "ax360e_audio",
      "SetVolume called: %.4f (clamped: %.4f)", volume, clamped);
}

aaudio_data_callback_result_t AAudioAudioDriver::AudioCallback(
    AAudioStream* stream,
    void* userdata,
    void* audioData,
    int32_t numFrames) {
  SCOPE_profile_cpu_f("apu");

  auto driver = static_cast<AAudioAudioDriver*>(userdata);
  float* output_buffer = reinterpret_cast<float*>(audioData);
  const int32_t samples_count = numFrames * 2;

  g_audio_health.OnCallbackStart();

  std::unique_lock<std::mutex> guard(driver->frames_mutex_);
  g_audio_health.RecordQueueDepth(static_cast<uint32_t>(driver->frames_queued_.size()));

  if (driver->frames_queued_.empty()) {
    g_audio_health.RecordUnderrun();
    std::memset(output_buffer, 0, samples_count * sizeof(float));
  } else {
    auto buffer = driver->frames_queued_.front();
    driver->frames_queued_.pop();

    if (cvars::mute) {
      std::memset(output_buffer, 0, samples_count * sizeof(float));
    } else {
        conversion::sequential_6_BE_to_interleaved_2_LE(
                output_buffer, buffer, channel_samples_);
        // Apply volume and output gain (AAudio exclusive mode has no system mixer)
        constexpr float kOutputGain = 6.0f;
        float vol = driver->volume_.load(std::memory_order_relaxed);
        float gain = vol * kOutputGain;
        for (int32_t i = 0; i < samples_count; i++) {
          float s = output_buffer[i] * gain;
          // Clamp to [-1.0, 1.0] to avoid clipping distortion
          output_buffer[i] = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        }
    }

    // Diagnostic: log sample values every ~2 seconds (~375 callbacks)
    static uint32_t diag_counter = 0;
    if (++diag_counter % 375 == 1) {
      // Check input (raw BE from guest) and output (converted LE stereo)
      float raw0 = buffer[0], raw1 = buffer[1], raw2 = buffer[256], raw3 = buffer[257];
      float out0 = output_buffer[0], out1 = output_buffer[1], out2 = output_buffer[2], out3 = output_buffer[3];
      float max_out = 0.0f;
      for (int i = 0; i < 512; i++) {
        float a = output_buffer[i] < 0 ? -output_buffer[i] : output_buffer[i];
        if (a > max_out) max_out = a;
      }
      __android_log_print(ANDROID_LOG_INFO, "ax360e_audio_diag",
          "raw[FL0]=%.6f raw[FL1]=%.6f raw[FR0]=%.6f raw[FR1]=%.6f | "
          "out[L0]=%.6f out[R0]=%.6f out[L1]=%.6f out[R1]=%.6f | max_abs=%.6f mute=%d",
          raw0, raw1, raw2, raw3, out0, out1, out2, out3, max_out, (int)cvars::mute);
    }

    driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
  }

  g_audio_health.OnCallbackEnd();
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioAudioDriver::AudioErrorCallback(
    AAudioStream* stream,
    void* userdata,
    aaudio_result_t error) {
  XELOGE("AAudio stream error: {}", error);
  g_audio_health.RecordStreamError(static_cast<int32_t>(error));
}

void AAudioAudioDriver::SubmitFrame(float* samples) {
    const auto input_frame = samples;
  float* output_frame;

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      g_audio_health.RecordDynamicAlloc();
      output_frame = new float[x360_frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, x360_frame_samples_*sizeof(float));

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
  }
}

void AAudioAudioDriver::Shutdown() {
  if (stream_) {
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }

  if (builder_) {
    AAudioStreamBuilder_delete(builder_);
    builder_ = nullptr;
  }

  stream_initialized_ = false;

  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }

  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

}  // namespace aaudio
}  // namespace apu
}  // namespace xe
