// Stream the Kinect v2 color camera to a v4l2loopback device as a webcam.
//
// Usage: kinect2-webcam [/dev/videoN] [--no-mirror] [--list-looks] [--list-luts]
// Requires the v4l2loopback module and libfreenect2.
// Image settings and looks live in effects.h; the GUI writes ~/.config/kinect2-webcam.conf
// and sends SIGHUP so they apply without restarting the stream.
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <libfreenect2/frame_listener_impl.h>
#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/logger.h>
#include <libfreenect2/packet_pipeline.h>

#include "effects.h"

static volatile std::sig_atomic_t g_stop = 0;
static volatile std::sig_atomic_t g_reload = 0;

static void onSignal(int) { g_stop = 1; }
static void onReload(int) { g_reload = 1; }

static std::string configPath() {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  const char *home = std::getenv("HOME");
  std::string base = xdg && *xdg ? xdg : (home ? std::string(home) + "/.config" : ".");
  return base + "/kinect2-webcam.conf";
}

static fx::Settings loadConfig(bool mirrorFromCli) {
  fx::Settings cfg;
  fx::loadSettings(cfg, configPath());
  if (mirrorFromCli) cfg.mirror = false;
  return cfg;
}

// GUI preview: while ~/.../kinect2-webcam.want is fresh (the GUI touches it every couple of
// seconds) the finished frame is published, downscaled, as "KWP1" + w + h + RGB24 so the GUI
// never has to open the loopback device (which would block Meet and friends).
static std::string runtimeFile(const char *name) {
  const char *dir = std::getenv("XDG_RUNTIME_DIR");
  return std::string(dir && *dir ? dir : "/tmp") + "/" + name;
}

class PreviewWriter {
 public:
  PreviewWriter() : want_(runtimeFile("kinect2-webcam.want")), path_(runtimeFile("kinect2-webcam-preview.rgb")) {}

  void publish(const uint8_t *yuyv, int w, int h) {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastCheck_ > std::chrono::milliseconds(500)) {  // cheap freshness check
      lastCheck_ = now;
      struct stat st;
      wanted_ = ::stat(want_.c_str(), &st) == 0 && std::time(nullptr) - st.st_mtime <= 5;
      if (!wanted_ && published_) {  // nobody is looking: do not leave an old frame behind
        std::remove(path_.c_str());
        published_ = false;
      }
    }
    if (!wanted_) return;
    if (now - lastWrite_ < std::chrono::milliseconds(30)) return;  // every frame of a 30 fps camera
    lastWrite_ = now;
    const int pw = 480, ph = 270;
    rgb_.resize(16 + static_cast<size_t>(pw) * ph * 3);
    std::memcpy(rgb_.data(), "KWP1", 4);
    const uint32_t dims[2] = {static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)};
    std::memcpy(rgb_.data() + 4, dims, sizeof(dims));
    fx::makePreview(yuyv, w, h, pw, ph, rgb_.data() + 16);
    const std::string tmp = path_ + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
      const bool ok = std::fwrite(rgb_.data(), 1, rgb_.size(), f) == rgb_.size();
      std::fclose(f);
      if (ok && std::rename(tmp.c_str(), path_.c_str()) == 0)  // atomic: the GUI never sees half a frame
        published_ = true;
    }
  }

  ~PreviewWriter() { std::remove(path_.c_str()); }

 private:
  std::string want_, path_;
  std::vector<uint8_t> rgb_;
  std::chrono::steady_clock::time_point lastWrite_{}, lastCheck_{};
  bool wanted_ = false, published_ = false;
};

static void applyCamera(libfreenect2::Freenect2Device *dev, const fx::Settings &cfg) {
  if (cfg.manualExposure)
    dev->setColorManualExposure(cfg.exposureMs, cfg.gain);
  else
    dev->setColorAutoExposure(cfg.exposure);
}

static int openLoopback(const char *path, int w, int h) {
  const int fd = open(path, O_RDWR);
  if (fd < 0) {
    std::perror(path);
    return -1;
  }
  v4l2_format fmt;
  std::memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = w;
  fmt.fmt.pix.height = h;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  fmt.fmt.pix.bytesperline = w * 2;
  fmt.fmt.pix.sizeimage = w * h * 2;
  fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    std::perror("VIDIOC_S_FMT");
    close(fd);
    return -1;
  }
  return fd;
}

