/*
 * libpowerservice_client.so —— Android::PowerServiceClient 最小 stub
 * ============================================================================
 * 用途：camera.vendor.tegra.so 的 DT_NEEDED 里要求 libpowerservice_client.so，
 * 并且确实引用了它的两个符号：
 *
 *   _ZN7android18PowerServiceClientC1Ev            ( 构造函数 )
 *   _ZN7android18PowerServiceClient13sendPowerHintEiPv ( sendPowerHint(int, void*) )
 *
 * 这两个符号在 Android 14 里彻底消失了：PowerServiceClient 是 NVIDIA 在
 * Android 5 时代加进 framework 的一个私有类（用于把相机/HWAS 的电源提示
 * 转发给 power HAL），AOSP 主线从来没有过。
 *
 * 实现策略
 * ---------------------------------------------------------------------------
 *  - 构造函数：真的走 RefBase 的构造，保证引用计数对象是可用的（blob 可能会
 *    把它当 sp<> 管理，sp 的 incStrong/decStrong 需要 mRefs 合法）。
 *    对象大小与 Android 5 一致：RefBase 只有一个 weakref_impl* 成员，
 *    blob 是按 4 字节（32 位）分配的，我们的类不增加任何成员，布局吻合。
 *  - sendPowerHint：直接返回 0（NO_ERROR）。
 *    power hint 是纯粹的**性能/功耗提示**（比如录像时提示提频），失败不影响
 *    相机功能，最坏情况是编码/预览性能略保守。
 */

#include <stdint.h>

#include <new>

#include <utils/RefBase.h>

namespace mocha_stub {

/*
 * 只继承 RefBase、不加任何成员，保证 sizeof 与 Android 5 的
 * PowerServiceClient 完全一致（32 位下 4 字节）。
 */
class PowerServiceClient : public android::RefBase {
  public:
    PowerServiceClient() = default;

    // status_t sendPowerHint(int hintId, void* data) —— 恒返回 NO_ERROR
    int sendPowerHint(int /*hintId*/, void* /*data*/) { return 0; }
};

}  // namespace mocha_stub

/*
 * 构造函数（C1 = complete object ctor，C2 = base object ctor）。
 * C++ 构造函数在 ABI 上返回 void，this 走第一个参数。
 */
extern "C" void _ZN7android18PowerServiceClientC2Ev(void* thiz) {
    new (thiz) mocha_stub::PowerServiceClient();
}

extern "C" void _ZN7android18PowerServiceClientC1Ev(void* thiz) {
    _ZN7android18PowerServiceClientC2Ev(thiz);
}

/*
 * status_t PowerServiceClient::sendPowerHint(int, void*)
 * 忽略参数、返回 0，理由见文件头。
 */
extern "C" int _ZN7android18PowerServiceClient13sendPowerHintEiPv(void* /*thiz*/,
                                                                 int /*hintId*/,
                                                                 void* /*data*/) {
    return 0;
}
