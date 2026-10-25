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

#include "calibration.h"
#include "config-settings.h"

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
const char* kDetectorModel = OMAFACE_MODEL_DIR "/face_detection_yunet_2023mar.onnx";
const char* kRecognizerModel = OMAFACE_MODEL_DIR "/face_recognition_sface_2021dec.onnx";
// Regular webcams only: two anti-spoofing models and an eye landmark model.
const char* kSpoofModelA = OMAFACE_MODEL_DIR "/MiniFASNetV2.onnx";
const char* kSpoofModelB = OMAFACE_MODEL_DIR "/MiniFASNetV1SE.onnx";
const char* kIrisModel = OMAFACE_MODEL_DIR "/iris_landmark.tflite";
const char* kGazeModel = OMAFACE_MODEL_DIR "/mobileone_s0_gaze.onnx";

const int kMaxAppearances = 3;
const int kLockoutFailures = 5;
const int kLockoutFailuresRgb = 3;  // a webcam is easier to fool; fewer tries
const int kLockoutSeconds = 300;
const int kFeatureSize = 128;

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

// ------------------------------------------------------------------ config

struct Config : omaface::Settings {
  std::string device = "auto";
  bool lock = true;
  bool login = false;
  bool sudo = true;
  bool polkit = true;
  bool attention = true;
  bool confirm = true;          // sudo and polkit wait for the person to choose omaFace
  bool rgbFallback = true;      // no IR camera: use the regular webcam
  bool headTurns = false;       // webcam: also ask for random head turns (strictest)
};

bool parseBool(const std::string& v, bool fallback) {
  if (v == "on" || v == "yes" || v == "true" || v == "1") return true;
  if (v == "off" || v == "no" || v == "false" || v == "0") return false;
  return fallback;
}

Config configFrom(const omaface::ConfigDocument& document) {
  Config c;
  static_cast<omaface::Settings&>(c) = omaface::numericSettings(document.text);
  std::istringstream in(document.text);
  std::string line, k, v;
  while (std::getline(in, line)) {
    if (!omaface::configLine(line, k, v)) continue;
    if (k == "device") c.device = v;
    else if (k == "lock") c.lock = parseBool(v, c.lock);
    else if (k == "login") c.login = parseBool(v, c.login);
    else if (k == "sudo") c.sudo = parseBool(v, c.sudo);
    else if (k == "polkit") c.polkit = parseBool(v, c.polkit);
    else if (k == "attention") c.attention = parseBool(v, true);
    else if (k == "confirm") c.confirm = parseBool(v, true);
    else if (k == "rgb_fallback") c.rgbFallback = parseBool(v, true);
    else if (k == "head_turns") c.headTurns = parseBool(v, false);
  }
  return c;
}

Config loadConfig() {
  omaface::ConfigDocument document;
  std::string error;
  if (!omaface::readConfigDocument(kConfigPath, document, error)) {
    fprintf(stderr, "omaface: %s; using defaults\n", error.c_str());
    return Config{};
  }
  return configFrom(document);
}

std::string configJson(const omaface::ConfigDocument& document) {
  Config cfg = configFrom(document);
  return std::string("{\"version\":1,\"values\":") +
         omaface::valuesJson(cfg) + ",\"defaults\":" + omaface::valuesJson(omaface::Settings{}) +
         ",\"bounds\":" + omaface::boundsJson() + ",\"revision\":\"" + document.revision +
         "\",\"attention\":" + (cfg.attention ? "true" : "false") +
         ",\"headTurns\":" + (cfg.headTurns ? "true" : "false") + '}';
}

int showConfig() {
  omaface::ConfigDocument document;
  std::string error;
  if (!omaface::readConfigDocument(kConfigPath, document, error)) {
    fprintf(stderr, "omaface-engine: %s\n", error.c_str());
    return 1;
  }
  printf("%s\n", configJson(document).c_str());
  return 0;
}

