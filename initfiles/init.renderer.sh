#!/system/bin/sh
#
# Force GPU (client) composition, and keep it forced.
#
# SurfaceFlinger's raw transaction code 1008 is "Toggle forced GPU
# composition"; the payload is a boolean:
#
#   case 1008: // Toggle forced GPU composition.
#       sfdo_forceClientComposition(data.readInt32() != 0);
#
# Letting the Tegra HWC composite by itself walks this port into
# hwcomposer.tegra.so's glc_set -> create_graphic_buffer and
# NvGrDecompressBuffer paths. Neither survives a full session on Android 14:
# the composer goes down, init counts that and turns it into a userspace
# reboot, and the device never gets past the boot animation. With client
# composition forced, the composer stays out of the way and the GPU draws.
#
# This is not an Android 5 leftover: in this tree the transaction code is
# live, and removing the script is what caused the boot animation loop of
# 2026-11-19. Do not delete it without booting a build without it first.
#
# The cost is GPU work for every frame on a Kepler GK20A driving a
# 1536x2048 panel, which is worth revisiting once the HWC paths are fixed.
service call SurfaceFlinger 1008 i32 1
