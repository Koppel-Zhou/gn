// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/profiler/stack_sampling_profiler.h"

#include <algorithm>

#include "base/bind.h"
#include "base/callback.h"
#include "base/memory/singleton.h"
#include "base/profiler/native_stack_sampler.h"
#include "base/synchronization/lock.h"
#include "base/timer/elapsed_timer.h"

namespace base {

// DefaultProfileProcessor ----------------------------------------------------

namespace {

// Singleton class responsible for providing the default processing for profiles
// (i.e. for profiles generated by profilers without their own completed
// callback).
class DefaultProfileProcessor {
 public:
  using CompletedCallback = StackSamplingProfiler::CompletedCallback;

  ~DefaultProfileProcessor();

  static DefaultProfileProcessor* GetInstance();

  // Sets the callback to use for processing profiles captured without a
  // per-profiler completed callback. Pending completed profiles are stored in
  // this object until a non-null callback is provided here. This function is
  // thread-safe.
  void SetCompletedCallback(CompletedCallback callback);

  // Processes |profiles|. This function is thread safe.
  void ProcessProfiles(
      const StackSamplingProfiler::CallStackProfiles& profiles);

 private:
  friend struct DefaultSingletonTraits<DefaultProfileProcessor>;

  DefaultProfileProcessor();

  // Copies the pending profiles from |profiles_| into |profiles|, and clears
  // |profiles_|. This function may be called on any thread.
  void GetAndClearPendingProfiles(
      StackSamplingProfiler::CallStackProfiles* profiles);

  // Gets the current completed callback, with proper locking.
  CompletedCallback GetCompletedCallback() const;

  mutable Lock callback_lock_;
  CompletedCallback default_completed_callback_;

  Lock profiles_lock_;
  StackSamplingProfiler::CallStackProfiles profiles_;

