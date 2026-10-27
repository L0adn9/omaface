// omaface-engine — IR face enrollment and verification for omaFace on Omarchy.
//
// Modes:
//   serve                      one verification over the socket on stdin/stdout
//                              (systemd omaface.socket, Accept=yes)
//   enroll <user> [--ui]       capture a new appearance (root only)
//   test <user> [--ui]         print live match scores, never authenticates
//   probe                      print the IR camera that would be used
//
// The engine never grants anything by itself. PAM only trusts omaface-pam,
// which exits 0 solely when this program answers "RESULT ok" on the socket.

#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include "omaface/core.hpp"

#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <systemd/sd-login.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace omaface;

#ifndef OMAFACE_CONFIG
#define OMAFACE_CONFIG "/etc/omaface/config"
#endif
const char* kConfigPath = OMAFACE_CONFIG;
// Overridable so a development build can run against scratch directories.
#ifndef OMAFACE_DATA_DIR
#define OMAFACE_DATA_DIR "/var/lib/omaface"
#endif
#ifndef OMAFACE_RUN_DIR
#define OMAFACE_RUN_DIR "/run/omaface"
#endif
const char* kDataDir = OMAFACE_DATA_DIR;
const char* kRunDir = OMAFACE_RUN_DIR;
#ifndef OMAFACE_MODEL_DIR
#define OMAFACE_MODEL_DIR "/usr/local/share/omaface/models"
#endif
const char* kModelDir = OMAFACE_MODEL_DIR;

const int kMaxAppearances = 3;
const int kLockoutFailures = 5;
const int kLockoutFailuresRgb = 3;  // a webcam is easier to fool; fewer tries
const int kLockoutSeconds = 300;

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// ------------------------------------------------------------------ config

struct Config {
  std::string device = "auto";
  bool lock = true;
  bool login = false;
  bool sudo = true;
  bool polkit = true;
  double threshold = 0.55;      // IR genuine scores measured 0.58-0.72 (top-5 mean)
  int matchFrames = 2;
  double timeoutSudo = 4.0;
  double timeoutPolkit = 5.0;
  double timeoutLock = 6.0;
  double timeoutLogin = 6.0;
  int minFaceWidth = 70;
  bool attention = true;
  bool confirm = true;          // sudo and polkit wait for the person to choose omaFace
  double timeoutChoose = 30.0;
  bool rgbFallback = true;      // no IR camera: use the regular webcam
  bool headTurns = false;       // webcam: also ask for random head turns (strictest)
  double thresholdRgb = 0.60;  // webcam genuine scores measured 0.78-0.88
};

bool parseBool(const std::string& v, bool fallback) {
  if (v == "on" || v == "yes" || v == "true" || v == "1") return true;
  if (v == "off" || v == "no" || v == "false" || v == "0") return false;
  return fallback;
}

Config loadConfig() {
  Config c;
  std::ifstream in(kConfigPath);
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      s.erase(0, s.find_first_not_of(" \t"));
      s.erase(s.find_last_not_of(" \t\r") + 1);
      return s;
    };
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    try {
      if (k == "device") c.device = v;
      else if (k == "lock") c.lock = parseBool(v, c.lock);
      else if (k == "login") c.login = parseBool(v, c.login);
      else if (k == "sudo") c.sudo = parseBool(v, c.sudo);
      else if (k == "polkit") c.polkit = parseBool(v, c.polkit);
      else if (k == "threshold") c.threshold = std::clamp(std::stod(v), 0.30, 0.95);
      else if (k == "attention") c.attention = parseBool(v, true);
      else if (k == "confirm") c.confirm = parseBool(v, true);
      else if (k == "rgb_fallback") c.rgbFallback = parseBool(v, true);
      else if (k == "head_turns") c.headTurns = parseBool(v, false);
      else if (k == "threshold_rgb") c.thresholdRgb = std::clamp(std::stod(v), 0.30, 0.95);
      else if (k == "timeout_choose") c.timeoutChoose = std::clamp(std::stod(v), 5.0, 120.0);
      else if (k == "match_frames") c.matchFrames = std::clamp(std::stoi(v), 1, 5);
      else if (k == "timeout_sudo") c.timeoutSudo = std::clamp(std::stod(v), 1.0, 15.0);
      else if (k == "timeout_polkit") c.timeoutPolkit = std::clamp(std::stod(v), 1.0, 15.0);
      else if (k == "timeout_lock") c.timeoutLock = std::clamp(std::stod(v), 1.0, 15.0);
      else if (k == "timeout_login") c.timeoutLogin = std::clamp(std::stod(v), 1.0, 15.0);
      else if (k == "min_face_width") c.minFaceWidth = std::clamp(std::stoi(v), 30, 300);
    } catch (...) {
      // A malformed value keeps the default rather than failing open or closed.
    }
  }
  return c;
}

// ------------------------------------------------------------------ camera

struct CameraInfo {
  std::string path;
  std::string card;
  uint32_t fourcc = 0;
  uint32_t width = 0, height = 0;
  int score = 0;  // higher = more certainly the IR camera
  bool ir = true; // false: a regular colour webcam (see rgb_fallback)
};

bool xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do r = ioctl(fd, req, arg); while (r == -1 && errno == EINTR);
  return r != -1;
}

std::string fourccName(uint32_t f) {
  std::string s;
  for (int i = 0; i < 4; i++) s += char((f >> (8 * i)) & 0xff);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// "IR camera", "Integrated IR", "Infrared": a whole word, not just the two
// letters (they sit inside ordinary words like "Firmware").
bool hasIrWord(const std::string& text) {
  std::string word;
  for (char ch : text + " ") {
    if (isalnum((unsigned char)ch)) { word += (char)tolower((unsigned char)ch); continue; }
    if (word == "ir" || word == "infrared" || word.rfind("ircam", 0) == 0) return true;
    word.clear();
  }
  return false;
}

// The frame size nearest 640 wide: enough for a face at arm's length, small
// enough to keep each frame cheap. Some drivers list nothing; S_FMT adjusts.
void pickSize(int fd, uint32_t fourcc, uint32_t& w, uint32_t& h) {
  w = 640;
  h = 480;
  v4l2_frmsizeenum fs{};
  fs.pixel_format = fourcc;
  long best = -1;
  for (fs.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs); fs.index++) {
    if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
      w = std::clamp<uint32_t>(640, fs.stepwise.min_width, fs.stepwise.max_width);
      h = std::clamp<uint32_t>(480, fs.stepwise.min_height, fs.stepwise.max_height);
      return;
    }
    if (fs.discrete.width < 240) continue;
    long d = std::labs((long)fs.discrete.width - 640);
    if (best < 0 || d < best) {
      best = d;
      w = fs.discrete.width;
      h = fs.discrete.height;
    }
  }
}