int settingsCommand(const std::vector<std::string>& args, const std::string* revision = nullptr) {
  std::vector<omaface::SettingChange> changes;
  std::string error;
  if (!omaface::assignments(args, false, changes, error)) {
    fprintf(stderr, "omaface-engine: %s\n", error.c_str());
    return 1;
  }
  if (!revision) return 0;
  omaface::ConfigDocument readback;
  if (!omaface::applySettings(kConfigPath, *revision, changes, readback, error)) {
    fprintf(stderr, "omaface-engine: %s\n", error.c_str());
    return 1;
  }
  printf("%s\n", configJson(readback).c_str());
  return 0;
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
    // Until a face is found, the middle of the frame, where faces sit.
    if (ir_) meter(cv::Rect(width_ / 4, height_ / 5, width_ / 2, height_ * 3 / 5));
    return true;
  }

  // Keep the exposure metered on the face as it moves; a region that still
  // mostly covers it is left alone.
  void followFace(const cv::Rect& face) {
    if (!ir_ || !meterable_) return;
    cv::Rect overlap = face & metered_;
    if (overlap.area() < 0.6 * face.area() || metered_.area() > 3 * face.area()) meter(face);
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

  bool meterable_ = true, meteringOn_ = false;
  cv::Rect metered_;

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
  return omaface::scaledMinFace(cfg.minFaceWidth, cam.width());
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

// ------------------------------------------------------------------ models

struct Face {
  cv::Mat row;          // 1x15 YuNet output
  float width = 0;
  float score = 0;
};

class FaceModels {
 public:
  bool load(std::string& err) {
    try {
      detector_ = cv::FaceDetectorYN::create(kDetectorModel, "", cv::Size(320, 320), 0.80f, 0.3f, 5);
      recognizer_ = cv::FaceRecognizerSF::create(kRecognizerModel, "");
    } catch (const cv::Exception& e) {
      err = std::string("model load failed: ") + e.what();
      return false;
    }
    if (!detector_ || !recognizer_) { err = "model load failed"; return false; }
    return true;
  }

  // Largest detected face, or none.
  bool detect(const cv::Mat& bgr, Face& out, int* count = nullptr) {
    if (bgr.size() != inputSize_) {
      inputSize_ = bgr.size();
      detector_->setInputSize(inputSize_);
    }
    cv::Mat faces;
    detector_->detect(bgr, faces);
    if (count) *count = faces.rows;
    if (faces.rows < 1) return false;
    if (faces.type() != CV_32F || faces.cols != 15 || !cv::checkRange(faces))
      throw cv::Exception(cv::Error::StsError, "invalid face model output", "detect", __FILE__, __LINE__);
    int best = 0;
    for (int i = 1; i < faces.rows; i++)
      if (faces.at<float>(i, 2) > faces.at<float>(best, 2)) best = i;
    out.row = faces.row(best).clone();
    out.width = faces.at<float>(best, 2);
    out.score = faces.at<float>(best, 14);
    return true;
  }

  cv::Mat feature(const cv::Mat& bgr, const Face& face) {
    cv::Mat aligned, feat;
    recognizer_->alignCrop(bgr, face.row, aligned);
    recognizer_->feature(aligned, feat);
    feat = feat.reshape(1, 1).clone();
    feat.convertTo(feat, CV_32F);
    double n = cv::norm(feat);
    if (feat.cols != kFeatureSize || !cv::checkRange(feat) || !std::isfinite(n) || n <= 0)
      throw cv::Exception(cv::Error::StsError, "invalid face feature", "feature", __FILE__, __LINE__);
    feat /= n;
    return feat;
  }

 private:
  cv::Ptr<cv::FaceDetectorYN> detector_;
  cv::Ptr<cv::FaceRecognizerSF> recognizer_;
  cv::Size inputSize_;
};

// ------------------------------------------------------------------ gallery

struct Appearance {
  std::string name;
  std::string created;
  cv::Mat features;  // N x 128, rows L2-normalised
  float headYaw = 0;  // head pose while looking straight at the screen
  float headPitch = 0;
  std::string kind = "ir";  // the camera it was enrolled with: "ir" or "rgb"
  float gazeYaw = 0;        // gaze while looking at the screen, degrees
  float gazePitch = 0;
  bool hasGaze = false;
};

std::string userDir(const std::string& user) { return std::string(kDataDir) + "/" + user; }
std::string galleryPath(const std::string& user) { return userDir(user) + "/faces.yml"; }

std::vector<Appearance> loadGallery(const std::string& user) {
  std::vector<Appearance> out;
  if (access(galleryPath(user).c_str(), R_OK) != 0) return out;
  try {
    cv::FileStorage fs(galleryPath(user), cv::FileStorage::READ);
    if (!fs.isOpened()) return out;
    cv::FileNode list = fs["appearances"];
    for (auto it = list.begin(); it != list.end(); ++it) {
      Appearance a;
      (*it)["name"] >> a.name;
      (*it)["created"] >> a.created;
      (*it)["features"] >> a.features;
      (*it)["head_yaw"] >> a.headYaw;
      (*it)["head_pitch"] >> a.headPitch;
      cv::FileNode gaze = (*it)["gaze"];
      if (gaze.isSeq() && gaze.size() == 2) {
        a.gazeYaw = (float)gaze[0];
        a.gazePitch = (float)gaze[1];
        a.hasGaze = true;
      }
      std::string kind;
      (*it)["kind"] >> kind;
      if (kind == "rgb") a.kind = kind;
      if (a.features.empty() || a.features.cols != kFeatureSize || a.features.type() != CV_32F ||
          !cv::checkRange(a.features) || !std::isfinite(a.headYaw) || !std::isfinite(a.headPitch) ||
          (a.hasGaze && (!std::isfinite(a.gazeYaw) || !std::isfinite(a.gazePitch)))) continue;
      out.push_back(a);
    }
  } catch (const cv::Exception&) {
    out.clear();
  }
  return out;
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
  try {
    cv::FileStorage fs(tmp, cv::FileStorage::WRITE);
    if (!fs.isOpened()) { err = "cannot write gallery"; return false; }
    fs << "version" << 1;
    fs << "appearances" << "[";
    for (auto& a : gallery)
      fs << "{" << "name" << a.name << "created" << a.created << "kind" << a.kind << "head_yaw" << a.headYaw << "head_pitch" << a.headPitch
         << "gaze" << "[" << a.gazeYaw << a.gazePitch << "]"
         << "features" << a.features << "}";
    fs << "]";
    fs.release();
  } catch (const cv::Exception& e) {
    err = e.what();
    return false;
  }
  if ((geteuid() == 0 && chown(tmp.c_str(), 0, pw->pw_gid) != 0) || chmod(tmp.c_str(), 0640) != 0 ||
      rename(tmp.c_str(), galleryPath(user).c_str()) != 0) {
    unlink(tmp.c_str());
    err = "cannot install gallery";
    return false;
  }
  return true;
}

double bestScore(const std::vector<Appearance>& gallery, const cv::Mat& feat, size_t* which = nullptr) {
  if (feat.type() != CV_32F || feat.cols != kFeatureSize || !cv::checkRange(feat)) return omaface::missing();
  double best = -1;
  for (size_t i = 0; i < gallery.size(); i++) {
    cv::Mat scores = gallery[i].features * feat.t();
    if (!cv::checkRange(scores)) return omaface::missing();
    double mx;
    cv::minMaxLoc(scores, nullptr, &mx);
    if (mx > best) {
      best = mx;
      if (which) *which = i;
    }
  }
  return best;
}

// ------------------------------------------------------------------ attention

cv::Point2f headPose(const Face& f);

// omaFace should not open for a face that is asleep or looking elsewhere. Two
// models judge every matching frame, on the IR camera and the webcam alike:
// MediaPipe's eye landmarks (are both eyelids open) and a Gaze360 gaze model
// (where the eyes point), with the head turned toward the screen.
//
// Gaze is relative to the gaze measured at enrollment. On a laptop webcam,
// looking at the screen stayed within about 16 degrees down (its bottom edge)
// and 10 up (the camera); a phone read 18-61 degrees down depending on how it
// was held, and a monitor above the laptop 20-32 up. Hence 20 down and 14 up:
// a phone held right under the screen's edge is, to any camera, the screen.
//
// Eyelids, as height over width of the eye contour: on the webcam 0.05-0.17
// closed and 0.28-0.34 on the screen, so 0.18. On the IR camera (grey,
// lit from beside the lens) 0.11-0.20 closed or looking down at a phone and
// 0.26-0.37 on the screen, so 0.22. Blinks dip lower and only restart the run.
//
// (Before, the IR camera looked for the pupil glowing in its light, which
// only shows looking almost straight into the lens and fades in daylight: it
// asked for a glance at the camera, not the screen.)
class Attention {
 public:
  static constexpr float kEyesOpenWebcam = 0.18f, kEyesOpenIr = 0.22f;
  static constexpr float kGazeDown = 20, kGazeUp = 14, kGazeSide = 25;

  bool load(std::string& err) {
    try {
      iris_ = cv::dnn::readNet(kIrisModel);
      gaze_ = cv::dnn::readNet(kGazeModel);
    } catch (const cv::Exception& e) {
      err = std::string("attention models: ") + e.what();
      return false;
    }
    if (iris_.empty() || gaze_.empty()) { err = "attention models missing"; return false; }
    return true;
  }

  static float eyesOpenAt(bool ir) { return ir ? kEyesOpenIr : kEyesOpenWebcam; }

  // "ok", "head" (turned away), "eyes" (closed) or "away" (eyes on
  // something other than the screen: a phone, a lap, another room).
  std::string check(const cv::Mat& bgr, const Face& f, const Appearance& a, bool ir) {
    lastEyes = std::numeric_limits<float>::quiet_NaN();
    lastGaze = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
    float facing = a.headPitch > 0 ? a.headYaw : 0.f;
    cv::Point2f pose = headPose(f);
    if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(facing)) return "unavailable";
    if (std::fabs(pose.x - facing) > 0.2f) return "head";
    lastEyes = eyesOpen(bgr, f);
    if (!std::isfinite(lastEyes)) return "unavailable";
    if (lastEyes < eyesOpenAt(ir)) return "eyes";
    cv::Point2f g = gaze(bgr, f);
    float dy = g.x - (a.hasGaze ? a.gazeYaw : 0.f), dp = g.y - (a.hasGaze ? a.gazePitch : 0.f);
    lastGaze = {dy, dp};
    if (!std::isfinite(dy) || !std::isfinite(dp)) return "unavailable";
    return (dp < -kGazeDown || dp > kGazeUp || std::fabs(dy) > kGazeSide) ? "away" : "ok";
  }

  // Gaze in degrees (yaw, pitch; pitch up is positive) from the face crop,
  // as the model was trained: RGB 448x448, ImageNet normalisation, 90 bins
  // of 4 degrees from -180, read as the expected value.
  cv::Point2f gaze(const cv::Mat& bgr, const Face& f) {
    const float* p = f.row.ptr<float>();
    cv::Rect box = cv::Rect(int(p[0]), int(p[1]), int(p[2]), int(p[3])) & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (box.width < 8 || box.height < 8) return {0, 0};
    cv::Mat rgb;
    cv::cvtColor(bgr(box), rgb, cv::COLOR_BGR2RGB);
    cv::resize(rgb, rgb, cv::Size(448, 448));
    rgb.convertTo(rgb, CV_32F, 1 / 255.0);
    cv::subtract(rgb, cv::Scalar(0.485, 0.456, 0.406), rgb);
    cv::divide(rgb, cv::Scalar(0.229, 0.224, 0.225), rgb);
    gaze_.setInput(cv::dnn::blobFromImage(rgb));
    std::vector<cv::Mat> outs;
    gaze_.forward(outs, std::vector<cv::String>{"yaw", "pitch"});
    if (outs.size() != 2) return {NAN, NAN};
    float angles[2];
    for (int k = 0; k < 2; k++) {
      cv::Mat logits = outs[k].reshape(1, 1), e;
      if (logits.type() != CV_32F || logits.cols != 90 || !cv::checkRange(logits)) return {NAN, NAN};
      double top;
      cv::minMaxLoc(logits, nullptr, &top);
      cv::exp(logits - top, e);
      double sum = cv::sum(e)[0], expected = 0;
      if (!std::isfinite(sum) || sum <= 0) return {NAN, NAN};
      for (int i = 0; i < e.cols; i++) expected += e.at<float>(0, i) * i;
      angles[k] = float(expected / sum * 4 - 180);
    }
    return {angles[0], angles[1]};
  }

  // The less open eye: eyelid height over width, from the eye contour.
  float eyesOpen(const cv::Mat& bgr, const Face& f) {
    const float* p = f.row.ptr<float>();
    cv::Point2f re(p[4], p[5]), le(p[6], p[7]), d = le - re;
    double iod = std::max(1.0f, std::hypot(d.x, d.y)), angle = std::atan2(d.y, d.x);
    float right = eyeOpen(bgr, re, angle, iod, false), left = eyeOpen(bgr, le, angle, iod, true);
    if (!std::isfinite(right) || !std::isfinite(left)) return NAN;
    return std::min(right, left);
  }

  // The last frame judged, gaze relative to enrollment (degrees), for the
  // self-test.
  cv::Point2f lastGaze{0, 0};
  float lastEyes = 0;

 private:
  // A square eye crop about one eye-spacing wide, eye line level, the left
  // eye mirrored (the model sees every eye the same way round), RGB 0-1.
  float eyeOpen(const cv::Mat& bgr, cv::Point2f eye, double angle, double iod, bool mirror) {
    double s = 64.0 / iod, al = s * std::cos(angle), be = s * std::sin(angle);
    cv::Matx23d M(al, be, (1 - al) * eye.x - be * eye.y + 32 - eye.x,
                  -be, al, be * eye.x + (1 - al) * eye.y + 32 - eye.y);
    cv::Mat patch;
    cv::warpAffine(bgr, patch, M, cv::Size(64, 64), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    if (mirror) cv::flip(patch, patch, 1);
    iris_.setInput(cv::dnn::blobFromImage(patch, 1 / 255.0, cv::Size(64, 64), cv::Scalar(), true));
    cv::Mat contour = iris_.forward("output_eyes_contours_and_brows").reshape(1, 71);
    if (contour.type() != CV_32F || contour.cols != 3 || !cv::checkRange(contour)) return NAN;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int k = 0; k < 16; k++) {  // the first 16 points outline the eyelids
      float x = contour.at<float>(k, 0), y = contour.at<float>(k, 1);
      minX = std::min(minX, x); maxX = std::max(maxX, x);
      minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
    return (maxY - minY) / std::max(1e-3f, maxX - minX);
  }

  cv::dnn::Net iris_, gaze_;
};

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

bool readLine(int fd, std::string& line, int timeoutMs) {
  auto start = Clock::now();
  line.clear();
  while (line.size() < 256) {
    int left = timeoutMs - int(secondsSince(start) * 1000);
    if (left <= 0) return false;
    pollfd p{fd, POLLIN, 0};
    int ready = poll(&p, 1, left);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) return false;
    char c;
    ssize_t r = recv(fd, &c, 1, MSG_DONTWAIT);
    if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
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

cv::Rect2f faceBox(const Face& f) {
  const float* p = f.row.ptr<float>();
  return {p[0], p[1], p[2], p[3]};
}

// A regular webcam cannot tell a face from a photo or a video of it, so the
// webcam mode asks the head to follow a random sequence: three moves, each
// to a random one of the two places it is not (from facing the camera: left
// or right; from a side: the other side or back), then back to facing the
// camera, where the face must still match. Each move is asked after a short
// random pause and must be finished within two seconds, but not so fast that
// it was anticipated.
//
// A real head turning moves the nose sideways against the eyes; a photo or a
// screen tilted the same way keeps the nose between the eyes (it is flat),
// so it never registers as a turn. A recording cannot follow a sequence it
// does not know: a clip turning one way fails at the first other move, and a
// blind guess at three moves is right one time in eight. Every frame must
// keep showing the same single face. Measured on a laptop webcam: facing yaw
// ~ -0.06, turned left ~ +0.25, turned right ~ -0.47.
//
// Returns "ok"; "unsettled" when the person never faced the camera long
// enough to be asked (an ordinary timeout); or a failure that counts toward
// the lockout: "wrong" (moved somewhere else), "slow" (asked, no answer),
// "early" (answered before it could have been read), "lost" (face gone or
// swapped), "identity" (the face back at the camera is not a match).
// "gone": the client left.
std::string headTurnChallenge(Camera& cam, FaceModels& models, const std::vector<Appearance>& gallery,
                              double threshold, int minFace, const std::string& key,
                              long seq, const Face& matched, bool test = false) {
  const float kTurn = 0.18f, kWrong = 0.25f, kCentre = 0.15f;
  const double kSettle = 5.0, kAnswer = 2.5, kTooFast = 0.15, kLost = 1.0;
  cv::Rect2f last = faceBox(matched);
  auto lastSeen = Clock::now();
  DarkFrameFilter unusedFilter;
  cv::Mat grey, bgr;

  // One frame: 1 = a face to judge, 0 = nothing usable yet, -1 = lost or
  // swapped, -2 = the client went away, -3 = more than one face in view.
  auto next = [&](Face& f) -> int {
    if (clientGone()) return -2;
    if (secondsSince(lastSeen) > kLost) return -1;
    if (!cam.grab(grey, 500, &bgr) || unusable(cam, unusedFilter, grey)) return 0;
    int count = 0;
    if (!models.detect(bgr, f, &count) || f.width < minFace * 0.8f) return 0;
    if (count > 1) return -3;
    cv::Rect2f b = faceBox(f);
    float jump = std::hypot(b.x + b.width / 2 - (last.x + last.width / 2), b.y + b.height / 2 - (last.y + last.height / 2));
    if (jump > 0.35f * last.width) return -1;
    last = b;
    lastSeen = Clock::now();
    return 1;
  };

  // Places: 0 = facing the camera, +1 = turned left, -1 = turned right
  // (relative to y0, the person's left is positive).
  float y0 = headPose(matched).x;
  auto at = [&](float d, int place) {
    return place == 0 ? std::fabs(d) <= kCentre : (place > 0 ? d >= kTurn : d <= -kTurn);
  };

  // Settle: facing the camera for a random pause before the first ask.
  // Looking away restarts the pause; never settling is no failure.
  {
    unsigned char rnd;
    if (getrandom(&rnd, 1, 0) != 1) return "lost";
    const double pause = 0.4 + rnd / 255.0 * 0.6;
    const float yMatched = y0;
    auto settleStart = Clock::now(), facingSince = Clock::now();
    float lastYaw = 0;
    int facing = 0;
    Face f;
    while (true) {
      if (secondsSince(settleStart) > kSettle) {
        fprintf(stderr, "omaface: head-turn check: not facing the camera (yaw %.2f, expected %.2f)\n", lastYaw, yMatched);
        return "unsettled";
      }
      int r = next(f);
      if (r == -2) return "gone";
      if (r == -3) return "crowd";
      if (r == -1) return "lost";
      if (r != 1) continue;
      lastYaw = headPose(f).x;
      if (std::fabs(lastYaw - yMatched) > kCentre) { facing = 0; continue; }
      if (facing == 0) facingSince = Clock::now();
      y0 = facing == 0 ? lastYaw : y0 * 0.8f + lastYaw * 0.2f;
      if (++facing >= 3 && secondsSince(facingSince) >= pause) break;
    }
  }

  unsigned char rnd[8];
  if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) return "lost";
  int place = 0;
  Face home;
  cv::Mat homeFrame;
  const int kMoves = 3;
  for (int step = 0; step <= kMoves; step++) {
    // The two places it is not, picked at random; the last step is home.
    int target;
    if (step == kMoves) target = 0;
    else if (place == 0) target = (rnd[step] & 1) ? 1 : -1;
    else target = (rnd[step] & 1) ? -place : 0;
    if (step == kMoves && place == 0) break;  // already home

    // A short random pause, holding the current place, before asking.
    const double pause = 0.15 + rnd[4 + step % 4] / 255.0 * 0.3;
    auto holdSince = Clock::now(), holdStart = Clock::now();
    Face f;
    while (secondsSince(holdSince) < pause) {
      if (secondsSince(holdStart) > kAnswer + pause) return "slow";
      int r = next(f);
      if (r == -2) return "gone";
      if (r == -3) return "crowd";
      if (r == -1) return "lost";
      if (r == 1 && !at(headPose(f).x - y0, place)) holdSince = Clock::now();
    }

    const char* ask = target == 0 ? "turn-back" : (target > 0 ? "turn-left" : "turn-right");
    if (!test) writeStatus(key, ask, seq);
    sendLine(std::string("STATUS ") + ask);
    auto asked = Clock::now();
    while (true) {
      if (secondsSince(asked) > kAnswer) return "slow";
      int r = next(f);
      if (r == -2) return "gone";
      if (r == -3) return "crowd";
      if (r == -1) return "lost";
      if (r != 1) continue;
      float d = headPose(f).x - y0;
      if (at(d, target)) {
        if (secondsSince(asked) < kTooFast) return "early";
        break;
      }
      // Somewhere it was not asked to go: from facing the camera, the other
      // side; on the way home, past it to the other side. (Crossing from one
      // side to the other starts on the far side, so there it only times out.)
      int away = place == 0 ? -target : (target == 0 ? -place : 0);
      if ((away > 0 && d >= kWrong) || (away < 0 && d <= -kWrong)) return "wrong";
    }
    place = target;
    if (place == 0) {
      home = f;
      bgr.copyTo(homeFrame);
    }
  }
  // The sequence always ends facing the camera; the face that arrived there
  // must still be the enrolled one.
  double score = bestScore(gallery, models.feature(homeFrame, home));
  if (!std::isfinite(score) || score < threshold) return "identity";
  if (!test) writeStatus(key, "scanning", seq);
  return "ok";
}

// ------------------------------------------------------------------ webcam checks

// A regular webcam sees what eyes see, so a photo or a phone screen showing
// the face looks like the face. Two anti-spoofing models (Silent-Face
// MiniFASNet V2 and V1SE, trained to tell live faces from prints and
// screens), averaged, run on the same frames as recognition, with nothing
// for the person to do. (An IR camera needs none: a print or a screen does
// not look like skin in its light.)
// Measured on a laptop webcam: live faces scored 0.99-1.00 on V1SE and
// 0.78-0.98 on V2; a photo and a video on a phone swung between 0 and 0.98
// from frame to frame, so a run of good frames is required.
class Liveness {
 public:
  bool load(std::string& err) {
    try {
      spoofA_ = cv::dnn::readNet(kSpoofModelA);
      spoofB_ = cv::dnn::readNet(kSpoofModelB);
    } catch (const cv::Exception& e) {
      err = std::string("webcam models: ") + e.what();
      return false;
    }
    if (spoofA_.empty() || spoofB_.empty()) { err = "webcam models missing"; return false; }
    return true;
  }

  // The mean and both model outputs, from one inference each on the usual crops.
  struct Scores {
    float mean = std::numeric_limits<float>::quiet_NaN();
    float v2 = std::numeric_limits<float>::quiet_NaN();
    float v1se = std::numeric_limits<float>::quiet_NaN();
  };

  Scores live(const cv::Mat& bgr, const Face& f) {
    Scores out;
    out.v2 = liveScore(spoofA_, crop(bgr, f, 2.7));
    out.v1se = liveScore(spoofB_, crop(bgr, f, 4.0));
    out.mean = 0.5f * (out.v2 + out.v1se);
    return out;
  }

 private:
  // The face box, enlarged as the models were trained (2.7x and 4x), 80x80.
  static cv::Mat crop(const cv::Mat& bgr, const Face& f, double scale) {
    const float* p = f.row.ptr<float>();
    double x = p[0], y = p[1], w = std::max(1.0f, p[2]), h = std::max(1.0f, p[3]);
    scale = std::min({(bgr.rows - 1) / h, (bgr.cols - 1) / w, scale});
    double cx = x + w / 2, cy = y + h / 2;
    int x1 = std::max(0, int(cx - w * scale / 2)), y1 = std::max(0, int(cy - h * scale / 2));
    int x2 = std::min(bgr.cols - 1, int(cx + w * scale / 2)), y2 = std::min(bgr.rows - 1, int(cy + h * scale / 2));
    cv::Mat out;
    cv::resize(bgr(cv::Rect(x1, y1, std::max(1, x2 - x1 + 1), std::max(1, y2 - y1 + 1))), out, cv::Size(80, 80));
    return out;
  }

  // BGR 0-255 in, three logits out; class 1 is "live".
  static float liveScore(cv::dnn::Net& net, const cv::Mat& face) {
    net.setInput(cv::dnn::blobFromImage(face, 1.0, cv::Size(80, 80), cv::Scalar(), false));
    cv::Mat logits = net.forward().reshape(1, 1), e;
    if (logits.type() != CV_32F || logits.cols != 3 || !cv::checkRange(logits))
      return std::numeric_limits<float>::quiet_NaN();
    double top;
    cv::minMaxLoc(logits, nullptr, &top);
    cv::exp(logits - top, e);
    double sum = cv::sum(e)[0];
    if (!std::isfinite(sum) || sum <= 0) return std::numeric_limits<float>::quiet_NaN();
    return float(e.at<float>(0, 1) / sum);
  }

  cv::dnn::Net spoofA_, spoofB_;
};

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

extern volatile sig_atomic_t gStop;
std::vector<std::string> brailleFrame(const cv::Mat& grey, const cv::Rect& focus, int cols, int rows);

int serve() {
  std::string line;
  bool complete = readLine(STDIN_FILENO, line, 3000);
  omaface::Request req = omaface::request(line);
  if (!complete || req.kind == omaface::RequestKind::Invalid) return finish("error bad-request");
  if (req.kind == omaface::RequestKind::Choose) return choose(req.user);
  if (req.kind == omaface::RequestKind::Reset) return resetStrikes();

  // The peer may only ask about itself, unless it is root (sudo, polkit).
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return finish("error no-peer");
  passwd* pw = getpwnam(req.user.c_str());
  if (!pw) return finish("error unknown-user");
  uid_t uid = pw->pw_uid;
  if (cred.uid != 0 && cred.uid != uid) return finish("denied peer");

  Config cfg = loadConfig();
  // TEST is the self-test with candidate settings for this scan only. They
  // never reach a real prompt: the request maps to omaface-test below.
  const bool candidate = req.kind == omaface::RequestKind::Test;
  if (candidate) omaface::changeSettings(cfg, req.changes);
  std::string key;
  double timeout = 0;
  bool enabled = false, needsSession = false, needsSeat = false, test = false;
  if (req.service == "sudo") { key = "sudo"; enabled = cfg.sudo; timeout = cfg.timeoutSudo; needsSession = true; }
  else if (req.service == "polkit-1") { key = "polkit"; enabled = cfg.polkit; timeout = cfg.timeoutPolkit; needsSession = true; }
  else if (req.service == "sddm") { key = "login"; enabled = cfg.login; timeout = cfg.timeoutLogin; needsSeat = true; }
  else if (req.service == "omaface-lock") { key = "lock"; enabled = cfg.lock; timeout = cfg.timeoutLock; }
  else if (req.service == "omaface-test") { key = "test"; enabled = true; timeout = cfg.timeoutTest; test = true; }
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
  auto status = [&](const std::string& state, long generation) {
    if (!test) writeStatus(key, state, generation);
  };
  // The lock screen hides the omaFace mark while the camera cannot see you.
  if (lidClosed()) { status("unavailable", 0); return finish("unavailable lid-closed"); }
  if (needsSession && !hasActiveLocalSession(uid)) return finish("unavailable no-local-session");
  if (needsSeat && !hasActiveLocalSession((uid_t)-1)) return finish("unavailable no-local-seat");

  Failures fails = test ? Failures{} : readFailures(uid);
  if (!test && fails.count >= lockoutAt) return finish("locked-out");

  long seq = (long)time(nullptr) * 1000 + getpid() % 1000;
  if (!test) unlink(choicePath(uid).c_str());  // only the current prompt's choice counts
  // A webcam always asks first for sudo and system prompts, whatever the
  // confirm setting: looking at the screen must never approve them.
  if ((key == "sudo" || key == "polkit") && (cfg.confirm || rgb)) {
    std::string choice = awaitChoice(key, uid, seq, cfg.timeoutChoose);
    if (choice != "face") {
      status("idle", seq);
      return finish(choice == "password" ? "declined" : (choice == "gone" ? "error client-gone" : "timeout choose"));
    }
  }

  mkdir(kRunDir, 0755);
  omaface::FileDescriptor cameraLock(::open((std::string(kRunDir) + "/camera.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  int lockFd = cameraLock.get();
  bool haveLock = false;
  for (int i = 0; i < 20 && lockFd >= 0; i++) {
    if (flock(lockFd, LOCK_EX | LOCK_NB) == 0) { haveLock = true; break; }
    usleep(50000);
  }
  if (!haveLock) return finish("busy");

  if (!haveCamera) { status("unavailable", 0); return finish("unavailable no-camera"); }

  FaceModels models;
  std::string err;
  if (!models.load(err)) { fprintf(stderr, "%s\n", err.c_str()); return finish("error models"); }

  Camera cam;
  if (!cam.open(info, err)) {
    fprintf(stderr, "%s\n", err.c_str());
    status("unavailable", 0);
    return finish("unavailable camera");
  }
  const int minFace = minFaceFor(cfg, cam);
  // Fail closed: without its models a check would quietly pass everything.
  Attention attention;
  if (cfg.attention && !attention.load(err)) { fprintf(stderr, "%s\n", err.c_str()); return finish("error models"); }
  Liveness liveness;
  if (rgb && !liveness.load(err)) { fprintf(stderr, "%s\n", err.c_str()); return finish("error models"); }

  status("scanning", seq);
  sendLine("STATUS scanning");

  auto start = Clock::now();
  int rejected = 0, inattentive = 0, notLive = 0;
  // IR keeps its attention floor of three; RGB defaults to eight lit frames.
  const auto requirements = omaface::requirements(cfg, rgb, cfg.attention, cam.width());
  const int needed = requirements.requiredFrames;
  // The self-test reports what this scan requires, as the engine applies it
  // (camera kind, attention floor, face width scaled to this camera).
  if (test) {
    char b[200];
    snprintf(b, sizeof b, "REQUIRE camera %s threshold %.3f live %.3f min_width %d frames %d timeout %.0f",
             rgb ? "webcam" : "ir", requirements.threshold, rgb ? requirements.liveThreshold : -1.0,
             requirements.minWidth, needed, timeout);
    sendLine(b);
  }
  omaface::Streak streak;
  bool sawFace = false;
  std::string lastStatus = "scanning";
  cv::Mat grey, bgr;
  DarkFrameFilter darkFilter;
  // A TEST also gets a small mirrored braille frame per frame, so the camera
  // is visible without exposing a video surface to the shell.
  auto sendPreview = [&](bool found, const Face& face) {
    if (!candidate) return;
    cv::Rect focus(0, 0, grey.cols, grey.rows);
    if (found && !face.row.empty()) {
      const float* p = face.row.ptr<float>();
      double s = std::max(p[2], p[3]) * 1.9;
      double cx = p[0] + p[2] / 2.0, cy = p[1] + p[3] / 2.0;
      focus = cv::Rect(int(cx - s / 2), int(cy - s * 0.52), int(s), int(s * 0.78));
    }
    std::string out = "PREVIEW";
    for (const auto& row : brailleFrame(grey, focus, 30, 12)) out += " " + row;
    sendLine(out);
  };
  while (secondsSince(start) < timeout) {
    if (gStop || clientGone()) { status("idle", seq); return 1; }
    // The dialog was dismissed, or the person started typing the password:
    // the camera turns off now, not when the scan would have timed out.
    if (!test && access(choicePath(uid).c_str(), F_OK) == 0) {
      std::ifstream f(choicePath(uid));
      std::string choice;
      f >> choice;
      unlink(choicePath(uid).c_str());
      if (choice == "password") {
        cam.close();
        status("idle", seq);
        return finish("declined");
      }
    }
    if (!cam.grab(grey, 500, &bgr)) continue;
    if (unusable(cam, darkFilter, grey)) continue;
    Face face;
    bool found = models.detect(bgr, face);
    if (found) cam.followFace(cv::Rect(faceBox(face)));
    if (!found || face.width < minFace) {
      omaface::Measurement measurement;
      measurement.found = found;
      measurement.width = found ? face.width : omaface::missing();
      auto gate = streak.judge(requirements, measurement);
      sendPreview(found, face);
      if (test) sendLine("SCORE -1 width " + std::string(found ? omaface::number(face.width) : "-1") +
                         " attention unavailable live -1 gaze 0 0 eyes 0 v2 -1 v1se -1 frames " +
                         std::to_string(gate.frames) + " required " + std::to_string(needed) + " gate " + gate.gate);
      if (lastStatus != "noface") { sendLine("STATUS noface"); lastStatus = "noface"; }
      continue;
    }
    sawFace = true;
    if (gallery.empty()) {
      if (lastStatus != "face") { sendLine("STATUS face"); lastStatus = "face"; }
      cam.close();
      status("idle", seq);
      return finish("unavailable not-enrolled");
    }
    size_t which = 0;
    double score = bestScore(gallery, models.feature(bgr, face), &which);
    // Attention and liveness only matter for a face that matched (and are
    // always shown by the self-test).
    omaface::Measurement measurement;
    measurement.found = true;
    measurement.width = face.width;
    measurement.score = score;
    measurement.attention = "off";
    Liveness::Scores live;
    if (score >= threshold || test) {
      if (cfg.attention) measurement.attention = attention.check(bgr, face, gallery[which], !rgb);
      if (rgb) live = liveness.live(bgr, face);
    }
    measurement.live = live.mean;
    measurement.liveV2 = live.v2;
    measurement.liveV1SE = live.v1se;
    auto gate = streak.judge(requirements, measurement);
    sendPreview(true, face);
    if (test) {
      char b[320];
      auto diagnostic = [](double v) { return std::isfinite(v) ? v : -1.0; };
      snprintf(b, sizeof b, "SCORE %.3f width %.0f attention %s live %.2f gaze %.0f %.0f eyes %.2f v2 %.3f v1se %.3f frames %d required %d gate %s",
               diagnostic(score), face.width, measurement.attention.c_str(), diagnostic(live.mean),
               diagnostic(attention.lastGaze.x), diagnostic(attention.lastGaze.y), diagnostic(attention.lastEyes),
               diagnostic(live.v2), diagnostic(live.v1se), gate.frames, needed, gate.gate.c_str());
      sendLine(b);
    }

    if (gate.gate == "models") {
      cam.close();
      status("unavailable", seq);
      return finish("error models");
    } else if (gate.gate == "nomatch") {
      rejected++;
      if (lastStatus != "nomatch") { sendLine("STATUS nomatch"); lastStatus = "nomatch"; }
    } else if (gate.gate == "attention") {
      // Your face, but asleep or looking elsewhere: no strike.
      inattentive++;
      if (lastStatus != "attention") { sendLine("STATUS attention"); lastStatus = "attention"; }
    } else if (gate.gate == "notlive") {
      notLive++;
      if (lastStatus != "notlive") { sendLine("STATUS notlive"); lastStatus = "notlive"; }
    } else if (gate.passed) {
      if (rgb && cfg.headTurns) {
        std::string turns = headTurnChallenge(cam, models, gallery, threshold, minFace, key, seq, face, test);
        if (turns == "gone") { status("idle", seq); return 1; }
        if (turns == "unsettled" || turns == "crowd") {
          // Never faced the camera long enough to be asked, or someone
          // else came into view: no strike, just no unlock.
          cam.close();
          status("timeout", seq);
          return finish("timeout " + turns);
        }
        if (turns != "ok") {
          cam.close();
          status("failure", seq);
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
      status("success", seq);
      if (!test) writeFailures(uid, Failures{});
      return finish("ok");
    } else if (lastStatus != "face") {
      sendLine("STATUS face");
      lastStatus = "face";
    }
  }
  cam.close();
  if (rgb && notLive >= needed && notLive > inattentive) {
    status("failure", seq);
    if (!test) {
      fails.count++;
      fails.last = time(nullptr);
      fails.sticky = true;
      writeFailures(uid, fails);
    }
    return finish("fail liveness spoof");
  }
  if (inattentive > 0) {
    status("attention", seq);
    return finish("attention");
  }
  // Only a face that was seen and rejected counts toward the lockout; nobody
  // in front of the camera is just a timeout.
  if (sawFace && rejected > 0) {
    status("failure", seq);
    if (!test) {
      fails.count++;
      fails.last = time(nullptr);
      fails.sticky = fails.sticky || rgb;
      writeFailures(uid, fails);
    }
    return finish("fail");
  }
  status("timeout", seq);
  if (gallery.empty()) return finish("unavailable not-enrolled no-face-seen");
  return finish("timeout");
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

// Head pose from the five YuNet landmarks: yaw from the nose against the eye
// midpoint, pitch from where the nose sits between the eyes and the mouth.
cv::Point2f headPose(const Face& f) {
  const float* p = f.row.ptr<float>();
  float rex = p[4], rey = p[5], lex = p[6], ley = p[7], nx = p[8], ny = p[9];
  float rmx = p[10], rmy = p[11], lmx = p[12], lmy = p[13];
  float eyeMidX = (rex + lex) / 2, eyeMidY = (rey + ley) / 2;
  float mouthMidY = (rmy + lmy) / 2;
  float eyeDist = std::max(1.0f, std::hypot(lex - rex, ley - rey));
  float yaw = (nx - eyeMidX) / eyeDist;
  float span = std::max(1.0f, mouthMidY - eyeMidY);
  float pitch = (ny - eyeMidY) / span;
  (void)rmx; (void)lmx;
  return {yaw, pitch};
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
  if (!models.load(err)) return fail(err);

  mkdir(kRunDir, 0755);
  int lockFd = ::open((std::string(kRunDir) + "/camera.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockFd < 0 || flock(lockFd, LOCK_EX) != 0) return fail("camera lock failed");

  Camera cam;
  if (!cam.open(info, err)) return fail(err);
  const int minFace = minFaceFor(cfg, cam);

  const int segments = 24;
  std::vector<int> filled(segments, 0);
  std::vector<cv::Mat> frontal, angled(segments);
  bool haveCenter = false;
  cv::Point2f center(0, 0);
  std::vector<cv::Point2f> centerSamples;
  // Where this person's eyes point looking at the screen: the baseline for
  // the attention check. The straight-ahead samples wait for open eyes;
  // some glasses hide them in glare, so after a few seconds enrollment goes
  // on without, and the scan measures from straight ahead instead.
  Attention attention;
  const bool measureEyes = attention.load(err);
  std::vector<float> gazeYaws, gazePitches;
  bool frontalSeen = false;
  auto frontalStart = Clock::now();
  auto eyesOpenNow = [&](const cv::Mat& colour, const Face& f) {
    if (!measureEyes) return true;
    if (!frontalSeen) { frontalSeen = true; frontalStart = Clock::now(); }
    bool open = attention.eyesOpen(colour, f) >= Attention::eyesOpenAt(cam.ir());
    if (open) {
      cv::Point2f g = attention.gaze(colour, f);
      gazeYaws.push_back(g.x);
      gazePitches.push_back(g.y);
    }
    return open || secondsSince(frontalStart) > 8;
  };
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
    }

    if (!got) hint = "Position your face in front of the camera";
    else if (count > 1) hint = "Only one face, please";
    else if (face.width < minFace + 15) hint = "Move a little closer";
    else if (face.score < 0.88f) hint = "Hold still";
    else {
      cv::Point2f pose = headPose(face);
      if (!haveCenter && !eyesOpenNow(bgr, face)) {
        hint = "Open your eyes and look at the screen";
      } else if (!haveCenter) {
        centerSamples.push_back(pose);
        hint = "Look straight at the screen";
        frontal.push_back(models.feature(bgr, face));
        if (centerSamples.size() >= 5) {
          for (auto& c : centerSamples) center += c;
          center *= 1.0f / centerSamples.size();
          haveCenter = true;
        }
      } else {
        cv::Point2f d = pose - center;
        // Pitch moves less than yaw for the same head turn; even them out.
        d.y *= 1.6f;
        float mag = std::hypot(d.x, d.y);
        if (mag < 0.07f) {
          hint = "Slowly move your head in a circle";
          if (frontal.size() < 8 && eyesOpenNow(bgr, face)) frontal.push_back(models.feature(bgr, face));
        } else if (mag > 0.75f) {
          hint = "Not so far";
        } else {
          hint = "Keep going";
          // 0 = up, clockwise as seen on screen (mirrored, like a selfie).
          double ang = std::atan2(-d.x, -d.y);
          int seg = int(std::floor((ang + CV_PI) / (2 * CV_PI) * segments)) % segments;
          seg = (seg + segments / 2) % segments;
          if (mag > 0.12f && !filled[seg]) {
            filled[seg] = 1;
            angled[seg] = models.feature(bgr, face);
          }
        }
      }
    }

    int nFilled = std::count(filled.begin(), filled.end(), 1);
    if (ui) {
      auto art = brailleFrame(grey, focus, 30, 12);
      std::string json = "{\"type\":\"frame\",\"face\":" + std::string(got ? "true" : "false") +
                         ",\"hint\":\"" + jsonEscape(hint) + "\",\"filled\":[";
      for (int i = 0; i < segments; i++) json += (i ? "," : "") + std::to_string(filled[i]);
      json += "],\"frontal\":" + std::to_string(frontal.size()) + ",\"preview\":[";
      for (size_t i = 0; i < art.size(); i++) json += (i ? ",\"" : "\"") + art[i] + "\"";
      json += "]}";
      emit(json, "");
    } else if (nFilled != lastFilled) {
      printf("\r  %3d%%  %s            ", nFilled * 100 / segments, hint.c_str());
      fflush(stdout);
    }
    lastFilled = nFilled;

    if (nFilled >= segments * 3 / 4 && frontal.size() >= 6) break;
  }
  cam.close();
  if (!ui) printf("\n");

  int nFilled = std::count(filled.begin(), filled.end(), 1);
  if (gStop) return fail("cancelled");
  if (frontal.size() < 4) return fail("could not see your face clearly, try again in better position");
  if (nFilled < segments / 2) return fail("not enough head movement captured, try again");

  Appearance a;
  a.name = cleanName(name);
  if (a.name.empty()) a.name = gallery.empty() ? "Face" : "Appearance " + std::to_string(gallery.size() + 1);
  a.created = nowIso();
  a.kind = cam.ir() ? "ir" : "rgb";
  a.headYaw = center.x;
  a.headPitch = center.y;
  if (gazeYaws.size() >= 3) {
    auto median = [](std::vector<float> v) {
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
      return v[v.size() / 2];
    };
    a.gazeYaw = median(gazeYaws);
    a.gazePitch = median(gazePitches);
    a.hasGaze = true;
  }
  for (auto& f : frontal) a.features.push_back(f);
  for (auto& f : angled) if (!f.empty()) a.features.push_back(f);
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
  if (!models.load(err)) { printf("%s\n", err.c_str()); return 1; }
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
  if (!models.load(err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
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
  if (!attention.load(err) || !liveness.load(err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  Liveness::Scores live = liveness.live(bgr, face);
  float open = attention.eyesOpen(bgr, face);
  cv::Point2f g = attention.gaze(bgr, face);
  if (!(live.mean >= 0 && live.mean <= 1) || !(live.v2 >= 0 && live.v2 <= 1) ||
      !(live.v1se >= 0 && live.v1se <= 1) || !(open >= 0 && open < 10) ||
      !(std::fabs(g.x) <= 180 && std::fabs(g.y) <= 180)) {
    fprintf(stderr, "models gave nonsense (live %f, eyes %f, gaze %f %f)\n", live.mean, open, g.x, g.y);
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
  if (mode == "config" && argc == 2) return showConfig();
  if (mode == "validate-settings") return settingsCommand({argv + 2, argv + argc});
  if (mode == "apply-settings") {
    if (argc < 3) { fprintf(stderr, "omaface-engine: missing config revision\n"); return 1; }
    std::string revision = argv[2];
    return settingsCommand({argv + 3, argv + argc}, &revision);
  }
  if (mode == "serve" && argc == 2) {
    // The models throw on output they cannot judge. Answer instead of
    // aborting, so PAM gets a result and no core dump is written.
    try {
      return serve();
    } catch (const cv::Exception& e) {
      fprintf(stderr, "omaface: %s\n", e.what());
      return finish("error models");
    }
  }
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
          "usage: omaface-engine config | validate-settings key=value... | apply-settings <revision|-> key=value...\n"
          "       omaface-engine serve | probe | cameras | emitter | selftest | list <user> | remove <user> <n|all> | rename <user> <n> <name> | enroll <user> [--name N] [--ui]\n"
          "self-test goes through the socket: omaface test\n");
  return 2;
}
