// Is the person awake and looking at the screen?
#include "omaface/core.hpp"

#include <opencv2/imgproc.hpp>

#include <cmath>

namespace omaface {

// omaFace should not open for a face that is asleep or looking elsewhere. Two
// models judge every matching frame, on the IR camera and the webcam alike:
// MediaPipe's eye landmarks (are both eyelids open) and a Gaze360 gaze model
// (where the eyes point), with the head turned toward the screen.
//
// Gaze is relative to the gaze measured at enrollment. On a laptop webcam,
// looking at the screen stayed within about 16 degrees down (its bottom edge)
// and 10 up (the camera); a phone read 18-61 degrees down depending on how it
// was held, and a monitor above the laptop 20-32 up. Hence 20 down and 18 up:
// a phone held right under the screen's edge is, to any camera, the screen.
// (Up was 14, but sitting lower than at enrollment read 15-18 on the screen
// with the IR camera.) Sideways the model jitters: reading the screen moved
// between 2 and 31 from one frame to the next, so 30 to the side, and a
// frame looking away only does not count; it does not restart the run.
//
// Eyelids, as height over width of the eye contour: on the webcam 0.05-0.17
// closed and 0.28-0.34 on the screen, so 0.18. On the IR camera (grey,
// lit from beside the lens) 0.11-0.20 closed or looking down at a phone and
// 0.26-0.37 on the screen, so 0.22. Blinks dip lower and only restart the run.
//
// (Before, the IR camera looked for the pupil glowing in its light, which
// only shows looking almost straight into the lens and fades in daylight: it
// asked for a glance at the camera, not the screen.)

bool Attention::load(const std::string& modelDir, std::string& err) {
  try {
    iris_ = cv::dnn::readNet(modelDir + "/" + kIrisFile);
    gaze_ = cv::dnn::readNet(modelDir + "/" + kGazeFile);
  } catch (const cv::Exception& e) {
    err = std::string("attention models: ") + e.what();
    return false;
  }
  if (iris_.empty() || gaze_.empty()) { err = "attention models missing"; return false; }
  return true;
}

std::string Attention::check(const cv::Mat& bgr, const Face& f, const Appearance& a, bool ir) {
  float facing = a.headPitch > 0 ? a.headYaw : 0.f;
  if (std::fabs(headPose(f).x - facing) > 0.2f) return "head";
  lastEyes = eyesOpen(bgr, f);
  if (lastEyes < eyesOpenAt(ir)) return "eyes";
  cv::Point2f g = gaze(bgr, f);
  float dy = g.x - (a.hasGaze ? a.gazeYaw : 0.f), dp = g.y - (a.hasGaze ? a.gazePitch : 0.f);
  lastGaze = {dy, dp};
  return (dp < -kGazeDown || dp > kGazeUp || std::fabs(dy) > kGazeSide) ? "away" : "ok";
}

// Gaze in degrees (yaw, pitch; pitch up is positive) from the face crop,
// as the model was trained: RGB 448x448, ImageNet normalisation, 90 bins
// of 4 degrees from -180, read as the expected value.
cv::Point2f Attention::gaze(const cv::Mat& bgr, const Face& f) {
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
  float angles[2];
  for (int k = 0; k < 2; k++) {
    cv::Mat logits = outs[k].reshape(1, 1), e;
    double top;
    cv::minMaxLoc(logits, nullptr, &top);
    cv::exp(logits - top, e);
    double sum = cv::sum(e)[0], expected = 0;
    for (int i = 0; i < e.cols; i++) expected += e.at<float>(0, i) * i;
    angles[k] = float(expected / sum * 4 - 180);
  }
  return {angles[0], angles[1]};
}

// The less open eye: eyelid height over width, from the eye contour.
float Attention::eyesOpen(const cv::Mat& bgr, const Face& f) {
  const float* p = f.row.ptr<float>();
  cv::Point2f re(p[4], p[5]), le(p[6], p[7]), d = le - re;
  double iod = std::max(1.0f, std::hypot(d.x, d.y)), angle = std::atan2(d.y, d.x);
  return std::min(eyeOpen(bgr, re, angle, iod, false), eyeOpen(bgr, le, angle, iod, true));
}

// A square eye crop about one eye-spacing wide, eye line level, the left
// eye mirrored (the model sees every eye the same way round), RGB 0-1.
float Attention::eyeOpen(const cv::Mat& bgr, cv::Point2f eye, double angle, double iod, bool mirror) {
  double s = 64.0 / iod, al = s * std::cos(angle), be = s * std::sin(angle);
  cv::Matx23d M(al, be, (1 - al) * eye.x - be * eye.y + 32 - eye.x,
                -be, al, be * eye.x + (1 - al) * eye.y + 32 - eye.y);
  cv::Mat patch;
  cv::warpAffine(bgr, patch, M, cv::Size(64, 64), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
  if (mirror) cv::flip(patch, patch, 1);
  iris_.setInput(cv::dnn::blobFromImage(patch, 1 / 255.0, cv::Size(64, 64), cv::Scalar(), true));
  cv::Mat contour = iris_.forward("output_eyes_contours_and_brows").reshape(1, 71);
  float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
  for (int k = 0; k < 16; k++) {  // the first 16 points outline the eyelids
    float x = contour.at<float>(k, 0), y = contour.at<float>(k, 1);
    minX = std::min(minX, x); maxX = std::max(maxX, x);
    minY = std::min(minY, y); maxY = std::max(maxY, y);
  }
  return (maxY - minY) / std::max(1e-3f, maxX - minX);
}

}  // namespace omaface