// IR cameras deliver greyscale (GREY, or Y16 on some). A few deliver YUYV,
// like ordinary colour webcams; those only count when the name says IR,
// because a colour camera can be fooled by a photo. `colour` also accepts a
// regular webcam (YUYV or MJPG) as a non-IR camera: the rgb_fallback, which
// adds a head-turn check against photos. `forced` (device= in the config)
// accepts any capture device.
bool inspectDevice(const std::string& path, CameraInfo& out, bool forced = false, bool colour = false) {
  int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = false;
  v4l2_capability cap{};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
      bool grey = false, y16 = false, yuyv = false, mjpg = false;
      v4l2_fmtdesc fmt{};
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      for (fmt.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fmt); fmt.index++) {
        if (fmt.pixelformat == V4L2_PIX_FMT_GREY) grey = true;
        else if (fmt.pixelformat == V4L2_PIX_FMT_Y16) y16 = true;
        else if (fmt.pixelformat == V4L2_PIX_FMT_YUYV) yuyv = true;
        else if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG) mjpg = true;
      }
      std::string card = reinterpret_cast<const char*>(cap.card);
      // uvcvideo names nodes "<product>: <stream>"; the stream part is the
      // stronger hint, since one product often has a colour and an IR node.
      size_t colon = card.rfind(':');
      bool irStream = hasIrWord(colon == std::string::npos ? card : card.substr(colon + 1));
      bool irProduct = colon != std::string::npos && hasIrWord(card.substr(0, colon));
      out.ir = grey || y16 || (yuyv && (irStream || irProduct));
      out.fourcc = grey ? V4L2_PIX_FMT_GREY
                 : y16 ? V4L2_PIX_FMT_Y16
                 : (yuyv && out.ir) ? V4L2_PIX_FMT_YUYV
                 : (forced || colour) ? (yuyv ? V4L2_PIX_FMT_YUYV : mjpg ? V4L2_PIX_FMT_MJPEG : 0) : 0;
      if (out.fourcc) {
        out.path = path;
        out.card = card;
        out.score = (grey || y16 ? 4 : 0) + (irStream ? 2 : 0) + (irProduct ? 1 : 0);
        pickSize(fd, out.fourcc, out.width, out.height);
        ok = true;
      }
    }
  }
  ::close(fd);
  return ok;
}

bool findCamera(const Config& cfg, CameraInfo& out) {
  if (cfg.device != "auto") return inspectDevice(cfg.device, out, true);
  bool any = false;
  for (int i = 0; i < 64; i++) {
    CameraInfo info;
    if (!inspectDevice("/dev/video" + std::to_string(i), info)) continue;
    if (!any || info.score > out.score) out = info;
    any = true;
  }
  if (any || !cfg.rgbFallback) return any;
  // No IR camera, and the person chose the regular webcam: the first one
  // (the built-in camera comes first on laptops).
  for (int i = 0; i < 64; i++)
    if (inspectDevice("/dev/video" + std::to_string(i), out, false, true)) return true;
  return false;
}

class Camera {
 public:
  ~Camera() { close(); }

  bool open(const CameraInfo& info, std::string& err) {
    fd_ = ::open(info.path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { err = std::string("cannot open camera: ") + strerror(errno); return false; }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = info.width;
    fmt.fmt.pix.height = info.height;
    fmt.fmt.pix.pixelformat = info.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (!xioctl(fd_, VIDIOC_S_FMT, &fmt) || fmt.fmt.pix.pixelformat != info.fourcc) {
      err = errno == EBUSY ? "camera busy" : "camera refused the " + fourccName(info.fourcc) + " format";
      return false;
    }
    fourcc_ = info.fourcc;
    ir_ = info.ir;
    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    uint32_t bpp = fourcc_ == V4L2_PIX_FMT_GREY ? 1 : 2;
    stride_ = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width_ * bpp;

    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (!xioctl(fd_, VIDIOC_REQBUFS, &req) || req.count < 2) {
      err = errno == EBUSY ? "camera busy" : "camera buffer request failed";
      return false;
    }
    for (uint32_t i = 0; i < req.count; i++) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (!xioctl(fd_, VIDIOC_QUERYBUF, &buf)) { err = "camera buffer query failed"; return false; }
      void* p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
      if (p == MAP_FAILED) { err = "camera mmap failed"; return false; }
      buffers_.push_back({p, buf.length});
      if (!xioctl(fd_, VIDIOC_QBUF, &buf)) { err = "camera queue failed"; return false; }
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!xioctl(fd_, VIDIOC_STREAMON, &type)) {
      err = errno == EBUSY ? "camera busy" : "camera stream failed";
      return false;
    }
    streaming_ = true;
    // Until a face is found: where the last one was (the camera keeps the
    // region between scans), or the first time, the middle of the frame.
    if (ir_) {
      cv::Rect last = region();
      if (last.area() >= 48 * 48 && (last & cv::Rect(0, 0, width_, height_)) == last) meter(last);
      else meter(cv::Rect(width_ / 4, height_ / 5, width_ / 2, height_ * 3 / 5));
    }
    return true;
  }

  // Keep the exposure metered on the face as it moves; a region that still
  // mostly covers it is left alone.
  void followFace(const cv::Rect& face) {
    if (!ir_ || !meterable_) return;
    searching_ = 0;
    cv::Rect overlap = face & metered_;
    if (overlap.area() < 0.6 * face.area() || metered_.area() > 3 * face.area()) meter(face);
  }

  // No face in a lit frame. A face outside the metered region can be blown
  // out to white by an exposure set for the room (sitting low, the screen
  // tilted back: the middle of the frame is then the wall behind), and a
  // white blob is no face to follow. So after about 1.2 s without one (the
  // exposure takes about a second to settle), meter somewhere else: the
  // lower middle, then the middle, in turn. Metering the whole frame does
  // not help: a bright window brightens nothing.
  void searchFace() {
    const int kHold = 18;  // lit frames
    if (!ir_ || !meterable_ || ++searching_ % kHold != 0) return;
    const int w = width_, h = height_;
    const cv::Rect places[] = {cv::Rect(w / 4, h * 2 / 5, w / 2, h * 3 / 5), cv::Rect(w / 4, h / 5, w / 2, h * 3 / 5)};
    meter(places[(searching_ / kHold - 1) % 2]);
  }

  // Returns false on timeout or error. `out` is always greyscale; `colour`,
  // when asked for, is the BGR frame the face models take (an IR frame is
  // simply repeated in all three channels).
  bool grab(cv::Mat& out, int timeoutMs, cv::Mat* colour = nullptr) {
    pollfd p{fd_, POLLIN, 0};
    int r;
    do r = poll(&p, 1, timeoutMs); while (r < 0 && errno == EINTR);
    if (r <= 0) return false;
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (!xioctl(fd_, VIDIOC_DQBUF, &buf)) return false;
    uint32_t bpp = fourcc_ == V4L2_PIX_FMT_GREY ? 1 : 2;
    bool ok = fourcc_ == V4L2_PIX_FMT_MJPEG ? buf.bytesused > 0 : buf.bytesused >= stride_ * (height_ - 1) + width_ * bpp;
    if (ok) {
      void* data = buffers_[buf.index].first;
      if (!ir_ && fourcc_ == V4L2_PIX_FMT_MJPEG) {
        cv::Mat bgr = cv::imdecode(cv::Mat(1, (int)buf.bytesused, CV_8UC1, data), cv::IMREAD_COLOR);
        ok = !bgr.empty();
        if (ok) {
          cv::cvtColor(bgr, out, cv::COLOR_BGR2GRAY);
          if (colour) *colour = bgr;
        }
      } else if (!ir_) {
        cv::Mat bgr;
        cv::cvtColor(cv::Mat(height_, width_, CV_8UC2, data, stride_), bgr, cv::COLOR_YUV2BGR_YUYV);
        cv::cvtColor(bgr, out, cv::COLOR_BGR2GRAY);
        if (colour) *colour = bgr;
      } else if (fourcc_ == V4L2_PIX_FMT_GREY) {
        cv::Mat(height_, width_, CV_8UC1, data, stride_).copyTo(out);
      } else if (fourcc_ == V4L2_PIX_FMT_YUYV) {
        cv::extractChannel(cv::Mat(height_, width_, CV_8UC2, data, stride_), out, 0);
      } else {
        // Y16 carries 10, 12 or 16 significant bits; find out from the
        // brightest value seen and keep the scale steady between frames.
        cv::Mat wide(height_, width_, CV_16UC1, data, stride_);
        double maxv = 0;
        cv::minMaxLoc(wide, nullptr, &maxv);
        int bits = maxv < 1024 ? 10 : (maxv < 4096 ? 12 : 16);
        y16Bits_ = std::max(y16Bits_, bits);
        wide.convertTo(out, CV_8U, 255.0 / ((1 << y16Bits_) - 1));
      }
      if (ok && ir_ && colour) cv::cvtColor(out, *colour, cv::COLOR_GRAY2BGR);
    }
    xioctl(fd_, VIDIOC_QBUF, &buf);
    return ok;
  }