int main(int argc, char **argv) {
  std::string devPath = "/dev/video10";
  bool mirrorFromCli = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-mirror") == 0) {
      mirrorFromCli = true;
    } else if (std::strcmp(argv[i], "--list-looks") == 0) {
      for (const auto &look : fx::looks()) std::printf("%s\t%s\n", look.id, look.label);
      return 0;
    } else if (std::strcmp(argv[i], "--list-luts") == 0) {
      for (const auto &lut : fx::lutPresets()) std::printf("%s\t%s\n", lut.id, lut.label);
      return 0;
    } else {
      devPath = argv[i];
    }
  }

  fx::Settings cfg = loadConfig(mirrorFromCli);
  fx::Pipeline pipeline;
  pipeline.configure(cfg);
  PreviewWriter preview;

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGHUP, onReload);

  libfreenect2::setGlobalLogger(libfreenect2::createConsoleLogger(libfreenect2::Logger::Warning));
  libfreenect2::Freenect2 freenect2;
  if (freenect2.enumerateDevices() == 0) {
    std::fprintf(stderr, "No Kinect v2 found.\n");
    return 1;
  }
  // Only the color stream is used; the CPU pipeline avoids needing OpenGL/OpenCL.
  libfreenect2::Freenect2Device *dev =
      freenect2.openDevice(freenect2.getDefaultDeviceSerialNumber(), new libfreenect2::CpuPacketPipeline());
  if (!dev) {
    std::fprintf(stderr, "Failed to open the Kinect v2.\n");
    return 1;
  }

  libfreenect2::SyncMultiFrameListener listener(libfreenect2::Frame::Color);
  dev->setColorFrameListener(&listener);
  if (!dev->startStreams(true, false)) {
    std::fprintf(stderr, "Failed to start the color stream.\n");
    dev->close();
    return 1;
  }

  applyCamera(dev, cfg);

  int fd = -1;
  std::vector<uint8_t> buf;
  libfreenect2::FrameMap frames;
  std::fprintf(stderr, "Streaming to %s (mirror=%s, exposure=%.1f, gamma=%.2f, look=%s, lut=%s). Ctrl+C to stop.\n",
               devPath.c_str(), cfg.mirror ? "on" : "off", cfg.exposure, cfg.gamma, cfg.look.c_str(),
               cfg.lutPreset.c_str());

  while (!g_stop) {
    if (g_reload) {
      g_reload = 0;
      cfg = loadConfig(mirrorFromCli);
      pipeline.configure(cfg);
      applyCamera(dev, cfg);
      std::fprintf(stderr, "Reloaded settings: exposure=%.1f gamma=%.2f look=%s strength=%.2f lut=%s lut_strength=%.2f auto=%d\n",
                   cfg.exposure, cfg.gamma, cfg.look.c_str(), cfg.lookStrength, cfg.lutPreset.c_str(),
                   cfg.lutStrength, cfg.autoBrightness ? 1 : 0);
    }
    if (!listener.waitForNewFrame(frames, 5000)) {
      std::fprintf(stderr, "Timeout waiting for a frame.\n");
      continue;
    }
    const libfreenect2::Frame *rgb = frames[libfreenect2::Frame::Color];
    if (fd < 0) {
      fd = openLoopback(devPath.c_str(), static_cast<int>(rgb->width), static_cast<int>(rgb->height));
      if (fd < 0) {
        listener.release(frames);
        break;
      }
      buf.resize(rgb->width * rgb->height * 2);
    }
    pipeline.process(rgb->data, static_cast<int>(rgb->width), static_cast<int>(rgb->height),
                     rgb->format == libfreenect2::Frame::BGRX, buf.data());
    if (write(fd, buf.data(), buf.size()) < 0) {
      std::perror("write");
      listener.release(frames);
      break;
    }
    preview.publish(buf.data(), static_cast<int>(rgb->width), static_cast<int>(rgb->height));
    listener.release(frames);
  }

  if (fd >= 0) close(fd);
  dev->stop();
  dev->close();
  return 0;
}
