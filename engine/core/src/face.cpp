// Detection, recognition, and the geometry of one face.
#include "omaface/core.hpp"

#include <opencv2/imgproc.hpp>

#include <cmath>

namespace omaface {

const char* const kDetectorFile = "face_detection_yunet_2023mar.onnx";
const char* const kRecognizerFile = "face_recognition_sface_2021dec.onnx";
const char* const kSpoofFileA = "MiniFASNetV2.onnx";
const char* const kSpoofFileB = "MiniFASNetV1SE.onnx";
const char* const kIrisFile = "iris_landmark.tflite";
const char* const kGazeFile = "mobileone_s0_gaze.onnx";

cv::Rect2f faceBox(const Face& f) {
  const float* p = f.row.ptr<float>();
  return {p[0], p[1], p[2], p[3]};
}

// The face inside the picture, with a little room. A face cut by the edge
// is not judged: holding a phone in front of the IR camera, the owner's own
// face peeked past it at the side, half of it out of view, and that half
// matched while the phone took the blame (the IR camera saw no face on the
// phone at all, even at 0.3 detector confidence). Only the chin may leave
// the picture, at the bottom, as long as the mouth is in it: the camera sits
// above the screen, and leaning in pushed the owner's chin up to 39 pixels
// past the bottom edge of a 400-pixel frame.
bool faceInView(const Face& f, const cv::Size& frame) {
  const float kMargin = 8;
  const float* p = f.row.ptr<float>();
  cv::Rect2f b = faceBox(f);
  float mouth = std::max(p[11], p[13]);
  return b.x >= kMargin && b.y >= kMargin && b.x + b.width <= frame.width - kMargin &&
         mouth <= frame.height - kMargin;
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

bool FaceModels::load(const std::string& modelDir, std::string& err) {
  try {
    detector_ = cv::FaceDetectorYN::create(modelDir + "/" + kDetectorFile, "", cv::Size(320, 320), 0.80f, 0.3f, 5);
    recognizer_ = cv::FaceRecognizerSF::create(modelDir + "/" + kRecognizerFile, "");
  } catch (const cv::Exception& e) {
    err = std::string("model load failed: ") + e.what();
    return false;
  }
  if (!detector_ || !recognizer_) { err = "model load failed"; return false; }
  return true;
}

bool FaceModels::detect(const cv::Mat& bgr, Face& out, int* count, float countMin) {
  if (bgr.size() != inputSize_) {
    inputSize_ = bgr.size();
    detector_->setInputSize(inputSize_);
  }
  cv::Mat faces;
  detector_->detect(bgr, faces);
  if (count) {
    *count = 0;
    for (int i = 0; i < faces.rows; i++)
      if (faces.at<float>(i, 2) >= countMin) ++*count;
  }
  if (faces.rows < 1) return false;
  int best = 0;
  for (int i = 1; i < faces.rows; i++)
    if (faces.at<float>(i, 2) > faces.at<float>(best, 2)) best = i;
  out.row = faces.row(best).clone();
  out.width = faces.at<float>(best, 2);
  out.score = faces.at<float>(best, 14);
  return true;
}

cv::Mat FaceModels::feature(const cv::Mat& bgr, const Face& face) {
  cv::Mat aligned, feat;
  recognizer_->alignCrop(bgr, face.row, aligned);
  recognizer_->feature(aligned, feat);
  feat = feat.reshape(1, 1).clone();
  feat.convertTo(feat, CV_32F);
  double n = cv::norm(feat);
  if (n > 0) feat /= n;
  return feat;
}

}  // namespace omaface