  uint32_t width() const { return width_; }
  bool ir() const { return ir_; }

 private:
  // UVC 1.5 cameras, many IR modules among them, meter auto-exposure on a
  // region of interest. Left as it powers up, one metered a 1x1 spot in the
  // dark top-left corner and blew a face at arm's length out to white, with
  // no features to find; Windows Hello points the region at the face. The
  // camera keeps the setting after it closes, so the next scan starts
  // exposed for the face too. Cameras without the control just ignore this.
  void meter(cv::Rect r) {
    r &= cv::Rect(0, 0, int(width_), int(height_));
    if (r.area() < 64) return;
    v4l2_rect rect{r.x, r.y, uint32_t(r.width), uint32_t(r.height)};
    v4l2_ext_control c{};
    c.id = V4L2_CID_UVC_REGION_OF_INTEREST_RECT;
    c.size = sizeof rect;
    c.p_rect = &rect;
    v4l2_ext_controls cs{};
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = &c;
    if (!xioctl(fd_, VIDIOC_S_EXT_CTRLS, &cs)) { meterable_ = false; return; }
    metered_ = r;
    // The region only steers auto-exposure once this bit is on (and the
    // camera refuses it while the region is still its 1x1 default).
    v4l2_control autoCtrl{V4L2_CID_UVC_REGION_OF_INTEREST_AUTO, V4L2_UVC_REGION_OF_INTEREST_AUTO_EXPOSURE};
    if (!meteringOn_) meteringOn_ = xioctl(fd_, VIDIOC_S_CTRL, &autoCtrl);
  }

  // The region the camera meters now; empty when it has none.
  cv::Rect region() {
    v4l2_rect rect{};
    v4l2_ext_control c{};
    c.id = V4L2_CID_UVC_REGION_OF_INTEREST_RECT;
    c.size = sizeof rect;
    c.p_rect = &rect;
    v4l2_ext_controls cs{};
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = &c;
    if (!xioctl(fd_, VIDIOC_G_EXT_CTRLS, &cs)) return {};
    return cv::Rect(rect.left, rect.top, int(rect.width), int(rect.height));
  }

  bool meterable_ = true, meteringOn_ = false;
  cv::Rect metered_;
  int searching_ = 0;  // lit frames in a row without a face

 public:

  void close() {
    if (fd_ < 0) return;
    if (streaming_) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      xioctl(fd_, VIDIOC_STREAMOFF, &type);
      streaming_ = false;
    }
    for (auto& b : buffers_) munmap(b.first, b.second);
    buffers_.clear();
    ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
  bool streaming_ = false;
  uint32_t fourcc_ = V4L2_PIX_FMT_GREY;
  bool ir_ = true;
  int y16Bits_ = 10;
  uint32_t width_ = 0, height_ = 0, stride_ = 0;
  std::vector<std::pair<void*, size_t>> buffers_;
};

// min_face_width is set for a 640-pixel-wide camera; scale it so a face at
// the same distance counts the same on a smaller or larger sensor.
int minFaceFor(const Config& cfg, const Camera& cam) {
  return std::max(30, int(cfg.minFaceWidth * (cam.width() ? cam.width() : 640) / 640.0));
}

// The emitter lights every other frame. Unlit frames only carry ambient IR,
// which ranges from black at night to bright in sunlight, so compare against
// the recent lit level instead of a fixed cutoff. With strong ambient light
// the two converge and every frame is simply treated as usable.
class DarkFrameFilter {
 public:
  bool isDark(const cv::Mat& grey) {
    cv::Mat small;
    cv::resize(grey, small, cv::Size(64, 40), 0, 0, cv::INTER_AREA);
    double mean = cv::mean(small)[0];
    if (mean < 3.0) return true;
    if (litLevel_ > 0 && mean < litLevel_ * 0.45) return true;
    litLevel_ = litLevel_ <= 0 ? mean : litLevel_ * 0.7 + mean * 0.3;
    return false;
  }

 private:
  double litLevel_ = 0;
};

// An IR frame between the emitter's flashes, or a webcam frame too dark to
// show a face (lens covered, lights off).
bool unusable(const Camera& cam, DarkFrameFilter& filter, const cv::Mat& grey) {
  if (cam.ir()) return filter.isDark(grey);
  return cv::mean(grey)[0] < 12.0;
}

// ------------------------------------------------------------------ gallery

std::string userDir(const std::string& user) { return std::string(kDataDir) + "/" + user; }
std::string galleryPath(const std::string& user) { return userDir(user) + "/faces.yml"; }

std::vector<Appearance> loadGallery(const std::string& user) {
  if (access(galleryPath(user).c_str(), R_OK) != 0) return {};
  return readGallery(galleryPath(user));
}

