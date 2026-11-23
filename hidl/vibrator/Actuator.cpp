/*
 * Copyright (C) 2026 Artem Bambalov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.vibrator@1.3-service.mocha"

#include "Actuator.h"

#include <SysfsNode.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <string>

namespace android {
namespace hardware {
namespace vibrator {
namespace V1_3 {
namespace implementation {

static const std::string ENABLE = "/sys/class/timed_output/vibrator/enable";
static const std::string STRENGTH = "/sys/vibrator/pwmvalue";

/* A sequence of strengths and durations, played by the driver rather than by
 * this process. An effect written here keeps its timing whether or not the
 * thread that asked for it is running, which a pair of writes separated by a
 * sleep would not. */
static const std::string PATTERN = "/sys/class/timed_output/vibrator/pattern";

/* What the framework counts strength in. */
static constexpr uint8_t FRAMEWORK_MAX_STRENGTH = 255;

/*
 * Whether writing a pattern makes the driver do anything.
 *
 * The node is created unconditionally, but its store only has a body when the
 * kernel was built with SUPPORT_WRITE_PAT; without it the store returns zero
 * having done nothing, which looks exactly like a zero-length write. Asking
 * for the echo that a real store gives back is therefore the only way to tell
 * the two apart, and it costs one write of a stop pattern that would not have
 * moved the motor anyway.
 */
static bool driverPlaysPatterns() {
    static const char probe[] = {0, 0};

    int fd = open(PATTERN.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;

    ssize_t written = ::write(fd, probe, sizeof(probe));

    close(fd);

    return written == static_cast<ssize_t>(sizeof(probe));
}

Actuator::Actuator()
      : mStrengthAdjustable(mocha::sysfs::writable(STRENGTH)),
        mDriverTimedPatterns(driverPlaysPatterns()) {
    if (!mStrengthAdjustable)
        ALOGW("no %s; effects will play at whatever strength was last set",
              STRENGTH.c_str());

    if (!mDriverTimedPatterns)
        ALOGI("no pattern support; effects are timed here");

    mPlayback = std::thread(&Actuator::playbackLoop, this);
}

Actuator::~Actuator() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mQuit = true;
        ++mGeneration;
    }
    mWake.notify_all();

    if (mPlayback.joinable()) mPlayback.join();
}

bool Actuator::writeStrength(uint8_t amplitude) const {
    if (!mStrengthAdjustable) return true;

    /* The interface counts 1 to 255; the amplifier counts 1 to 127. Map the
     * ends onto each other and space the rest evenly between them. */
    long strength = std::lround((amplitude - 1) / (FRAMEWORK_MAX_STRENGTH - 1.0) *
                                        (MAX_STRENGTH - 1) + 1);

    return mocha::sysfs::write(STRENGTH, static_cast<int>(strength));
}

bool Actuator::writeRawStrength(uint8_t strength) const {
    if (!mStrengthAdjustable) return true;

    if (strength > MAX_STRENGTH) strength = MAX_STRENGTH;

    return mocha::sysfs::write(STRENGTH, static_cast<int>(strength));
}

bool Actuator::startTimer(uint32_t milliseconds) const {
    return mocha::sysfs::write(ENABLE, static_cast<int>(milliseconds));
}

bool Actuator::halt() const {
    return mocha::sysfs::write(ENABLE, 0);
}

bool Actuator::run(uint32_t milliseconds) const {
    /* A run is the framework asking for continuous vibration, so it takes the
     * motor back from any effect still playing, and from the strength that
     * effect left behind. */
    cancelPlayback();
    if (!writeStrength(mFrameworkStrength.load())) return false;

    return startTimer(milliseconds);
}

bool Actuator::stop() const {
    cancelPlayback();

    return halt();
}

bool Actuator::setStrength(uint8_t amplitude) const {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mFrameworkStrength.store(amplitude);
    }

    return writeStrength(amplitude);
}

void Actuator::cancelPlayback() const {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mPending.clear();
        ++mGeneration;
    }
    mWake.notify_all();
}

bool Actuator::play(const std::vector<Step>& steps) const {
    if (steps.empty()) return false;

    if (mDriverTimedPatterns) return playTimedByDriver(steps);

    return playHere(steps);
}

/*
 * The leading byte is the mode the driver reads out of the buffer before the
 * pairs begin; a trailing zero pair ends the sequence. Durations are one byte,
 * so no single step runs longer than 255 ms -- which no effect here wants to.
 */
bool Actuator::playTimedByDriver(const std::vector<Step>& steps) const {
    std::vector<uint8_t> buf;

    buf.push_back(0);
    for (const auto& step : steps) {
        buf.push_back(step.first);
        buf.push_back(step.second);
    }
    buf.push_back(0);
    buf.push_back(0);

    /* A bin_attribute store reports the bytes it consumed only when it has a
     * body, so anything short of the whole buffer means the driver did not
     * take the shape and this process has to play it. */
    if (mocha::sysfs::write(PATTERN, buf.data(), buf.size())) return true;

    return playHere(steps);
}

bool Actuator::playHere(const std::vector<Step>& steps) const {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mPending = steps;
        ++mGeneration;
    }
    mWake.notify_all();

    return true;
}

bool Actuator::waitOrCancel(uint32_t milliseconds, uint32_t generation) const {
    std::unique_lock<std::mutex> lock(mLock);

    return mWake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                          [this, generation] {
                              return mQuit || mGeneration != generation;
                          });
}

void Actuator::playbackLoop() {
    for (;;) {
        std::vector<Step> steps;
        uint32_t generation;

        {
            std::unique_lock<std::mutex> lock(mLock);

            mWake.wait(lock, [this] { return mQuit || !mPending.empty(); });
            if (mQuit) return;

            steps = std::move(mPending);
            mPending.clear();
            generation = mGeneration;
        }

        for (const auto& step : steps) {
            if (step.first == 0) {
                halt();
            } else {
                writeRawStrength(step.first);
                startTimer(step.second);
            }

            if (waitOrCancel(step.second, generation)) break;
        }

        /* The shape is over, whether it ran to the end or was cut short; the
         * motor must not be left on and the strength node must go back to what
         * a plain run expects. */
        halt();
        writeStrength(mFrameworkStrength.load());
    }
}

}  // namespace implementation
}  // namespace V1_3
}  // namespace vibrator
}  // namespace hardware
}  // namespace android
