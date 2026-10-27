// The webcam's head-turn check.
#include "omaface/core.hpp"

#include <cmath>

namespace omaface {

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

namespace {
const float kTurn = 0.18f, kWrong = 0.25f, kCentre = 0.15f;
const double kSettle = 5.0, kAnswer = 2.5, kTooFast = 0.15, kLost = 1.0;
const int kMoves = 3;
}  // namespace

HeadTurns::HeadTurns(const Face& matched, const unsigned char random[9], double t,
                     std::function<double(const cv::Mat&, const Face&)> identity, double threshold, int minFace)
    : identity_(std::move(identity)), threshold_(threshold), minFace_(minFace) {
  for (int i = 0; i < 8; i++) rnd_[i] = random[1 + i];
  last_ = faceBox(matched);
  lastSeen_ = t;
  // Places: 0 = facing the camera, +1 = turned left, -1 = turned right
  // (relative to y0, the person's left is positive).
  y0_ = yMatched_ = headPose(matched).x;
  // Settle: facing the camera for a random pause before the first ask.
  // Looking away restarts the pause; never settling is no failure.
  pause_ = 0.4 + random[0] / 255.0 * 0.6;
  settleStart_ = facingSince_ = t;
}

bool HeadTurns::at(float d, int place) const {
  return place == 0 ? std::fabs(d) <= kCentre : (place > 0 ? d >= kTurn : d <= -kTurn);
}

// Before each frame: the time limits of the current phase, the next
// question, and whether the face has been gone too long.
std::string HeadTurns::poll(double t) {
  if (phase_ == Phase::Done) return result_;
  if (phase_ == Phase::Settle && t - settleStart_ > kSettle) {
    phase_ = Phase::Done;
    return result_ = "unsettled";
  }
  if (phase_ == Phase::Hold) {
    if (t - holdSince_ >= holdPause_) {
      phase_ = Phase::Ask;
      asked_ = t;
      return target_ == 0 ? "turn-back" : (target_ > 0 ? "turn-left" : "turn-right");
    }
    if (t - holdStart_ > kAnswer + holdPause_) {
      phase_ = Phase::Done;
      return result_ = "slow";
    }
  }
  if (phase_ == Phase::Ask && t - asked_ > kAnswer) {
    phase_ = Phase::Done;
    return result_ = "slow";
  }
  if (t - lastSeen_ > kLost) {
    phase_ = Phase::Done;
    return result_ = "lost";
  }
  return "";
}

// 1 = a face to judge, 0 = nothing usable, -1 = lost or swapped, -3 = more
// than one face in view.
int HeadTurns::accept(double t, bool usable, bool found, const Face& f, int count) {
  if (!usable || !found || f.width < minFace_ * 0.8f) return 0;
  if (count > 1) return -3;
  cv::Rect2f b = faceBox(f);
  float jump = std::hypot(b.x + b.width / 2 - (last_.x + last_.width / 2), b.y + b.height / 2 - (last_.y + last_.height / 2));
  if (jump > 0.35f * last_.width) return -1;
  last_ = b;
  lastSeen_ = t;
  return 1;
}

std::string HeadTurns::frame(double t, bool usable, const cv::Mat& bgr, bool found, const Face& f, int count) {
  if (phase_ == Phase::Done) return result_;
  int r = accept(t, usable, found, f, count);
  if (r == -3 || r == -1) {
    phase_ = Phase::Done;
    return result_ = r == -3 ? "crowd" : "lost";
  }
  if (r != 1) return "";
  float yaw = headPose(f).x;
  if (phase_ == Phase::Settle) {
    lastYaw_ = yaw;
    if (std::fabs(yaw - yMatched_) > kCentre) {
      facing_ = 0;
      return "";
    }
    if (facing_ == 0) facingSince_ = t;
    y0_ = facing_ == 0 ? yaw : y0_ * 0.8f + yaw * 0.2f;
    if (++facing_ >= 3 && t - facingSince_ >= pause_) return nextStep(t);
    return "";
  }
  if (phase_ == Phase::Hold) {
    if (!at(yaw - y0_, place_)) holdSince_ = t;
    return "";
  }
  // Asked.
  float d = yaw - y0_;
  if (at(d, target_)) {
    if (t - asked_ < kTooFast) {
      phase_ = Phase::Done;
      return result_ = "early";
    }
    return finishMove(t, bgr, f);
  }
  // Somewhere it was not asked to go: from facing the camera, the other
  // side; on the way home, past it to the other side. (Crossing from one
  // side to the other starts on the far side, so there it only times out.)
  int away = place_ == 0 ? -target_ : (target_ == 0 ? -place_ : 0);
  if ((away > 0 && d >= kWrong) || (away < 0 && d <= -kWrong)) {
    phase_ = Phase::Done;
    return result_ = "wrong";
  }
  return "";
}

// The next move: one of the two places it is not, picked at random; the
// last step is home. Then a short random pause, holding the current place,
// before asking.
std::string HeadTurns::nextStep(double t) {
  if (step_ == kMoves) target_ = 0;
  else if (place_ == 0) target_ = (rnd_[step_] & 1) ? 1 : -1;
  else target_ = (rnd_[step_] & 1) ? -place_ : 0;
  if (step_ == kMoves && place_ == 0) {
    // Already home. The sequence always ends facing the camera; the face
    // that arrived there must still be the enrolled one.
    phase_ = Phase::Done;
    return result_ = identity_(homeFrame_, home_) < threshold_ ? "identity" : "ok";
  }
  holdPause_ = 0.15 + rnd_[4 + step_ % 4] / 255.0 * 0.3;
  holdSince_ = holdStart_ = t;
  phase_ = Phase::Hold;
  return "";
}

std::string HeadTurns::finishMove(double t, const cv::Mat& bgr, const Face& f) {
  place_ = target_;
  if (place_ == 0) {
    home_ = f;
    bgr.copyTo(homeFrame_);
  }
  if (++step_ > kMoves) {
    phase_ = Phase::Done;
    return result_ = identity_(homeFrame_, home_) < threshold_ ? "identity" : "ok";
  }
  return nextStep(t);
}

}  // namespace omaface