bool saveGallery(const std::string& user, const std::vector<Appearance>& gallery, std::string& err) {
  passwd* pw = getpwnam(user.c_str());
  if (!pw) { err = "unknown user"; return false; }
  mkdir(kDataDir, 0755);
  std::string dir = userDir(user);
  mkdir(dir.c_str(), 0750);
  // root owns the template so a process running as the user cannot swap in
  // someone else's face; the user's group may read it for the lock screen.
  // (Only root can hand the files to root; a development build running as
  // the user keeps them its own.)
  if ((geteuid() == 0 && chown(dir.c_str(), 0, pw->pw_gid) != 0) || chmod(dir.c_str(), 0750) != 0) {
    err = "cannot set data directory permissions";
    return false;
  }
  std::string tmp = galleryPath(user) + ".tmp";
  if (!writeGallery(tmp, gallery, err)) return false;
  if ((geteuid() == 0 && chown(tmp.c_str(), 0, pw->pw_gid) != 0) || chmod(tmp.c_str(), 0640) != 0 ||
      rename(tmp.c_str(), galleryPath(user).c_str()) != 0) {
    unlink(tmp.c_str());
    err = "cannot install gallery";
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ system checks

bool lidClosed() {
  DIR* d = opendir("/proc/acpi/button/lid");
  if (!d) return false;
  bool closed = false;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::ifstream f(std::string("/proc/acpi/button/lid/") + e->d_name + "/state");
    std::string s((std::istreambuf_iterator<char>(f)), {});
    if (s.find("closed") != std::string::npos) closed = true;
  }
  closedir(d);
  return closed;
}

// sudo and polkit only accept a face while the user is physically at this
// machine: an active, non-remote logind session on a seat. The login screen
// has no user session yet, so there any active local seat session (the
// greeter's) will do: pass uid = -1.
bool hasActiveLocalSession(uid_t uid) {
  char** sessions = nullptr;
  int n = uid == (uid_t)-1 ? sd_get_sessions(&sessions) : sd_uid_get_sessions(uid, 1, &sessions);
  if (n <= 0 || !sessions) return false;
  bool found = false;
  for (int i = 0; i < n; i++) {
    const char* id = sessions[i];
    char* seat = nullptr;
    char* state = nullptr;
    if (!found && sd_session_is_active(id) > 0 && sd_session_is_remote(id) == 0 &&
        sd_session_get_seat(id, &seat) >= 0 && seat && *seat &&
        sd_session_get_state(id, &state) >= 0 && state && strcmp(state, "active") == 0)
      found = true;
    free(seat);
    free(state);
    free(sessions[i]);
  }
  free(sessions);
  return found;
}

std::string failPath(uid_t uid) { return std::string(kRunDir) + "/fail-" + std::to_string(uid); }

// Consecutive "a face was seen and it was not you" results.
// Rejected faces, per user. IR strikes wear off after kLockoutSeconds.
// Webcam strikes are sticky: once paused, omaFace stays off until the
// password is used (RESET from the lock screen) or the machine restarts,
// so a photo or video gets three tries, not three every five minutes.
struct Failures { int count = 0; time_t last = 0; bool sticky = false; };

Failures readFailures(uid_t uid) {
  Failures f;
  std::ifstream in(failPath(uid));
  int sticky = 0;
  in >> f.count >> f.last;
  if (!in) return Failures{};
  if (in >> sticky) f.sticky = sticky != 0;
  if (!f.sticky && f.last && time(nullptr) - f.last > kLockoutSeconds) f = Failures{};
  return f;
}

void writeFailures(uid_t uid, const Failures& f) {
  std::string path = failPath(uid);
  if (f.count == 0) { unlink(path.c_str()); return; }
  std::ofstream out(path, std::ios::trunc);
  out << f.count << " " << f.last << " " << (f.sticky ? 1 : 0) << "\n";
}

// Live state for the lock screen and polkit dialog animation. World-readable,
// holds no biometric data: just what the scanner is doing right now.
void writeStatus(const std::string& service, const std::string& state, long seq) {
  std::string path = std::string(kRunDir) + "/status";
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  fchmod(fd, 0644);
  char buf[256];
  int n = snprintf(buf, sizeof buf, "{\"service\":\"%s\",\"state\":\"%s\",\"seq\":%ld,\"at\":%ld}\n",
                   service.c_str(), state.c_str(), seq, (long)time(nullptr));
  if (n > 0) (void)!write(fd, buf, std::min<int>(n, sizeof buf - 1));
  ::close(fd);
}

// ------------------------------------------------------------------ serve

struct Request {
  std::string user;
  std::string service;
};

bool readLine(int fd, std::string& line, int timeoutMs) {
  auto start = Clock::now();
  line.clear();
  while (line.size() < 256) {
    int left = timeoutMs - int(secondsSince(start) * 1000);
    if (left <= 0) return false;
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, left) <= 0) return false;
    char c;
    ssize_t r = read(fd, &c, 1);
    if (r <= 0) return false;
    if (c == '\n') return true;
    line.push_back(c);
  }
  return false;
}

void sendLine(const std::string& s) {
  std::string l = s + "\n";
  ssize_t r = send(STDOUT_FILENO, l.data(), l.size(), MSG_NOSIGNAL);
  if (r < 0) (void)!write(STDOUT_FILENO, l.data(), l.size());
}

bool clientGone() {
  pollfd p{STDIN_FILENO, POLLIN | POLLRDHUP, 0};
  if (poll(&p, 1, 0) <= 0) return false;
  if (p.revents & (POLLHUP | POLLRDHUP | POLLERR)) return true;
  char c;
  return recv(STDIN_FILENO, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

int finish(const std::string& result) {
  sendLine("RESULT " + result);
  return result == "ok" ? 0 : 1;
}

// ------------------------------------------------------------------ webcam liveness

// The webcam's head-turn check (omaface-core's HeadTurns): the camera, the
// prompts and the randomness are here, the rules in the core. Returns its
// result, or "gone" when the client left.
std::string headTurnChallenge(Camera& cam, FaceModels& models, const std::vector<Appearance>& gallery,
                              double threshold, int minFace, const std::string& key,
                              long seq, const Face& matched) {
  unsigned char rnd[9];
  if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) return "lost";
  auto start = Clock::now();
  HeadTurns turns(matched, rnd, 0,
                  [&](const cv::Mat& bgr, const Face& f) { return bestScore(gallery, models.feature(bgr, f)); },
                  threshold, minFace);
  DarkFrameFilter unusedFilter;
  cv::Mat grey, bgr;
  std::string result;
  while (result.empty()) {
    if (clientGone()) return "gone";
    std::string s = turns.poll(secondsSince(start));
    if (s == "turn-left" || s == "turn-right" || s == "turn-back") {
      writeStatus(key, s, seq);
      sendLine("STATUS " + s);
      continue;
    }
    if (!s.empty()) { result = s; break; }
    bool usable = cam.grab(grey, 500, &bgr) && !unusable(cam, unusedFilter, grey);
    Face f;
    int count = 0;
    bool found = usable && models.detect(bgr, f, &count);
    result = turns.frame(secondsSince(start), usable, bgr, found, f, count);
  }
  if (result == "unsettled")
    fprintf(stderr, "omaface: head-turn check: not facing the camera (yaw %.2f, expected %.2f)\n", turns.lastYaw(),
            turns.expectedYaw());
  if (result == "ok") writeStatus(key, "scanning", seq);
  return result;
}

// ------------------------------------------------------------------ webcam checks

std::string choicePath(uid_t uid) { return std::string(kRunDir) + "/choice-" + std::to_string(uid); }

// sudo and system prompts approve something, so looking at the screen must
// never be enough on its own: the person first asks for omaFace (Enter in
// the terminal, a click or Enter in the dialog) or picks the password. The
// camera stays off while it waits. The terminal client answers on this
// connection; the dialog, which polkit's sandboxed helper cannot talk to,
// answers through CHOOSE, which leaves /run/omaface/choice-<uid>.
std::string awaitChoice(const std::string& key, uid_t uid, long seq, double timeout) {
  unlink(choicePath(uid).c_str());  // only a choice made for this prompt counts
  writeStatus(key, "choose", seq);
  sendLine("STATUS choose");
  auto start = Clock::now();
  while (secondsSince(start) < timeout) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, 100) > 0) {
      char c;
      if (recv(STDIN_FILENO, &c, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) return "gone";
      std::string line;
      if (readLine(STDIN_FILENO, line, 500)) {
        if (line == "CHOICE face") return "face";
        if (line == "CHOICE password") return "password";
      }
    }
    std::ifstream f(choicePath(uid));
    std::string choice;
    if (f >> choice) {
      unlink(choicePath(uid).c_str());
      if (choice == "face" || choice == "password") return choice;
    }
  }
  return "timeout";
}

// RESET, from the lock screen after the password unlocked it: the person is
// back, so earlier rejected scans no longer count.
int resetStrikes() {
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return finish("error no-peer");
  if (!hasActiveLocalSession(cred.uid)) return finish("denied no-local-session");
  writeFailures(cred.uid, Failures{});
  return finish("ok");
}

// CHOOSE face|password, from the polkit dialog running as the person.
int choose(const std::string& choice) {
  if (choice != "face" && choice != "password") return finish("error bad-request");
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return finish("error no-peer");
  if (!hasActiveLocalSession(cred.uid)) return finish("denied no-local-session");
  mkdir(kRunDir, 0755);
  std::string path = choicePath(cred.uid), tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << choice << "\n";
    if (!f) return finish("error write");
  }
  chmod(tmp.c_str(), 0600);
  if (rename(tmp.c_str(), path.c_str()) != 0) return finish("error write");
  return finish("ok");
}

