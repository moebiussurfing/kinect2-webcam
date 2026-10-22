# Changelog

## Unreleased

- Increase the initial panel height from 1040 to 1120 pixels so the default controls fit without manual resizing.

## v1.0 — 2026-10-23

First release of Kinect Webcam for Linux, tested on Ubuntu 26.04 with GNOME/Wayland and a Kinect v2.

## Highlights
- Kinect v2 colour camera as a 1920×1080 / 30 fps virtual webcam via libfreenect2 and v4l2loopback.
- Native-resolution live preview, scaled to the panel without opening the virtual camera device.
- Image/exposure controls, retro/cinema looks, 20 built-in colour LUTs and 20 factory presets.
- Persistent personal presets: save, instant loading, previous/next navigation and confirmed deletion with a recovery backup. Factory presets cannot be deleted; deleting a personal preset leaves the current image unchanged.
- Rounded, collapsible control cards with centred icons and padded interiors. Webcam and Look start expanded.
- Day/Night switch with a persistent, app-specific theme preference.
- Opening the panel starts the camera without changing the separate Start at login setting. Closing the panel leaves the video service running.

## Installation and scope
This is a **source release**; GitHub provides source archives below. Follow the repository README to build and install. No precompiled, machine-specific binary is included.

Requires a Kinect v2 on USB 3, libfreenect2, v4l2loopback, a C++20/OpenMP toolchain, Python 3, GTK 4 and libadwaita. Secure Boot may require enrolling the DKMS module signing key. Only the colour camera is supported, not depth. The panel no longer imports external LUT files; existing legacy configurations/presets are preserved.

## Validation
26 isolated GUI/storage tests and the C++ effects test suite pass. The service builds locally. Preview, light/dark layouts and preservation of existing camera settings/personal presets were checked on the target machine.
