// omaface-core — the face recognition engine shared by omaFace (Linux) and
// Nod (macOS): detection, recognition, attention, webcam liveness and the
// rules that decide whether a run of frames is the enrolled person.
//
// Nothing here touches a camera, a socket, PAM or a file outside the gallery
// path it is given. The platform feeds it frames and acts on its answers.
#pragma once

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/objdetect/face.hpp>

#include <functional>
#include <string>
#include <vector>

namespace omaface {

const int kFeatureSize = 128;

// Model files, by name inside the model directory the platform installs.
extern const char* const kDetectorFile;    // YuNet
extern const char* const kRecognizerFile;  // SFace
extern const char* const kSpoofFileA;      // MiniFASNetV2 (webcam only)
extern const char* const kSpoofFileB;      // MiniFASNetV1SE (webcam only)
extern const char* const kIrisFile;        // MediaPipe iris landmarks
extern const char* const kGazeFile;        // mobileone_s0 gaze

// ------------------------------------------------------------------ faces

struct Face {
  cv::Mat row;          // 1x15 YuNet output
  float width = 0;
  float score = 0;
};

cv::Rect2f faceBox(const Face& f);
bool faceInView(const Face& f, const cv::Size& frame);
cv::Point2f headPose(const Face& f);

class FaceModels {
 public:
  bool load(const std::string& modelDir, std::string& err);
  // Largest detected face, or none. count: how many faces are at least
  // countMin pixels wide (all of them by default).
  bool detect(const cv::Mat& bgr, Face& out, int* count = nullptr, float countMin = 0);
  cv::Mat feature(const cv::Mat& bgr, const Face& face);

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

// The gallery file format (OpenCV YAML). Reading skips malformed
// appearances; a file that cannot be parsed reads as empty. Writing goes to
// exactly the path given: permissions and atomic replace are the caller's.
std::vector<Appearance> readGallery(const std::string& path);
bool writeGallery(const std::string& path, const std::vector<Appearance>& gallery, std::string& err);

double bestScore(const std::vector<Appearance>& gallery, const cv::Mat& feat, size_t* which = nullptr);

// ------------------------------------------------------------------ attention

class Attention {
 public:
  static constexpr float kEyesOpenWebcam = 0.18f, kEyesOpenIr = 0.22f;
  static constexpr float kGazeDown = 20, kGazeUp = 18, kGazeSide = 30;

  bool load(const std::string& modelDir, std::string& err);
  static float eyesOpenAt(bool ir) { return ir ? kEyesOpenIr : kEyesOpenWebcam; }
  // "ok", "head" (turned away), "eyes" (closed) or "away" (eyes on
  // something other than the screen: a phone, a lap, another room).
  std::string check(const cv::Mat& bgr, const Face& f, const Appearance& a, bool ir);
  cv::Point2f gaze(const cv::Mat& bgr, const Face& f);
  float eyesOpen(const cv::Mat& bgr, const Face& f);

  // The last frame judged, gaze relative to enrollment (degrees), for the
  // self-test.
  cv::Point2f lastGaze{0, 0};
  float lastEyes = 0;

 private:
  float eyeOpen(const cv::Mat& bgr, cv::Point2f eye, double angle, double iod, bool mirror);
  cv::dnn::Net iris_, gaze_;
};

// ------------------------------------------------------------------ webcam liveness

class Liveness {
 public:
  static constexpr float kLive = 0.85f;

  bool load(const std::string& modelDir, std::string& err);
  // 0-1: how sure the anti-spoofing models are that this is a live face.
  float live(const cv::Mat& bgr, const Face& f);

 private:
  static cv::Mat crop(const cv::Mat& bgr, const Face& f, double scale);
  static float liveScore(cv::dnn::Net& net, const cv::Mat& face);
  cv::dnn::Net spoofA_, spoofB_;
};

// ------------------------------------------------------------------ verification

struct VerifyRules {
  bool webcam = false;   // a regular camera: liveness on every matching frame
  double threshold = 0.55;
  int needed = 3;        // matching frames in a row
  int minFace = 70;      // pixels
  bool attention = true;
  bool test = false;     // self-test: attention and liveness on every face
};

// What one usable frame showed. verify() fills it; tests can build one.
struct Observation {
  bool found = false;    // any face at all
  int faces = 0;         // faces at least minFace wide
  Face face;             // the largest
  cv::Size frame;
  bool scored = false;   // score and the rest below were measured
  double score = -1;
  size_t which = 0;      // the appearance it matched best
  std::string attentive = "off";
  float live = -1;       // webcam only
};

// One verification: frames in, a decision out. The platform owns the
// camera and the clock, sends the status lines and counts the strikes.
class Verifier {
 public:
  Verifier(const VerifyRules& rules, const std::vector<Appearance>& gallery, FaceModels& models,
           Attention* attention, Liveness* liveness);

  // One usable frame at time t (seconds). Returns what the person should
  // see: "noface", "nomatch", "attention", "notlive", "face"; "match" when
  // the run is complete; "notenrolled" for a face with nothing enrolled.
  std::string verify(const cv::Mat& bgr, double t);
  // The same decision on an observation measured elsewhere (tests).
  std::string judge(const Observation& o, double t);
  const Observation& last() const { return last_; }