int serve() {
  std::string line;
  if (!readLine(STDIN_FILENO, line, 3000)) return finish("error bad-request");
  std::istringstream ss(line);
  std::string verb;
  Request req;
  ss >> verb >> req.user >> req.service;
  if (verb == "CHOOSE") return choose(req.user);
  if (verb == "RESET") return resetStrikes();
  if (verb != "VERIFY" || req.user.empty() || req.service.empty()) return finish("error bad-request");

  // The peer may only ask about itself, unless it is root (sudo, polkit).
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return finish("error no-peer");
  passwd* pw = getpwnam(req.user.c_str());
  if (!pw) return finish("error unknown-user");
  uid_t uid = pw->pw_uid;
  if (cred.uid != 0 && cred.uid != uid) return finish("denied peer");

  Config cfg = loadConfig();
  std::string key;
  double timeout = 0;
  bool enabled = false, needsSession = false, needsSeat = false, test = false;
  if (req.service == "sudo") { key = "sudo"; enabled = cfg.sudo; timeout = cfg.timeoutSudo; needsSession = true; }
  else if (req.service == "polkit-1") { key = "polkit"; enabled = cfg.polkit; timeout = cfg.timeoutPolkit; needsSession = true; }
  else if (req.service == "sddm") { key = "login"; enabled = cfg.login; timeout = cfg.timeoutLogin; needsSeat = true; }
  else if (req.service == "omaface-lock") { key = "lock"; enabled = cfg.lock; timeout = cfg.timeoutLock; }
  else if (req.service == "omaface-test") { key = "test"; enabled = true; timeout = 8.0; test = true; }
  else return finish("disabled unknown-service");
  // Only the lock screen and the self-test may be asked for by the user itself;
  // sudo, polkit and login requests must come from their root PAM stacks.
  if ((key == "sudo" || key == "polkit" || key == "login") && cred.uid != 0) return finish("denied peer");
  if (!enabled) return finish("disabled");

  // Which camera decides the rules. The IR camera when there is one; when
  // there is none, the regular webcam (rgb_fallback), everywhere, with the
  // anti-spoofing check on every scan, sudo and system prompts always
  // waiting for the person to ask, and sticky strikes. Appearances only match the
  // kind of camera they were enrolled with.
  CameraInfo info;
  bool haveCamera = findCamera(cfg, info);
  const bool rgb = haveCamera && !info.ir;
  const double threshold = rgb ? cfg.thresholdRgb : cfg.threshold;
  const int lockoutAt = rgb ? kLockoutFailuresRgb : kLockoutFailures;

  auto gallery = loadGallery(req.user);
  gallery.erase(std::remove_if(gallery.begin(), gallery.end(),
                               [&](const Appearance& a) { return (a.kind == "rgb") != rgb; }),
                gallery.end());
  // The self-test still runs the camera and models with nothing enrolled, so
  // a fresh install can be checked end to end before the first enrollment.
  if (gallery.empty() && !test) return finish("unavailable not-enrolled");
  // The lock screen hides the omaFace mark while the camera cannot see you.
  if (lidClosed()) { writeStatus(key, "unavailable", 0); return finish("unavailable lid-closed"); }
  if (needsSession && !hasActiveLocalSession(uid)) return finish("unavailable no-local-session");
  if (needsSeat && !hasActiveLocalSession((uid_t)-1)) return finish("unavailable no-local-seat");

  Failures fails = readFailures(uid);
  if (!test && fails.count >= lockoutAt) return finish("locked-out");

  long seq = (long)time(nullptr) * 1000 + getpid() % 1000;
  unlink(choicePath(uid).c_str());  // a choice only counts for the prompt it was made in
  // A webcam always asks first for sudo and system prompts, whatever the
  // confirm setting: looking at the screen must never approve them.
  if ((key == "sudo" || key == "polkit") && (cfg.confirm || rgb)) {
    std::string choice = awaitChoice(key, uid, seq, cfg.timeoutChoose);
    if (choice != "face") {
      writeStatus(key, "idle", seq);
      return finish(choice == "password" ? "declined" : (choice == "gone" ? "error client-gone" : "timeout choose"));
    }
  }

  mkdir(kRunDir, 0755);
  int lockFd = ::open((std::string(kRunDir) + "/camera.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  bool haveLock = false;
  for (int i = 0; i < 20 && lockFd >= 0; i++) {
    if (flock(lockFd, LOCK_EX | LOCK_NB) == 0) { haveLock = true; break; }
    usleep(50000);
  }
  if (!haveLock) return finish("busy");

  if (!haveCamera) { writeStatus(key, "unavailable", 0); return finish("unavailable no-camera"); }

  FaceModels models;
  std::string err;
  if (!models.load(kModelDir, err)) { fprintf(stderr, "%s\n", err.c_str()); return finish("error models"); }

  Camera cam;
  if (!cam.open(info, err)) {
    fprintf(stderr, "%s\n", err.c_str());
    writeStatus(key, "unavailable", 0);
    return finish("unavailable camera");
  }
  const int minFace = minFaceFor(cfg, cam);
  // Fail closed: without its models a check would quietly pass everything.
  Attention attention;
  if (cfg.attention && !attention.load(kModelDir, err)) { fprintf(stderr, "%s\n", err.c_str()); return finish("error models"); }
  Liveness liveness;
  if (rgb && !liveness.load(kModelDir, err)) { fprintf(stderr, "%s\n", err.c_str()); return finish("error models"); }

  writeStatus(key, "scanning", seq);
  sendLine("STATUS scanning");

  auto start = Clock::now();
  // IR: a few matching frames in a row (three when attention is on, about
  // 0.4 s, so one eyelid reading near the line cannot open it). Webcam: a
  // run of eight, since a photo or a screen can pass the anti-spoofing
  // models on a lucky frame.
  VerifyRules rules;
  rules.webcam = rgb;
  rules.threshold = threshold;
  rules.needed = rgb ? 8 : cfg.attention ? std::max(cfg.matchFrames, 3) : cfg.matchFrames;
  rules.minFace = minFace;
  rules.attention = cfg.attention;
  rules.test = test;
  Verifier verifier(rules, gallery, models, &attention, rgb ? &liveness : nullptr);
  std::string lastStatus = "scanning";
  auto show = [&](const std::string& status) {
    if (status != lastStatus) {
      sendLine("STATUS " + status);
      lastStatus = status;
    }
  };
  cv::Mat grey, bgr;
  DarkFrameFilter darkFilter;
  while (secondsSince(start) < timeout) {
    if (clientGone()) { writeStatus(key, "idle", seq); return 1; }
    // The dialog was dismissed, or the person started typing the password:
    // the camera turns off now, not when the scan would have timed out.
    if (access(choicePath(uid).c_str(), F_OK) == 0) {
      std::ifstream f(choicePath(uid));
      std::string choice;
      f >> choice;
      unlink(choicePath(uid).c_str());
      if (choice == "password") {
        cam.close();
        writeStatus(key, "idle", seq);
        return finish("declined");
      }
    }
    if (!cam.grab(grey, 500, &bgr)) continue;
    if (unusable(cam, darkFilter, grey)) continue;
    std::string status = verifier.verify(bgr, secondsSince(start));
    const Observation& seen = verifier.last();
    if (seen.found) cam.followFace(cv::Rect(faceBox(seen.face)));
    else cam.searchFace();
    if (test && seen.scored) {
      char b[160];
      snprintf(b, sizeof b, "SCORE %.3f width %.0f attention %s live %.2f gaze %.0f %.0f eyes %.2f", seen.score,
               seen.face.width, seen.attentive.c_str(), seen.live, attention.lastGaze.x, attention.lastGaze.y,
               attention.lastEyes);
      sendLine(b);
    }
    if (status == "notenrolled") {
      show("face");
      cam.close();
      writeStatus(key, "idle", seq);
      return finish("unavailable not-enrolled");
    }
    if (status != "match") {
      show(status);
      continue;
    }
    if (rgb && cfg.headTurns) {
      std::string turns = headTurnChallenge(cam, models, gallery, threshold, minFace, key, seq, seen.face);
      if (turns == "gone") { writeStatus(key, "idle", seq); return 1; }
      if (turns == "unsettled" || turns == "crowd") {
        // Never faced the camera long enough to be asked, or someone
        // else came into view: no strike, just no unlock.
        cam.close();
        writeStatus(key, "timeout", seq);
        return finish("timeout " + turns);
      }
      if (turns != "ok") {
        cam.close();
        writeStatus(key, "failure", seq);
        if (!test) {
          fails.count++;
          fails.last = time(nullptr);
          fails.sticky = true;
          writeFailures(uid, fails);
        }
        return finish("fail liveness " + turns);
      }
    }
    cam.close();
    writeStatus(key, "success", seq);
    if (!test) writeFailures(uid, Failures{});
    return finish("ok");
  }
  cam.close();
  Verifier::Outcome out = verifier.outcome();
  if (out.strike) {
    writeStatus(key, "failure", seq);
    if (!test) {
      fails.count++;
      fails.last = time(nullptr);
      fails.sticky = fails.sticky || out.sticky;
      writeFailures(uid, fails);
    }
    return finish(out.result);
  }
  if (out.result == "attention") {
    writeStatus(key, "attention", seq);
    return finish("attention");
  }
  writeStatus(key, "timeout", seq);
  return finish(out.result);
}

// ------------------------------------------------------------------ enroll + test

// Mirrored braille rendering of the IR frame, cropped around the face: a live
// preview that fits a terminal or a QML Text and never leaves the machine.
std::vector<std::string> brailleFrame(const cv::Mat& grey, const cv::Rect& focus, int cols, int rows) {
  int dw = cols * 2, dh = rows * 4;
  cv::Rect roi = focus & cv::Rect(0, 0, grey.cols, grey.rows);
  if (roi.width < 8 || roi.height < 8) roi = cv::Rect(0, 0, grey.cols, grey.rows);
  cv::Mat small;
  cv::resize(grey(roi), small, cv::Size(dw, dh), 0, 0, cv::INTER_AREA);
  cv::flip(small, small, 1);
  cv::equalizeHist(small, small);
  static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  static const int bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
  std::vector<std::string> out;
  for (int r = 0; r < rows; r++) {
    std::string line;
    for (int c = 0; c < cols; c++) {
      int code = 0;
      for (int y = 0; y < 4; y++)
        for (int x = 0; x < 2; x++) {
          int px = c * 2 + x, py = r * 4 + y;
          int v = small.at<uint8_t>(py, px);
          if (v > (bayer[py % 4][px % 4] * 16 + 8)) code |= bit[y][x];
        }
      int cp = 0x2800 + code;
      line += char(0xE0 | (cp >> 12));
      line += char(0x80 | ((cp >> 6) & 0x3F));
      line += char(0x80 | (cp & 0x3F));
    }
    out.push_back(line);
  }
  return out;
}

// Appearance names are shown in the settings window and stored in YAML: keep
// any language, drop control characters and quotes, collapse spaces, and cap
// the length without cutting a UTF-8 character in half.
std::string cleanName(const std::string& in) {
  const size_t kMaxBytes = 40;
  std::string out;
  bool space = false;
  for (unsigned char c : in) {
    if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == ' ') { space = !out.empty(); continue; }
    if (space) { out += ' '; space = false; }
    out += (char)c;
  }
  if (out.size() > kMaxBytes) {
    size_t cut = kMaxBytes;
    while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
    out.resize(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

std::string nowIso() {
  char b[32];
  time_t t = time(nullptr);
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", localtime(&t));
  return b;
}

volatile sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }

// The settings window runs enrollment through pkexec, so the process is root
// and the window cannot signal it. Instead it closes our stdin when it stops
// or goes away; notice that before every frame so the camera turns off at once.
bool uiCallerGone() {
  struct stat st;
  if (fstat(STDIN_FILENO, &st) != 0 || !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) return false;
  pollfd p{STDIN_FILENO, POLLIN, 0};
  if (poll(&p, 1, 0) <= 0) return false;
  if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) return true;
  char sink[64];
  return read(STDIN_FILENO, sink, sizeof sink) == 0;
}

// A reader that stops reading would otherwise block us mid-write with the
// camera still streaming. Give it a second, then give up.
bool writeAllUi(const std::string& line) {
  size_t off = 0;
  while (off < line.size()) {
    pollfd p{STDOUT_FILENO, POLLOUT, 0};
    if (poll(&p, 1, 1000) <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
    ssize_t n = write(STDOUT_FILENO, line.data() + off, line.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    off += n;
  }
  return true;
}

int enroll(const std::string& user, const std::string& name, bool ui) {
#ifndef OMAFACE_DEV
  if (geteuid() != 0) { fprintf(stderr, "omaface-engine: enroll must run as root\n"); return 2; }
#endif
  passwd* pw = getpwnam(user.c_str());
  if (!pw) { fprintf(stderr, "omaface-engine: unknown user %s\n", user.c_str()); return 2; }

  // A closed stdout means the settings window went away: stop and save nothing.
  auto emit = [&](const std::string& json, const std::string& human) {
    if (ui) {
      if (!writeAllUi(json + "\n")) gStop = 1;
      return;
    }
    if (!human.empty()) printf("%s\n", human.c_str());
    if (fflush(stdout) != 0 || ferror(stdout)) gStop = 1;
  };
  auto fail = [&](const std::string& msg) {
    emit("{\"type\":\"error\",\"message\":\"" + jsonEscape(msg) + "\"}", "omaFace: " + msg);
    return 1;
  };

  auto gallery = loadGallery(user);
  if ((int)gallery.size() >= kMaxAppearances)
    return fail("already " + std::to_string(kMaxAppearances) + " appearances, remove one first");

  Config cfg = loadConfig();
  CameraInfo info;
  if (!findCamera(cfg, info)) return fail("no IR camera found");
  FaceModels models;
  std::string err;
  if (!models.load(kModelDir, err)) return fail(err);

  mkdir(kRunDir, 0755);
  int lockFd = ::open((std::string(kRunDir) + "/camera.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockFd < 0 || flock(lockFd, LOCK_EX) != 0) return fail("camera lock failed");

  Camera cam;
  if (!cam.open(info, err)) return fail(err);
  const int minFace = minFaceFor(cfg, cam);

  // The rules of enrolling are omaface-core's; the camera, the preview and
  // the gallery file are here. Without the eye models enrollment goes on
  // without a gaze baseline.
  Attention attention;
  const bool measureEyes = attention.load(kModelDir, err);
  Enrollment enrollment(sensors(models, measureEyes ? &attention : nullptr), cam.ir(), minFace);
  const int segments = Enrollment::kSegments;
  auto start = Clock::now();
  const double limit = 45.0;
  std::string hint;
  int lastFilled = -1;
  cv::Mat grey, bgr;
  DarkFrameFilter darkFilter;

  emit("{\"type\":\"start\",\"segments\":" + std::to_string(segments) + "}",
       "omaFace: look at the screen, then slowly move your head in a circle.");

  while (!gStop && secondsSince(start) < limit) {
    if (ui && uiCallerGone()) { gStop = 1; break; }
    if (!cam.grab(grey, 500, &bgr)) continue;
    if (unusable(cam, darkFilter, grey)) continue;
    Face face;
    int count = 0;
    bool got = models.detect(bgr, face, &count);
    cv::Rect focus(0, 0, grey.cols, grey.rows);
    if (got) {
      cam.followFace(cv::Rect(faceBox(face)));
      const float* p = face.row.ptr<float>();
      float cx = p[0] + p[2] / 2, cy = p[1] + p[3] / 2, s = std::max(p[2], p[3]) * 1.9f;
      focus = cv::Rect(int(cx - s / 2), int(cy - s * 0.42f * 1.25f), int(s), int(s * 0.62f * 1.25f));
    } else {
      cam.searchFace();
    }

    hint = enrollment.add(bgr, got, face, count, secondsSince(start));
    int nFilled = enrollment.filledCount();
    if (ui) {
      auto art = brailleFrame(grey, focus, 30, 12);
      std::string json = "{\"type\":\"frame\",\"face\":" + std::string(got ? "true" : "false") +
                         ",\"hint\":\"" + jsonEscape(hint) + "\",\"filled\":[";
      for (int i = 0; i < segments; i++) json += (i ? "," : "") + std::to_string(enrollment.filled()[i]);
      json += "],\"frontal\":" + std::to_string(enrollment.frontalCount()) + ",\"preview\":[";
      for (size_t i = 0; i < art.size(); i++) json += (i ? ",\"" : "\"") + art[i] + "\"";
      json += "]}";
      emit(json, "");
    } else if (nFilled != lastFilled) {
      printf("\r  %3d%%  %s            ", nFilled * 100 / segments, hint.c_str());
      fflush(stdout);
    }
    lastFilled = nFilled;

    if (enrollment.complete()) break;
  }
  cam.close();
  if (!ui) printf("\n");

  if (gStop) return fail("cancelled");
  Appearance a;
  std::string problem = enrollment.result(a);
  if (!problem.empty()) return fail(problem);
  a.name = cleanName(name);
  if (a.name.empty()) a.name = gallery.empty() ? "Face" : "Appearance " + std::to_string(gallery.size() + 1);
  a.created = nowIso();
  gallery.push_back(a);
  if (!saveGallery(user, gallery, err)) return fail(err);
  // A fresh enrollment clears any lockout from earlier failed scans.
  writeFailures(pw->pw_uid, Failures{});

  emit("{\"type\":\"done\",\"samples\":" + std::to_string(a.features.rows) + ",\"appearances\":" +
           std::to_string(gallery.size()) + ",\"eyes\":" + (a.hasGaze ? "true" : "false") + "}",
       "omaFace: enrolled \"" + a.name + "\" (" + std::to_string(a.features.rows) + " samples).");
  return 0;
}

#ifdef OMAFACE_DEV
// Development only: build a throwaway gallery from the first frontal frames,
// then print scores for the following ones. Nothing is written to disk.
int calib(double seconds) {
  Config cfg;
  CameraInfo info;
  if (!findCamera(cfg, info)) { printf("no camera\n"); return 1; }
  FaceModels models;
  std::string err;
  if (!models.load(kModelDir, err)) { printf("%s\n", err.c_str()); return 1; }
  Camera cam;
  auto t0 = Clock::now();
  if (!cam.open(info, err)) { printf("%s\n", err.c_str()); return 1; }
  printf("open %.3fs %s %s\n", secondsSince(t0), info.path.c_str(), info.card.c_str());
  Appearance a;
  cv::Mat grey, bgr;
  DarkFrameFilter darkFilter;
  int lit = 0, dark = 0, detected = 0;
  double firstFace = -1;
  std::vector<double> scores;
  while (secondsSince(t0) < seconds) {
    if (!cam.grab(grey, 500, &bgr)) continue;
    if (unusable(cam, darkFilter, grey)) { dark++; continue; }
    lit++;
    auto td = Clock::now();
    Face f;
    if (!models.detect(bgr, f)) continue;
    double tdet = secondsSince(td);
    detected++;
    if (firstFace < 0) firstFace = secondsSince(t0);
    auto tf = Clock::now();
    cv::Mat feat = models.feature(bgr, f);
    double tfe = secondsSince(tf);
    if (a.features.rows < 6) { a.features.push_back(feat); continue; }
    std::vector<Appearance> g{a};
    double sc = bestScore(g, feat);
    scores.push_back(sc);
    cv::Point2f pose = headPose(f);
    printf("t=%.2f w=%.0f det=%.3f score=%.3f mean=%.0f det_ms=%.0f feat_ms=%.0f yaw=%.2f pitch=%.2f\n",
           secondsSince(t0), f.width, f.score, sc, cv::mean(grey)[0], tdet * 1000, tfe * 1000, pose.x, pose.y);
  }
  std::sort(scores.begin(), scores.end());
  printf("lit=%d dark=%d detected=%d first_face=%.2fs\n", lit, dark, detected, firstFace);
  if (!scores.empty())
    printf("scores min=%.3f p10=%.3f median=%.3f\n", scores.front(), scores[scores.size() / 10], scores[scores.size() / 2]);
  return 0;
}
#endif

int listAppearances(const std::string& user) {
  auto gallery = loadGallery(user);
  printf("[");
  for (size_t i = 0; i < gallery.size(); i++)
    printf("%s{\"name\":\"%s\",\"created\":\"%s\",\"kind\":\"%s\",\"samples\":%d}", i ? "," : "",
           jsonEscape(gallery[i].name).c_str(), jsonEscape(gallery[i].created).c_str(), gallery[i].kind.c_str(),
           gallery[i].features.rows);
  printf("]\n");
  return 0;
}

int removeAppearance(const std::string& user, const std::string& which) {
  if (geteuid() != 0) { fprintf(stderr, "omaface-engine: remove must run as root\n"); return 2; }
  auto gallery = loadGallery(user);
  if (which == "all") {
    unlink(galleryPath(user).c_str());
    rmdir(userDir(user).c_str());
    printf("Removed all appearances.\n");
    return 0;
  }
  int index = atoi(which.c_str());
  if (index < 1 || index > (int)gallery.size()) {
    fprintf(stderr, "omaface: no appearance %s (have %zu)\n", which.c_str(), gallery.size());
    return 1;
  }
  std::string name = gallery[index - 1].name;
  gallery.erase(gallery.begin() + (index - 1));
  std::string err;
  if (gallery.empty()) {
    unlink(galleryPath(user).c_str());
    rmdir(userDir(user).c_str());
  } else if (!saveGallery(user, gallery, err)) {
    fprintf(stderr, "omaface: %s\n", err.c_str());
    return 1;
  }
  printf("Removed \"%s\".\n", name.c_str());
  return 0;
}

int renameAppearance(const std::string& user, const std::string& which, const std::string& rawName) {
  if (geteuid() != 0) { fprintf(stderr, "omaface-engine: rename must run as root\n"); return 2; }
  auto gallery = loadGallery(user);
  int index = atoi(which.c_str());
  if (index < 1 || index > (int)gallery.size()) {
    fprintf(stderr, "omaface: no appearance %s (have %zu)\n", which.c_str(), gallery.size());
    return 1;
  }
  std::string name = cleanName(rawName);
  if (name.empty()) { fprintf(stderr, "omaface: the name is empty\n"); return 1; }
  gallery[index - 1].name = name;
  std::string err;
  if (!saveGallery(user, gallery, err)) {
    fprintf(stderr, "omaface: %s\n", err.c_str());
    return 1;
  }
  printf("Renamed appearance %d to \"%s\".\n", index, name.c_str());
  return 0;
}

// Loads both models and runs them once on a synthetic frame: proves the
// binary, its OpenCV libraries and the model files still work together.
int selftest() {
  FaceModels models;
  std::string err;
  if (!models.load(kModelDir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  cv::Mat bgr(400, 640, CV_8UC3, cv::Scalar(40, 40, 40));
  cv::circle(bgr, cv::Point(320, 200), 90, cv::Scalar(160, 160, 160), cv::FILLED);
  Face face;
  models.detect(bgr, face);
  face.row = cv::Mat::zeros(1, 15, CV_32F);
  float* p = face.row.ptr<float>();
  float pts[15] = {230, 110, 180, 180, 280, 170, 360, 170, 320, 210, 290, 250, 350, 250, 0.9f};
  std::copy(pts, pts + 15, p);
  cv::Mat feat = models.feature(bgr, face);
  if (feat.cols != kFeatureSize) { fprintf(stderr, "unexpected feature size %d\n", feat.cols); return 1; }
  // The attention and anti-spoofing models too: a broken OpenCV must not
  // leave checks that quietly do nothing (a scan refuses to run without them).
  Attention attention;
  Liveness liveness;
  if (!attention.load(kModelDir, err) || !liveness.load(kModelDir, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  float live = liveness.live(bgr, face), open = attention.eyesOpen(bgr, face);
  cv::Point2f g = attention.gaze(bgr, face);
  if (!(live >= 0 && live <= 1) || !(open >= 0 && open < 10) || !(std::fabs(g.x) <= 180 && std::fabs(g.y) <= 180)) {
    fprintf(stderr, "models gave nonsense (live %f, eyes %f, gaze %f %f)\n", live, open, g.x, g.y);
    return 1;
  }
  printf("ok\n");
  return 0;
}

// probe webcam: also report a regular webcam when there is no IR camera (the
// installer asks before using one).
int probe(bool webcam) {
  Config cfg = loadConfig();
  if (webcam) cfg.rgbFallback = true;
  CameraInfo info;
  if (!findCamera(cfg, info)) { printf("none\n"); return 1; }
  printf("%s\t%s\t%ux%u\t%s\t%s\n", info.path.c_str(), info.card.c_str(), info.width, info.height,
         fourccName(info.fourcc).c_str(), info.ir ? "ir" : "webcam");
  return 0;
}

// Every capture device, what it offers, and which one omaFace would use:
// the first thing to look at on a laptop where the IR camera is not found.
int listCameras() {
  Config cfg = loadConfig();
  CameraInfo chosen;
  bool have = findCamera(cfg, chosen);
  int shown = 0;
  for (int i = 0; i < 64; i++) {
    std::string path = "/dev/video" + std::to_string(i);
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    v4l2_capability cap{};
    bool capture = false;
    std::string formats;
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
      uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
      capture = (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
      v4l2_fmtdesc fmt{};
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      for (fmt.index = 0; capture && xioctl(fd, VIDIOC_ENUM_FMT, &fmt); fmt.index++)
        formats += (formats.empty() ? "" : ",") + fourccName(fmt.pixelformat);
    }
    ::close(fd);
    if (!capture) continue;
    CameraInfo info;
    std::string verdict = have && chosen.path == path ? "used by omaFace"
                        : inspectDevice(path, info) ? "IR candidate"
                        : "colour camera, not used";
    printf("%s\t%s\t%s\t%s\n", path.c_str(), reinterpret_cast<const char*>(cap.card), formats.c_str(), verdict.c_str());
    shown++;
  }
  if (!shown) printf("no cameras\n");
  return have ? 0 : 1;
}

// Whether the IR light comes on. Most IR cameras pulse it on every other
// frame; some keep it on. Many laptops leave it off until it is switched on
// once (linux-enable-ir-emitter), and then every frame is near black.
int checkEmitter() {
  Config cfg = loadConfig();
  CameraInfo info;
  if (!findCamera(cfg, info)) { printf("{\"verdict\":\"no-camera\"}\n"); return 1; }
  Camera cam;
  std::string err;
  if (!cam.open(info, err)) {
    printf("{\"verdict\":\"%s\"}\n", err == "camera busy" ? "busy" : "error");
    return 1;
  }
  std::vector<double> means;
  cv::Mat grey, small;
  auto start = Clock::now();
  while (secondsSince(start) < 2.5 && means.size() < 40) {
    if (!cam.grab(grey, 500)) continue;
    cv::resize(grey, small, cv::Size(64, 40), 0, 0, cv::INTER_AREA);
    means.push_back(cv::mean(small)[0]);
  }
  cam.close();
  // The first frames are often the sensor settling; leave them out.
  if (means.size() > 8) means.erase(means.begin(), means.begin() + 4);
  if (means.size() < 6) { printf("{\"verdict\":\"no-frames\",\"frames\":%zu}\n", means.size()); return 1; }
  int alternating = 0;
  double hi = 0, lo = 0;
  for (size_t i = 0; i + 1 < means.size(); i++) {
    double a = std::max(means[i], means[i + 1]), b = std::min(means[i], means[i + 1]);
    if (a > b * 1.6 + 2) alternating++;
    hi += a;
    lo += b;
  }
  size_t pairs = means.size() - 1;
  hi /= pairs;
  lo /= pairs;
  double avg = std::accumulate(means.begin(), means.end(), 0.0) / means.size();
  const char* verdict = alternating * 10 >= (int)pairs * 6 ? "pulsing" : (avg >= 18 ? "steady" : "dark");
  printf("{\"verdict\":\"%s\",\"frames\":%zu,\"lit\":%.1f,\"unlit\":%.1f,\"mean\":%.1f}\n",
         verdict, means.size(), hi, lo, avg);
  return strcmp(verdict, "dark") == 0 ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  // OpenCL would load the NVIDIA ICD and wake the dGPU from D3cold for a job
  // the CPU finishes in milliseconds.
  setenv("OPENCV_OPENCL_DEVICE", "disabled", 1);
  setenv("OPENCV_OPENCL_RUNTIME", "disabled", 1);
  cv::ocl::setUseOpenCL(false);
  cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
  cv::setNumThreads(4);
  umask(022);

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);

  std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "serve") return serve();
  if (mode == "probe") return probe(argc > 2 && std::string(argv[2]) == "webcam");
  if (mode == "selftest") return selftest();
  if (mode == "list" && argc > 2) return listAppearances(argv[2]);
  if (mode == "cameras") return listCameras();
  if (mode == "emitter") return checkEmitter();
  if (mode == "remove" && argc > 3) return removeAppearance(argv[2], argv[3]);
  if (mode == "rename" && argc > 4) return renameAppearance(argv[2], argv[3], argv[4]);
#ifdef OMAFACE_DEV
  if (mode == "calib") return calib(argc > 2 ? atof(argv[2]) : 10);
#endif
  if (mode == "enroll" && argc > 2) {
    std::string name;
    bool ui = false;
    for (int i = 3; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--ui") ui = true;
      else if (a == "--name" && i + 1 < argc) name = argv[++i];
    }
    return enroll(argv[2], name, ui);
  }
  fprintf(stderr,
          "usage: omaface-engine serve | probe | cameras | emitter | selftest | list <user> | remove <user> <n|all> | rename <user> <n> <name> | enroll <user> [--name N] [--ui]\n"
          "self-test goes through the socket: omaface test\n");
  return 2;
}
