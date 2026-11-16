/*
 * libpowerservice_client.so -- minimal stub for android::PowerServiceClient
 * ============================================================================
 * Purpose: camera.vendor.tegra.so lists libpowerservice_client.so in DT_NEEDED and
 * actually references two of its symbols:
 *
 *   _ZN7android18PowerServiceClientC1Ev            ( constructor )
 *   _ZN7android18PowerServiceClient13sendPowerHintEiPv ( sendPowerHint(int, void*) )
 *
 * Both symbols are gone in Android 14: PowerServiceClient is a private class NVIDIA added
 * to the framework in the Android 5 era (to forward camera/HWAS power hints to the power
 * HAL); it never existed in AOSP mainline.
 *
 * Implementation strategy
 * ---------------------------------------------------------------------------
 *  - Constructor: really runs the RefBase constructor so the refcounted object is usable
 *    (the blob may manage it as sp<>, and sp's incStrong/decStrong need a valid mRefs).
 *    The object size matches Android 5: RefBase has a single weakref_impl* member, and the
 *    blob allocates 4 bytes (32-bit); this class adds no members, so the layout matches.
 *  - sendPowerHint: returns 0 (NO_ERROR) directly.
 *    A power hint is purely a *performance/power hint* (e.g. raise clocks while recording);
 *    failing it does not affect the camera, at worst encode/preview runs slightly conservative.
 */

#include <stdint.h>

#include <new>

#include <utils/RefBase.h>

namespace mocha_stub {

/*
 * Inherit only RefBase and add no members so that sizeof matches the Android 5
 * PowerServiceClient exactly (4 bytes on 32-bit).
 */
class PowerServiceClient : public android::RefBase {
  public:
    PowerServiceClient() = default;

    // status_t sendPowerHint(int hintId, void* data) -- always returns NO_ERROR
    int sendPowerHint(int /*hintId*/, void* /*data*/) { return 0; }
};

}  // namespace mocha_stub

/*
 * Constructors (C1 = complete object ctor, C2 = base object ctor).
 * C++ constructors return void in the ABI and receive this as the first argument.
 */
extern "C" void _ZN7android18PowerServiceClientC2Ev(void* thiz) {
    new (thiz) mocha_stub::PowerServiceClient();
}

extern "C" void _ZN7android18PowerServiceClientC1Ev(void* thiz) {
    _ZN7android18PowerServiceClientC2Ev(thiz);
}

/*
 * status_t PowerServiceClient::sendPowerHint(int, void*)
 * Ignore the arguments and return 0; see the file header for the reasoning.
 */
extern "C" int _ZN7android18PowerServiceClient13sendPowerHintEiPv(void* /*thiz*/,
                                                                 int /*hintId*/,
                                                                 void* /*data*/) {
    return 0;
}
