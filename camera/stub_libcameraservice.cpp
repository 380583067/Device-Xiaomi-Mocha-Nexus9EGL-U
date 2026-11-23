/*
 * libcameraservice.so - an empty stub
 * ============================================================================
 * Purpose: only so that camera.vendor.tegra.so (the NVIDIA 2015 camera blob)
 * passes the dlopen DT_NEEDED check.
 *
 * Evidence:
 *   The blob lists libcameraservice.so in DT_NEEDED, but walking all 345 of its
 *   undefined symbols shows **none** of them refer to CameraService /
 *   ICameraService / CameraHardwareInterface / Camera2Client / ProCamera. This is a
 *   leftover from the Android 5 build, where the static libcameraservice got linked
 *   in as a side effect - the classic "--as-needed was off" artefact.
 *
 * Why there is no existing one to use:
 *   Android 14's cameraserver is an executable and no longer produces
 *   libcameraservice.so (verified: the file is not in /system/lib).
 *
 * Why the name is mandatory:
 *   The dynamic linker resolves DT_NEEDED by file name and the name cannot change,
 *   so an empty library with the same name in /system/lib is the only way the blob's
 *   default namespace can find it.
 *
 * Safety:
 *   This library exports nothing (one internal keepalive function), so even if
 *   something dlopens it later no wrong resolution is possible - the worst case is a
 *   missing symbol and a clean failure, never a call into a wrong implementation.
 */

extern "C" void libcameraservice_stub_keepalive(void) {}
