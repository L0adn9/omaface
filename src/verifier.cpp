// The rules of one verification: which frames count, which start the run
// again, and what a scan that ran out of time amounts to.
#include "omaface/core.hpp"

#include <cmath>

namespace omaface {

namespace {

// One person at a time. The engine judges the largest face, so with two in
// view, any frame that misses the one in front (turned, half out of frame)
// would judge whoever stands behind. A second face big enough to match
// stops the run for a second; so does a face that was clearly not yours
// (strangers measured below 0.40, LFW 99.9%), for any face somewhere else
// in the frame: a stranger at the screen whose face flickers out of the
// detector, with the owner behind. Not for a face in the same place: the
// owner's own first frames, while the exposure settles, also score 0.25-0.40.
// The run must also stay on one face: a jump of more than a third of its
// width between frames starts it again.
const double kStranger = 0.40, kStrangerHold = 1.0;

bool moved(const cv::Rect2f& a, const cv::Rect2f& b) {
  return std::hypot(a.x + a.width / 2 - (b.x + b.width / 2), a.y + a.height / 2 - (b.y + b.height / 2)) > b.width / 3;
}

}  // namespace

Verifier::Verifier(const VerifyRules& rules, const std::vector<Appearance>& gallery, FaceModels& models,
                   Attention* attention, Liveness* liveness)
    : rules_(rules), gallery_(gallery), models_(models), attention_(attention), liveness_(liveness) {}

std::string Verifier::verify(const cv::Mat& bgr, double t) {
  Observation o;
  o.frame = bgr.size();
  o.found = models_.detect(bgr, o.face, &o.faces, rules_.minFace);
  last_ = o;
  std::string status = detected(o, t);
  if (!status.empty()) return status;
  o.score = bestScore(gallery_, models_.feature(bgr, o.face), &o.which);
  // Attention and liveness only matter for a face that matched (and are
  // always shown by the self-test).
  if (o.score >= rules_.threshold || rules_.test) {
    if (rules_.attention && attention_) o.attentive = attention_->check(bgr, o.face, gallery_[o.which], !rules_.webcam);
    if (rules_.webcam && liveness_) o.live = liveness_->live(bgr, o.face);
  }
  o.scored = true;
  last_ = o;
  return scored(o, t);
}

std::string Verifier::judge(const Observation& o, double t) {
  last_ = o;
  std::string status = detected(o, t);
  return status.empty() ? scored(o, t) : status;
}

// Before the face is compared: is there exactly one, whole, big enough?
// Empty: go on and score it.
std::string Verifier::detected(const Observation& o, double t) {
  if (!o.found || o.face.width < rules_.minFace || !faceInView(o.face, o.frame)) {
    matches_ = 0;
    return "noface";
  }
  sawFace_ = true;
  if (o.faces > 1) {
    // Not a strike: you with someone looking over your shoulder.
    matches_ = 0;
    crowded_++;
    crowdAt_ = t;
    return "nomatch";
  }
  cv::Rect2f box = faceBox(o.face);
  if (matches_ > 0 && moved(box, runBox_)) matches_ = 0;
  runBox_ = box;
  if (gallery_.empty()) return "notenrolled";
  return "";
}

std::string Verifier::scored(const Observation& o, double t) {
  cv::Rect2f box = faceBox(o.face);
  if (o.score < rules_.threshold) {
    matches_ = 0;
    rejected_++;
    if (o.score < kStranger) {
      strangerAt_ = t;
      strangerBox_ = box;
    }
    return "nomatch";
  }
  if (t - crowdAt_ < kStrangerHold || (t - strangerAt_ < kStrangerHold && moved(box, strangerBox_))) {
    matches_ = 0;
    return "nomatch";
  }
  if (o.attentive != "off" && o.attentive != "ok") {
    // Your face, but asleep or looking elsewhere: not a match, and not a
    // stranger either, so it never counts toward the lockout. Closed eyes
    // or a turned head start the run again; a glance away just does not
    // count (gaze jitters from frame to frame).
    if (o.attentive != "away") matches_ = 0;
    inattentive_++;
    return "attention";
  }
  if (rules_.webcam && o.live < Liveness::kLive) {
    matches_ = 0;
    notLive_++;
    return "notlive";
  }
  return ++matches_ >= rules_.needed ? "match" : "face";
}

Verifier::Outcome Verifier::outcome() const {
  if (rules_.webcam && notLive_ >= rules_.needed && notLive_ > inattentive_) return {"fail liveness spoof", true, true};
  if (inattentive_ > 0) return {"attention", false, false};
  // Only a face that was seen and rejected counts toward the lockout; nobody
  // in front of the camera is just a timeout.
  if (sawFace_ && rejected_ > 0) return {"fail", true, rules_.webcam};
  if (crowded_ > 0) return {"timeout crowd", false, false};
  if (gallery_.empty()) return {"unavailable not-enrolled no-face-seen", false, false};
  return {"timeout", false, false};
}

}  // namespace omaface
