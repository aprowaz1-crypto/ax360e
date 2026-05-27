/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#ifndef AX360E_XE_AAUDIO_AUDIO_DRIVER_H
#define AX360E_XE_AAUDIO_AUDIO_DRIVER_H

#include <atomic>
#include <mutex>
#include <queue>
#include <stack>

#include <aaudio/AAudio.h>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace aaudio {

class AAudioAudioDriver : public AudioDriver {
 public:
  AAudioAudioDriver(Memory* memory, xe::threading::Semaphore* semaphore);
  ~AAudioAudioDriver() override;

  bool Initialize();
    void Pause() override;
    void Resume() override;
    void SetVolume(float volume) override;
  void SubmitFrame(float* frame) override;
  void Shutdown();

 protected:
  static aaudio_data_callback_result_t AudioCallback(
      AAudioStream* stream,
      void* userdata,
      void* audioData,
      int32_t numFrames);

  static void AudioErrorCallback(
      AAudioStream* stream,
      void* userdata,
      aaudio_result_t error);

  xe::threading::Semaphore* semaphore_ = nullptr;

  AAudioStreamBuilder* builder_ = nullptr;
  AAudioStream* stream_ = nullptr;
  bool stream_initialized_ = false;

  static const uint32_t host_frame_frequency_ = 48000;
  static const uint32_t host_frame_channels_ = 2;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t x360_frame_samples_ = 6 * channel_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
  std::atomic<float> volume_{1.0f};
};

}  // namespace aaudio
}  // namespace apu
}  // namespace xe

#endif //AX360E_XE_AAUDIO_AUDIO_DRIVER_H
