/*
 * libpowerservice_client.so - a minimal Android::PowerServiceClient stub
 * ============================================================================
 * Purpose: camera.vendor.tegra.so needs libpowerservice_client.so in DT_NEEDED and
 * really references two of its symbols:
 *
 *   _ZN7android18PowerServiceClientC1Ev            ( constructor )
 *   _ZN7android18PowerServiceClient13sendPowerHintEiPv ( sendPowerHint(int, void*) )
 *
 * Both symbols are gone in Android 14: PowerServiceClient was a private class NVIDIA
 * added to the framework in the Android 5 era (to forward camera/HWAS power hints to
 * the power HAL); AOSP mainline never had it.
 *
 * Strategy
 * ---------------------------------------------------------------------------
 *  - Constructor: really run RefBase's constructor so the refcounted object is
 *    usable (the blob may hold it as an sp<>, and incStrong/decStrong need a valid
 *    mRefs). The object size matches Android 5: RefBase has one weakref_impl*
 *    member, the blob allocates 4 bytes (32-bit) and this class adds nothing.
 *  - sendPowerHint: return 0 (NO_ERROR).
 *    A power hint is a pure **performance/power nudge** (raise clocks while
 *    recording, say); failing it breaks no camera function, at worst encoding and
 *    preview run a little conservative.
 */

#include <stdint.h>

#include <new>

#include <utils/RefBase.h>

namespace mocha_stub {

/*
 * Inherit only RefBase and add no members, so sizeof matches Android 5's
 * PowerServiceClient exactly (4 bytes on 32-bit).
 */
class PowerServiceClient : public android::RefBase {
  public:
    PowerServiceClient() = default;

    // status_t sendPowerHint(int hintId, void* data) - always returns NO_ERROR
    int sendPowerHint(int /*hintId*/, void* /*data*/) { return 0; }
};

}  // namespace mocha_stub

/*
 * Constructors (C1 = complete object ctor, C2 = base object ctor).
 * In the C++ ABI a constructor returns void and takes this as its first argument.
 */
extern "C" void _ZN7android18PowerServiceClientC2Ev(void* thiz) {
    new (thiz) mocha_stub::PowerServiceClient();
}

extern "C" void _ZN7android18PowerServiceClientC1Ev(void* thiz) {
    _ZN7android18PowerServiceClientC2Ev(thiz);
}

/*
 * status_t PowerServiceClient::sendPowerHint(int, void*)
 * Ignore the arguments and return 0, see the file header for why.
 */
extern "C" int _ZN7android18PowerServiceClient13sendPowerHintEiPv(void* /*thiz*/,
                                                                 int /*hintId*/,
                                                                 void* /*data*/) {
    return 0;
}