  // After the time ran out: the result to report, and whether it counts
  // toward the lockout (sticky: only the password clears it).
  struct Outcome {
    std::string result;
    bool strike = false;
    bool sticky = false;
  };
  Outcome outcome() const;

 private:
  std::string detected(const Observation& o, double t);
  std::string scored(const Observation& o, double t);

  VerifyRules rules_;
  const std::vector<Appearance>& gallery_;
  FaceModels& models_;
  Attention* attention_;
  Liveness* liveness_;
  Observation last_;

  int matches_ = 0, rejected_ = 0, inattentive_ = 0, notLive_ = 0, crowded_ = 0;
  bool sawFace_ = false;
  double strangerAt_ = -1e9, crowdAt_ = -1e9;
  cv::Rect2f runBox_, strangerBox_;
};

// ------------------------------------------------------------------ enrollment

// How enrollment and the head-turn check measure a face. sensors() gives the
// real ones; tests give their own.
struct Sensors {
  std::function<cv::Mat(const cv::Mat& bgr, const Face& f)> feature;
  // Empty when the eye models are missing: eyes then count as open and no
  // gaze baseline is recorded.
  std::function<float(const cv::Mat& bgr, const Face& f)> eyesOpen;
  std::function<cv::Point2f(const cv::Mat& bgr, const Face& f)> gaze;
};
Sensors sensors(FaceModels& models, Attention* attention);

// One new appearance: a few straight-ahead samples, then the head moving in
// a circle to fill 24 segments of angles. The platform owns the camera and
// the screen; it passes every usable frame with what the detector found.
class Enrollment {
 public:
  static const int kSegments = 24;

  Enrollment(const Sensors& sensors, bool ir, int minFace);
  // Returns the hint to show for this frame.
  std::string add(const cv::Mat& bgr, bool found, const Face& face, int count, double t);
  bool complete() const;
  int filledCount() const;
  const std::vector<int>& filled() const { return filled_; }
  size_t frontalCount() const { return frontal_.size(); }
  // When complete or out of time: "" and the appearance (features, head
  // pose, gaze, kind; the caller names and dates it), or what went wrong.
  std::string result(Appearance& out) const;

 private:
  bool eyesOpenNow(const cv::Mat& bgr, const Face& f, double t);

  Sensors sensors_;
  bool ir_;
  int minFace_;
  std::vector<int> filled_;
  std::vector<cv::Mat> frontal_, angled_;
  bool haveCenter_ = false;
  cv::Point2f center_{0, 0};
  std::vector<cv::Point2f> centerSamples_;
  std::vector<float> gazeYaws_, gazePitches_;
  bool frontalSeen_ = false;
  double frontalStart_ = 0;
};

// ------------------------------------------------------------------ head turns

// The webcam's optional head-turn check (head_turns=on), one frame at a
// time. The platform supplies 9 random bytes, the clock and the frames:
//
//   HeadTurns h(matched, random, t, identity);
//   while (true) {
//     s = h.poll(now);            // before each frame
//     if (s is "turn-left", "turn-right" or "turn-back") { show it; continue; }
//     if (!s.empty()) break;      // the result
//     grab a frame ...
//     s = h.frame(now, ...);      // "" or the result
//   }
//
// Results: "ok"; "unsettled" (never faced the camera long enough to be
// asked) and "crowd" (a second face), which are no strike; "wrong", "slow",
// "early", "lost" and "identity", which are.
class HeadTurns {
 public:
  // identity: how well a face in a frame matches the enrolled person.
  HeadTurns(const Face& matched, const unsigned char random[9], double t,
            std::function<double(const cv::Mat& bgr, const Face& f)> identity, double threshold, int minFace);
  std::string poll(double t);
  // usable: false for a frame with nothing to judge (dark, or none at all).
  std::string frame(double t, bool usable, const cv::Mat& bgr, bool found, const Face& f, int count);
  // For the log after "unsettled": the yaw last seen and the one expected.
  float lastYaw() const { return lastYaw_; }
  float expectedYaw() const { return yMatched_; }

 private:
  enum class Phase { Settle, Hold, Ask, Done };
  int accept(double t, bool usable, bool found, const Face& f, int count);
  bool at(float d, int place) const;
  std::string nextStep(double t);
  std::string finishMove(double t, const cv::Mat& bgr, const Face& f);

  std::function<double(const cv::Mat&, const Face&)> identity_;
  double threshold_;
  int minFace_;
  unsigned char rnd_[8];
  Phase phase_ = Phase::Settle;
  cv::Rect2f last_;
  double lastSeen_;
  float y0_, yMatched_, lastYaw_ = 0;
  double settleStart_, facingSince_, pause_, holdPause_ = 0;
  int facing_ = 0;
  int step_ = 0, place_ = 0, target_ = 0;
  double holdSince_ = 0, holdStart_ = 0, asked_ = 0;
  Face home_;
  cv::Mat homeFrame_;
  std::string result_;
};

}  // namespace omaface