  DISALLOW_COPY_AND_ASSIGN(DefaultProfileProcessor);
};

DefaultProfileProcessor::~DefaultProfileProcessor() {}

// static
DefaultProfileProcessor* DefaultProfileProcessor::GetInstance() {
  return Singleton<DefaultProfileProcessor>::get();
}

void DefaultProfileProcessor::SetCompletedCallback(CompletedCallback callback) {
  {
    AutoLock scoped_lock(callback_lock_);
    default_completed_callback_ = callback;
  }

  if (!callback.is_null()) {
    // Provide any pending profiles to the callback immediately.
    StackSamplingProfiler::CallStackProfiles profiles;
    GetAndClearPendingProfiles(&profiles);
    if (!profiles.empty())
      callback.Run(profiles);
  }
}

void DefaultProfileProcessor::ProcessProfiles(
    const StackSamplingProfiler::CallStackProfiles& profiles) {
  CompletedCallback callback = GetCompletedCallback();

  // Store pending profiles if we don't have a valid callback.
  if (!callback.is_null()) {
    callback.Run(profiles);
  } else {
    AutoLock scoped_lock(profiles_lock_);
    profiles_.insert(profiles_.end(), profiles.begin(), profiles.end());
  }
}

DefaultProfileProcessor::DefaultProfileProcessor() {}

void DefaultProfileProcessor::GetAndClearPendingProfiles(
    StackSamplingProfiler::CallStackProfiles* profiles) {
  profiles->clear();

  AutoLock scoped_lock(profiles_lock_);
  profiles_.swap(*profiles);
}
DefaultProfileProcessor::CompletedCallback
DefaultProfileProcessor::GetCompletedCallback() const {
  AutoLock scoped_lock(callback_lock_);
  return default_completed_callback_;
}

}  // namespace

// StackSamplingProfiler::Module ----------------------------------------------

StackSamplingProfiler::Module::Module() : base_address(nullptr) {}
StackSamplingProfiler::Module::Module(const void* base_address,
                                      const std::string& id,
                                      const FilePath& filename)
    : base_address(base_address), id(id), filename(filename) {}

StackSamplingProfiler::Module::~Module() {}

// StackSamplingProfiler::Frame -----------------------------------------------

StackSamplingProfiler::Frame::Frame(const void* instruction_pointer,
                                    size_t module_index)
    : instruction_pointer(instruction_pointer),
      module_index(module_index) {}

StackSamplingProfiler::Frame::~Frame() {}

// StackSamplingProfiler::CallStackProfile ------------------------------------

StackSamplingProfiler::CallStackProfile::CallStackProfile()
    : preserve_sample_ordering(false) {}

StackSamplingProfiler::CallStackProfile::~CallStackProfile() {}

// StackSamplingProfiler::SamplingThread --------------------------------------

StackSamplingProfiler::SamplingThread::SamplingThread(
    scoped_ptr<NativeStackSampler> native_sampler,
    const SamplingParams& params,
    CompletedCallback completed_callback)
    : native_sampler_(native_sampler.Pass()),
      params_(params),
      stop_event_(false, false),
      completed_callback_(completed_callback) {
}

StackSamplingProfiler::SamplingThread::~SamplingThread() {}

void StackSamplingProfiler::SamplingThread::ThreadMain() {
  PlatformThread::SetName("Chrome_SamplingProfilerThread");

  CallStackProfiles profiles;
  CollectProfiles(&profiles);
  completed_callback_.Run(profiles);
}

// Depending on how long the sampling takes and the length of the sampling
// interval, a burst of samples could take arbitrarily longer than
// samples_per_burst * sampling_interval. In this case, we (somewhat
// arbitrarily) honor the number of samples requested rather than strictly
// adhering to the sampling intervals. Once we have established users for the
// StackSamplingProfiler and the collected data to judge, we may go the other
// way or make this behavior configurable.
bool StackSamplingProfiler::SamplingThread::CollectProfile(
    CallStackProfile* profile,
    TimeDelta* elapsed_time) {
  ElapsedTimer profile_timer;
  CallStackProfile current_profile;
  native_sampler_->ProfileRecordingStarting(&current_profile.modules);
  current_profile.sampling_period = params_.sampling_interval;
  bool burst_completed = true;
  TimeDelta previous_elapsed_sample_time;
  for (int i = 0; i < params_.samples_per_burst; ++i) {
    if (i != 0) {
      // Always wait, even if for 0 seconds, so we can observe a signal on
      // stop_event_.
      if (stop_event_.TimedWait(
              std::max(params_.sampling_interval - previous_elapsed_sample_time,
                       TimeDelta()))) {
        burst_completed = false;
        break;
      }
    }
    ElapsedTimer sample_timer;
    current_profile.samples.push_back(Sample());
    native_sampler_->RecordStackSample(&current_profile.samples.back());
    previous_elapsed_sample_time = sample_timer.Elapsed();
  }

  *elapsed_time = profile_timer.Elapsed();
  current_profile.profile_duration = *elapsed_time;
  native_sampler_->ProfileRecordingStopped();

  if (burst_completed)
    *profile = current_profile;

  return burst_completed;
}

// In an analogous manner to CollectProfile() and samples exceeding the expected
// total sampling time, bursts may also exceed the burst_interval. We adopt the
// same wait-and-see approach here.
void StackSamplingProfiler::SamplingThread::CollectProfiles(
    CallStackProfiles* profiles) {
  if (stop_event_.TimedWait(params_.initial_delay))
    return;

  TimeDelta previous_elapsed_profile_time;
  for (int i = 0; i < params_.bursts; ++i) {
    if (i != 0) {
      // Always wait, even if for 0 seconds, so we can observe a signal on
      // stop_event_.
      if (stop_event_.TimedWait(
              std::max(params_.burst_interval - previous_elapsed_profile_time,
                       TimeDelta())))
        return;
    }

    CallStackProfile profile;
    if (!CollectProfile(&profile, &previous_elapsed_profile_time))
      return;
    profiles->push_back(profile);
  }
}

void StackSamplingProfiler::SamplingThread::Stop() {
  stop_event_.Signal();
}

// StackSamplingProfiler ------------------------------------------------------

StackSamplingProfiler::SamplingParams::SamplingParams()
    : initial_delay(TimeDelta::FromMilliseconds(0)),
      bursts(1),
      burst_interval(TimeDelta::FromMilliseconds(10000)),
      samples_per_burst(300),
      sampling_interval(TimeDelta::FromMilliseconds(100)),
      preserve_sample_ordering(false) {
}

StackSamplingProfiler::StackSamplingProfiler(PlatformThreadId thread_id,
                                             const SamplingParams& params)
    : thread_id_(thread_id), params_(params) {}

StackSamplingProfiler::StackSamplingProfiler(PlatformThreadId thread_id,
                                             const SamplingParams& params,
                                             CompletedCallback callback)
    : thread_id_(thread_id), params_(params), completed_callback_(callback) {}

StackSamplingProfiler::~StackSamplingProfiler() {
  Stop();
  if (!sampling_thread_handle_.is_null())
    PlatformThread::Join(sampling_thread_handle_);
}

void StackSamplingProfiler::Start() {
  scoped_ptr<NativeStackSampler> native_sampler =
      NativeStackSampler::Create(thread_id_);
  if (!native_sampler)
    return;

  CompletedCallback callback =
      !completed_callback_.is_null() ? completed_callback_ :
      Bind(&DefaultProfileProcessor::ProcessProfiles,
           Unretained(DefaultProfileProcessor::GetInstance()));
  sampling_thread_.reset(
      new SamplingThread(native_sampler.Pass(), params_, callback));
  if (!PlatformThread::Create(0, sampling_thread_.get(),
                              &sampling_thread_handle_))
    sampling_thread_.reset();
}

void StackSamplingProfiler::Stop() {
  if (sampling_thread_)
    sampling_thread_->Stop();
}

// static
void StackSamplingProfiler::SetDefaultCompletedCallback(
    CompletedCallback callback) {
  DefaultProfileProcessor::GetInstance()->SetCompletedCallback(callback);
}

// StackSamplingProfiler::Frame global functions ------------------------------

bool operator==(const StackSamplingProfiler::Frame &a,
                const StackSamplingProfiler::Frame &b) {
  return a.instruction_pointer == b.instruction_pointer &&
      a.module_index == b.module_index;
}

bool operator<(const StackSamplingProfiler::Frame &a,
               const StackSamplingProfiler::Frame &b) {
  return (a.module_index < b.module_index) ||
      (a.module_index == b.module_index &&
       a.instruction_pointer < b.instruction_pointer);
}

}  // namespace base
