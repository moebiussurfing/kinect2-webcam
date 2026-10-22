# kinect2-webcam

**v1.0** · [Release notes](CHANGELOG.md)

Use a Kinect v2 as a webcam on Linux, with colour grading, retro and cinema looks, built-in LUTs,
auto brightness and a live preview.

The service reads the Kinect colour camera through [libfreenect2](https://github.com/OpenKinect/libfreenect2),
processes each frame and writes it to a [v4l2loopback](https://github.com/umlaeute/v4l2loopback)
device, so browsers and video-call apps see a normal camera.

Tested on Ubuntu 26.04 (GNOME, Wayland) with a Kinect v2 at 1920x1080, 30 fps.

## Features

- **Image controls**: brightness, contrast, saturation, temperature, gamma, camera exposure
  (auto compensation or manual time and gain).
- **Auto brightness**: follows the room light by easing a gamma exponent, so highlights do not clip.
- **Looks**: Polaroid, Analog film, Black & white, CRT monitor, 80s TV, Green monitor, Cinema (Teal & orange,
  Warm, Cold), Noir. Each one has an intensity control. Optional 2.39:1 cinema bars.
- **20 factory presets + personal presets**: a dropdown in Look combines ready-made recipes and your saved settings.
  Selecting a preset applies it immediately. Factory recipes combine looks, built-in LUTs and image controls.
- **Card-style accordion**: coloured icons, rounded cards, plus/minus headers, centered icon badges and 20px body insets.
  Webcam and Look start expanded; Colour LUT, Image and Camera start collapsed.
- **20 built-in LUTs**. A LUT stacks with any look. No external LUT file importer in the panel.
- **Live preview** in the control panel. It does not open the camera device, so a video call
  can use the camera at the same time.
- **Cost**: about 2-14 ms per 1080p frame, depending on the effects, on 4 threads.

## Requirements

- Kinect v2 on a USB 3 port.
- [libfreenect2](https://github.com/OpenKinect/libfreenect2) installed under `~/.local/freenect2`
  (or set `FREENECT2_PREFIX`).
- `v4l2loopback` kernel module (`v4l2loopback-dkms` on Ubuntu). With Secure Boot the DKMS module needs a
  signing key enrolled with `mokutil`.
- A C++20 compiler with OpenMP.
- For the control panel: Python 3, GTK 4 and libadwaita (PyGObject).

## Install

```bash
./build.sh                                   # builds ./kinect2-webcam
install -m 755 kinect2-webcam ~/.local/bin/
install -m 755 kinect-webcam-app ~/.local/bin/

# virtual camera at /dev/video10 and access to the Kinect without root
sudo cp packaging/v4l2loopback.conf /etc/modprobe.d/
echo v4l2loopback | sudo tee /etc/modules-load.d/v4l2loopback.conf
sudo cp packaging/60-kinect2.rules /etc/udev/rules.d/ && sudo udevadm control --reload

# user service and launcher
mkdir -p ~/.config/systemd/user ~/.local/share/applications
cp packaging/kinect2-webcam.service ~/.config/systemd/user/
cp packaging/local.manu.KinectWebcam.desktop ~/.local/share/applications/
systemctl --user daemon-reload
# Open Kinect Webcam to start the camera. Login autostart is optional:
# systemctl --user enable kinect2-webcam
```

Opening "Kinect Webcam" from the app menu activates the camera; you can then turn it off and change settings.
This does not change the separate **Start at login** setting. Closing the panel leaves the stream running.
In a browser pick the camera called "Kinect v2". It is only usable while the service is running.

## Configuration

The panel writes `~/.config/kinect2-webcam.conf` and sends `SIGHUP` to the service, which reloads it
without restarting the stream. You can also edit the file by hand (`key = value`, one per line):

| Key | Range / values | Meaning |
|---|---|---|
| `exposure` | -2 to 2 | Camera auto-exposure compensation |
| `manual_exposure`, `exposure_ms`, `gain` | 0/1, 1-66, 1-4 | Fixed exposure time and gain |
| `mirror` | 0/1 | Mirror the picture (default 1) |
| `gamma` | 0.3 to 3 | Luma curve, above 1 darkens midtones |
| `auto_brightness`, `auto_target` | 0/1, 0.25-0.7 | Software auto brightness and its target |
| `brightness`, `contrast`, `saturation`, `temperature` | see panel | Basic grading |
| `look`, `look_strength` | id, 0-1 | Look and its intensity (`kinect2-webcam --list-looks`) |
| `letterbox` | 0/1 | 2.39:1 cinema bars |
| `lut_preset`, `lut_strength` | `none`, id, `file`; 0-1 | Built-in LUT (`kinect2-webcam --list-luts`) or your file |
| `lut` | path | `.cube` 3D LUT used when `lut_preset = file` |

With every setting at its default the output is identical to a plain conversion of the camera frame.

## Saved presets

In **Look**, open the **Preset** dropdown and choose one of the 20 numbered factory styles:
Natural, Soft Portrait, Golden Hour, Cool Morning, Vivid Studio, Pastel Dream, Analog Film,
Vintage Print, Cinema Teal, Cinema Warm, Cinema Cold, Noir, Silver B&W, Sepia Archive, Cyanotype,
80s TV, CRT Arcade, Matrix, Neon Night and Moonlight. Selecting a name applies it immediately, without a separate Load button. Recipes are starting points, not automatic
scene correction; adjust intensity or exposure for your lighting after loading.

Use the **← / →** buttons next to the dropdown to browse factory and personal presets with
immediate application. Navigation stops at the first/last preset (no wrapping).

The header **Day / Night** toggle switches the panel theme. The choice is saved in
`~/.config/kinect2-webcam-ui.json` and restored on the next launch, independently of image settings.

Use **Save** in the Look header to save the current settings under a unique name. Personal entries
appear as **Saved · name** in the same dropdown and persist in `~/.config/kinect2-webcam-presets.json`.
Factory recipes live in the app code, stay separate from your file and cannot be overwritten by saving.
Presets include all image settings (including exposure and mirror), but not webcam on/off, live preview
or login autostart. Loading restores a complete configuration, not cumulative effects.

**Delete preset** below Look removes only the selected personal preset, after confirmation
(Cancel is the default). Factory presets are protected. Deleting leaves the current image untouched
and saves a recovery copy beside the preset file as `kinect2-webcam-presets.json.bak-DATE-TIME`.

The panel no longer imports external LUT files. Existing legacy settings/personal presets are preserved;
if they reference a custom `.cube`, keep it in place to load them. No old files are deleted.
Duplicate personal names are rejected. Invalid files/settings or missing legacy LUTs are reported
without replacing your personal preset file or applying a partial configuration.

## Develop

```bash
./build.sh test      # builds and runs the effects tests, no Kinect needed
/usr/bin/python3 test_gui.py  # preset tests + GTK tests (graphical session needed for the latter)
./build.sh           # builds the service
```

`effects.h` holds the whole image pipeline and has no libfreenect2 dependency, which is what keeps
it testable.

## Notes and limits

- v4l2loopback allows one reader at a time on this setup. The control panel reads a small preview
  file published at native camera resolution by the service instead of the device, to leave the
  camera free for other apps. The panel scales that frame down to fit its viewport.
- Looks and LUTs were tuned by eye on a single scene.
- Only the colour stream is used. Depth is not.
