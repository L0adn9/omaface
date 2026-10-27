// Webcam liveness: a live face or a photo or screen showing one?
#include "omaface/core.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace omaface {

// A regular webcam sees what eyes see, so a photo or a phone screen showing
// the face looks like the face. Two anti-spoofing models (Silent-Face
// MiniFASNet V2 and V1SE, trained to tell live faces from prints and
// screens), averaged, run on the same frames as recognition, with nothing
// for the person to do. (An IR camera needs none: a print or a screen does
// not look like skin in its light.)
// Measured on a laptop webcam: live faces scored 0.99-1.00 on V1SE and
// 0.78-0.98 on V2; a photo and a video on a phone swung between 0 and 0.98
// from frame to frame, so a run of good frames is required.

bool Liveness::load(const std::string& modelDir, std::string& err) {
  try {
    spoofA_ = cv::dnn::readNet(modelDir + "/" + kSpoofFileA);
    spoofB_ = cv::dnn::readNet(modelDir + "/" + kSpoofFileB);
  } catch (const cv::Exception& e) {
    err = std::string("webcam models: ") + e.what();
    return false;
  }
  if (spoofA_.empty() || spoofB_.empty()) { err = "webcam models missing"; return false; }
  return true;
}

float Liveness::live(const cv::Mat& bgr, const Face& f) {
  return 0.5f * (liveScore(spoofA_, crop(bgr, f, 2.7)) + liveScore(spoofB_, crop(bgr, f, 4.0)));
}

// The face box, enlarged as the models were trained (2.7x and 4x), 80x80.
cv::Mat Liveness::crop(const cv::Mat& bgr, const Face& f, double scale) {
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
float Liveness::liveScore(cv::dnn::Net& net, const cv::Mat& face) {
  net.setInput(cv::dnn::blobFromImage(face, 1.0, cv::Size(80, 80), cv::Scalar(), false));
  cv::Mat logits = net.forward().reshape(1, 1), e;
  double top;
  cv::minMaxLoc(logits, nullptr, &top);
  cv::exp(logits - top, e);
  return float(e.at<float>(0, 1) / cv::sum(e)[0]);
}

}  // namespace omaface
