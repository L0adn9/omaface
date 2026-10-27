// Enrolling a new appearance.
#include "omaface/core.hpp"

#include <algorithm>
#include <cmath>

namespace omaface {

Sensors sensors(FaceModels& models, Attention* attention) {
  Sensors s;
  s.feature = [&models](const cv::Mat& bgr, const Face& f) { return models.feature(bgr, f); };
  if (attention) {
    s.eyesOpen = [attention](const cv::Mat& bgr, const Face& f) { return attention->eyesOpen(bgr, f); };
    s.gaze = [attention](const cv::Mat& bgr, const Face& f) { return attention->gaze(bgr, f); };
  }
  return s;
}

Enrollment::Enrollment(const Sensors& sensors, bool ir, int minFace)
    : sensors_(sensors), ir_(ir), minFace_(minFace), filled_(kSegments, 0), angled_(kSegments) {}

// Where this person's eyes point looking at the screen: the baseline for
// the attention check. The straight-ahead samples wait for open eyes;
// some glasses hide them in glare, so after a few seconds enrollment goes
// on without, and the scan measures from straight ahead instead.
bool Enrollment::eyesOpenNow(const cv::Mat& bgr, const Face& f, double t) {
  if (!sensors_.eyesOpen) return true;
  if (!frontalSeen_) {
    frontalSeen_ = true;
    frontalStart_ = t;
  }
  bool open = sensors_.eyesOpen(bgr, f) >= Attention::eyesOpenAt(ir_);
  if (open && sensors_.gaze) {
    cv::Point2f g = sensors_.gaze(bgr, f);
    gazeYaws_.push_back(g.x);
    gazePitches_.push_back(g.y);
  }
  return open || t - frontalStart_ > 8;
}

std::string Enrollment::add(const cv::Mat& bgr, bool found, const Face& face, int count, double t) {
  if (!found) return "Position your face in front of the camera";
  if (count > 1) return "Only one face, please";
  if (!faceInView(face, bgr.size())) return "Move into the middle of the view";
  if (face.width < minFace_ + 15) return "Move a little closer";
  if (face.score < 0.88f) return "Hold still";
  cv::Point2f pose = headPose(face);
  if (!haveCenter_ && !eyesOpenNow(bgr, face, t)) return "Open your eyes and look at the screen";
  if (!haveCenter_) {
    centerSamples_.push_back(pose);
    frontal_.push_back(sensors_.feature(bgr, face));
    if (centerSamples_.size() >= 5) {
      for (auto& c : centerSamples_) center_ += c;
      center_ *= 1.0f / centerSamples_.size();
      haveCenter_ = true;
    }
    return "Look straight at the screen";
  }
  cv::Point2f d = pose - center_;
  // Pitch moves less than yaw for the same head turn; even them out.
  d.y *= 1.6f;
  float mag = std::hypot(d.x, d.y);
  if (mag < 0.07f) {
    if (frontal_.size() < 8 && eyesOpenNow(bgr, face, t)) frontal_.push_back(sensors_.feature(bgr, face));
    return "Slowly move your head in a circle";
  }
  if (mag > 0.75f) return "Not so far";
  // 0 = up, clockwise as seen on screen (mirrored, like a selfie).
  double ang = std::atan2(-d.x, -d.y);
  int seg = int(std::floor((ang + CV_PI) / (2 * CV_PI) * kSegments)) % kSegments;
  seg = (seg + kSegments / 2) % kSegments;
  if (mag > 0.12f && !filled_[seg]) {
    filled_[seg] = 1;
    angled_[seg] = sensors_.feature(bgr, face);
  }
  return "Keep going";
}

int Enrollment::filledCount() const { return int(std::count(filled_.begin(), filled_.end(), 1)); }

bool Enrollment::complete() const { return filledCount() >= kSegments * 3 / 4 && frontal_.size() >= 6; }

std::string Enrollment::result(Appearance& a) const {
  if (frontal_.size() < 4) return "could not see your face clearly, try again in better position";
  if (filledCount() < kSegments / 2) return "not enough head movement captured, try again";
  a.kind = ir_ ? "ir" : "rgb";
  a.headYaw = center_.x;
  a.headPitch = center_.y;
  if (gazeYaws_.size() >= 3) {
    auto median = [](std::vector<float> v) {
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
      return v[v.size() / 2];
    };
    a.gazeYaw = median(gazeYaws_);
    a.gazePitch = median(gazePitches_);
    a.hasGaze = true;
  }
  a.features = cv::Mat();
  for (auto& f : frontal_) a.features.push_back(f);
  for (auto& f : angled_) if (!f.empty()) a.features.push_back(f);
  return "";
}

}  // namespace omaface
