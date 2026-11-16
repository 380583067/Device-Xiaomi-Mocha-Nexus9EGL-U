/*
 * libcameraservice.so —— 空 stub
 * ============================================================================
 * 用途：仅为了让 camera.vendor.tegra.so（NVIDIA 2015 年相机 blob）能通过
 * dlopen 的 DT_NEEDED 检查。
 *
 * 依据：
 *   blob 的 DT_NEEDED 中列出 libcameraservice.so，但遍历 blob 全部 345 个未定义
 *   符号后确认，**没有任何一个**指向 CameraService / ICameraService /
 *   CameraHardwareInterface / Camera2Client / ProCamera 之类的东西 —— 这是当年
 *   Android 5 编译时 libcameraservice 静态库被顺手链接进来留下的痕迹，属于
 *   "--as-needed 没开" 的典型产物。
 *
 * 为什么不能在系统里找到现成的：
 *   Android 14 的 cameraserver 是可执行文件，不再产出 libcameraservice.so
 *   （实测 /system/lib 下确实没有这个文件）。
 *
 * 为什么必须叫这个名字：
 *   动态链接器是按名字找文件，名字改不了。所以只能提供一个同名空库，放在
 *   /system/lib/ 让 blob 的 default namespace 能找到。
 *
 * 安全性：
 *   本库不导出任何符号（只有一个内部 keepalive 函数），所以即使将来别的东西
 *   dlopen 到它，也不会造成错误解析 —— 最坏情况是找不到符号而失败，不会
 *   解析到错误实现。
 */

extern "C" void libcameraservice_stub_keepalive(void) {}
