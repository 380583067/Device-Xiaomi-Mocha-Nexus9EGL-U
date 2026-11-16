/*
 * libcameraservice.so -- empty stub
 * ============================================================================
 * Purpose: only so that camera.vendor.tegra.so (the NVIDIA 2015 camera blob) passes the
 * DT_NEEDED check performed by dlopen.
 *
 * Evidence:
 *   The blob lists libcameraservice.so in DT_NEEDED, but walking all 345 undefined
 *   symbols shows that *none* of them points at CameraService / ICameraService /
 *   CameraHardwareInterface / Camera2Client / ProCamera. It is a leftover from the
 *   Android 5 build where the libcameraservice static library got linked in as a side
 *   effect of --as-needed not being enabled.
 *
 * Why the system cannot provide one:
 *   On Android 14 cameraserver is an executable and no longer produces
 *   libcameraservice.so (verified: the file does not exist under /system/lib).
 *
 * Why the name is mandatory:
 *   The dynamic linker looks files up by name and the name cannot change, so an empty
 *   library with that name is placed in /system/lib/ where the blob's default namespace finds it.
 *
 * Safety:
 *   This library exports no symbols (only an internal keepalive function), so even if
 *   something dlopens it later nothing can be mis-resolved - the worst case is a missing
 *   symbol, never a wrong implementation.
 */

extern "C" void libcameraservice_stub_keepalive(void) {}
