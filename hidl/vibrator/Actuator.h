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
#ifndef MOCHA_VIBRATOR_ACTUATOR_H
#define MOCHA_VIBRATOR_ACTUATOR_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace android {
namespace hardware {
namespace vibrator {
namespace V1_3 {
namespace implementation {

/* One step of a pattern: how hard, and for how long. A strength of zero is a
 * pause. */
using Step = std::pair<uint8_t, uint8_t>;

/*
 * The motor, and the three ways this board can drive it.
 *
 * Running for a while is one thing and playing a shape is another, and they
 * do not share a path: a run takes its strength from whatever was last set,
 * while a shape carries its own.
 *
 * A shape is best left to the driver, which keeps its timing whether or not
 * this process is scheduled; /sys/class/timed_output/vibrator/pattern does
 * exactly that, but only when the kernel was built with SUPPORT_WRITE_PAT,
 * and this one was not -- the node exists and its store is compiled out, so
 * every write comes back having written nothing. The constructor therefore
 * asks the node whether it does anything, and when the answer is no the shape
 * is played here instead: one step at a time on the same strength and enable
 * nodes that a plain run uses, from a thread of its own so that the caller is
 * not held for the length of the effect.
 */
class Actuator {
  public:
    Actuator();
    ~Actuator();

    /* Run at the strength last set, for this long. */
    bool run(uint32_t milliseconds) const;
    bool stop() const;

    /* The strength ordinary running uses, on the interface's scale of 1..255. */
    bool setStrength(uint8_t amplitude) const;

    /* Whether the strength can be set at all, asked of the node rather than
     * assumed -- the framework changes what it does with the answer. */
    bool strengthAdjustable() const { return mStrengthAdjustable; }

    /* Play a shape. Returns as soon as the shape is under way. */
    bool play(const std::vector<Step>& steps) const;

    /* Where strength ends. Not a choice: the amplifier takes a signed
     * real-time playback value and its input is configured bidirectional, so
     * this is 100% of rated voltage and there is nothing above it to ask
     * for. The driver clamps to the same number. */
    static constexpr uint8_t MAX_STRENGTH = 127;

  private:
    /* Writes that do not take the lock and do not cancel anything, so the
     * playback thread can use them on itself. */
    bool writeStrength(uint8_t amplitude) const;
    /* A step's amplitude is already on the amplifier's own scale; only a
     * framework amplitude needs the 1..255 -> 1..127 mapping. */
    bool writeRawStrength(uint8_t strength) const;
    bool startTimer(uint32_t milliseconds) const;
    bool halt() const;

    /* Abandon whatever the playback thread is doing, if anything. */
    void cancelPlayback() const;

    /* Hand the shape to the kernel, or to the thread. */
    bool playTimedByDriver(const std::vector<Step>& steps) const;
    bool playHere(const std::vector<Step>& steps) const;

    void playbackLoop();
    /* True when the shape should be abandoned before its next step. */
    bool waitOrCancel(uint32_t milliseconds, uint32_t generation) const;

    const bool mStrengthAdjustable;
    const bool mDriverTimedPatterns;

    /* What the framework asked for. An effect borrows the strength node, so
     * this is what has to be put back before a plain run can use it again. */
    mutable std::atomic<uint8_t> mFrameworkStrength{255};

    mutable std::mutex mLock;
    mutable std::condition_variable mWake;
    /* Guarded by mLock. A generation change is how a new request, a stop, or
     * the destructor tells the playback thread its shape is stale. */
    mutable std::vector<Step> mPending;
    mutable uint32_t mGeneration = 0;
    mutable bool mQuit = false;

    std::thread mPlayback;
};

}  // namespace implementation
}  // namespace V1_3
}  // namespace vibrator
}  // namespace hardware
}  // namespace android

#endif  // MOCHA_VIBRATOR_ACTUATOR_H
