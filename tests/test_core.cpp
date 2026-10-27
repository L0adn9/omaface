// Tests for the parts of omaface-core that need no models: face geometry,
// matching, and the verification rules. Cases come from measurements on
// real cameras (640x400 IR frames), kept as box and landmark numbers only.
#include "omaface/core.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

using namespace omaface;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    failures++;
    printf("FAIL %s\n", what);
  }
}

void expectEq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    failures++;
    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
  }
}

// A YuNet row: box, right eye, left eye, nose, right and left mouth corner.
Face face(float x, float y, float w, float h, float rex, float rey, float lex, float ley, float nx, float ny,
          float rmx, float rmy, float lmx, float lmy) {
  Face f;
  float row[15] = {x, y, w, h, rex, rey, lex, ley, nx, ny, rmx, rmy, lmx, lmy, 0.9f};
  f.row = cv::Mat(1, 15, CV_32F, row).clone();
  f.width = w;
  f.score = 0.9f;
  return f;
}

// A plain frontal face of width w with its box at (x, y).
Face frontal(float x, float y, float w = 130) {
  float h = w * 1.35f;
  return face(x, y, w, h, x + w * 0.3f, y + h * 0.35f, x + w * 0.7f, y + h * 0.35f, x + w * 0.5f, y + h * 0.52f,
              x + w * 0.33f, y + h * 0.72f, x + w * 0.67f, y + h * 0.72f);
}

const cv::Size kFrame(640, 400);

void geometry() {
  // The owner at the screen (IR, sitting normally).
  expect(faceInView(face(252, 188, 134, 185, 289, 252, 348, 255, 318, 284, 292, 330, 345, 332), kFrame),
         "centred face is in view");
  // Leaning in: the box runs 27 px past the bottom edge, the mouth is inside.
  expect(faceInView(face(120, 120, 230, 307, 180, 230, 290, 230, 235, 290, 190, 350, 280, 350), kFrame),
         "leaning in, chin past the bottom, is in view");
  // The owner's face peeking past a phone at the left edge, one eye outside.
  expect(!faceInView(face(-1, 135, 115, 265, -7, 231, 49, 225, 13, 277, -4, 320, 40, 318), kFrame),
         "face cut by the left edge is not in view");
  // Same, video run: box flush with the edge, every landmark inside.
  expect(!faceInView(face(1, 145, 187, 249, 48, 230, 130, 219, 99, 269, 60, 320, 120, 312), kFrame),
         "face flush with the left edge is not in view");
  expect(!faceInView(frontal(520, 150), kFrame), "face cut by the right edge is not in view");
  expect(!faceInView(frontal(250, 4), kFrame), "face cut by the top edge is not in view");
  expect(!faceInView(face(250, 250, 130, 175, 290, 300, 340, 300, 315, 330, 295, 395, 335, 395), kFrame),
         "mouth at the bottom edge is not in view");
  cv::Point2f p = headPose(frontal(250, 150));
  expect(std::fabs(p.x) < 1e-4, "frontal face has no yaw");
}

// Unit rows: each appearance sample pointing a known way.
cv::Mat unit(float a, float b) {
  cv::Mat v = cv::Mat::zeros(1, kFeatureSize, CV_32F);
  v.at<float>(0, 0) = a;
  v.at<float>(0, 1) = b;
  return v / cv::norm(v);
}

void matching() {
  Appearance a;
  for (int i = 0; i < 10; i++) a.features.push_back(unit(1, i < 5 ? 0.f : 10.f));
  cv::Mat probe = unit(1, 0);
  // Five samples equal the probe (1.0), five are nearly orthogonal: top-5 is 1.
  expect(std::fabs(bestScore({a}, probe) - 1.0) < 1e-5, "top-5 mean of five exact samples is 1");
  // One lucky sample out of ten is not enough: the mean of the best five.
  Appearance lucky;
  lucky.features.push_back(unit(1, 0));
  for (int i = 0; i < 9; i++) lucky.features.push_back(unit(0, 1));
  expect(std::fabs(bestScore({lucky}, probe) - 0.2) < 1e-5, "one lucky sample scores only a fifth");
  size_t which = 9;
  bestScore({lucky, a}, probe, &which);
  expect(which == 1, "best appearance is reported");
}

void galleryFile() {
  Appearance a;
  a.name = "Face";
  a.created = "2026-10-27T12:00:00";
  a.kind = "rgb";
  a.headYaw = -0.06f;
  a.headPitch = 0.55f;
  a.gazeYaw = 3.5f;
  a.gazePitch = -7.25f;
  a.hasGaze = true;
  for (int i = 0; i < 25; i++) a.features.push_back(unit(1, float(i)));
  Appearance b = a;
  b.name = "Appearance 2";
  b.kind = "ir";
  b.features = a.features.rowRange(0, 10).clone();
  Appearance broken;  // wrong feature size: skipped on reading
  broken.name = "broken";
  broken.features = cv::Mat::ones(5, 64, CV_32F);

  char path[] = "/tmp/omaface-core-test-XXXXXX";
  int fd = mkstemp(path);
  expect(fd >= 0, "temp file");
  close(fd);
  std::string err;
  expect(writeGallery(path, {a, b, broken}, err), "gallery written");
  std::vector<Appearance> back = readGallery(path);
  unlink(path);
  expect(back.size() == 2, "two good appearances read back, the broken one skipped");
  if (back.size() != 2) return;
  expect(back[0].name == "Face" && back[0].created == a.created && back[0].kind == "rgb", "names and kind survive");
  expect(back[1].name == "Appearance 2" && back[1].kind == "ir", "second appearance survives");
  expect(back[0].headYaw == a.headYaw && back[0].headPitch == a.headPitch, "head pose survives");
  expect(back[0].hasGaze && back[0].gazeYaw == a.gazeYaw && back[0].gazePitch == a.gazePitch, "gaze survives");
  expect(back[0].features.rows == 25 && cv::norm(back[0].features, a.features) == 0, "features survive exactly");
  expect(readGallery("/nonexistent/faces.yml").empty(), "a missing file reads as empty");
}

struct Run {
  std::vector<Appearance> gallery{Appearance{}};
  FaceModels models;  // judge() never uses them
  Verifier v;
  explicit Run(VerifyRules r, bool enrolled = true)
      : gallery(enrolled ? 1 : 0), v(r, gallery, models, nullptr, nullptr) {}
};

Observation seen(Face f, double score, std::string attentive = "ok", float live = -1, int faces = 1) {
  Observation o;
  o.found = true;
  o.faces = faces;
  o.face = f;
  o.frame = kFrame;
  o.scored = true;
  o.score = score;
  o.attentive = attentive;
  o.live = live;
  return o;
}

VerifyRules ir() {
  VerifyRules r;
  r.threshold = 0.55;
  r.needed = 3;
  return r;
}

void rules() {
  const Face me = frontal(250, 150);
  {
    Run run(ir());
    expectEq(run.v.judge(seen(me, 0.80), 0.0), "face", "first good frame");
    expectEq(run.v.judge(seen(me, 0.80), 0.1), "face", "second good frame");
    expectEq(run.v.judge(seen(me, 0.80), 0.2), "match", "third good frame matches");
  }
  {
    Run run(ir());
    run.v.judge(seen(me, 0.80), 0.0);
    run.v.judge(seen(me, 0.80), 0.1);
    expectEq(run.v.judge(seen(me, 0.80, "ok", -1, 2), 0.2), "nomatch", "second face stops the run");
    expectEq(run.v.judge(seen(me, 0.80), 0.5), "nomatch", "held for a second after a second face");
    expectEq(run.v.judge(seen(me, 0.80), 1.3), "face", "counting again after the hold");
    run.v.judge(seen(me, 0.80), 1.4);
    expectEq(run.v.judge(seen(me, 0.80), 1.5), "match", "match after the hold");
  }
  {
    // A stranger at the screen, then the owner's face somewhere else.
    Run run(ir());
    expectEq(run.v.judge(seen(frontal(80, 150), 0.30), 0.0), "nomatch", "stranger rejected");
    expectEq(run.v.judge(seen(frontal(420, 150), 0.80), 0.5), "nomatch", "owner elsewhere held after a stranger");
    expectEq(run.v.judge(seen(frontal(420, 150), 0.80), 1.1), "face", "owner counts after the hold");
  }
  {
    // The owner's own first frames score low while the exposure settles:
    // a low score in the same place holds nothing.
    Run run(ir());
    run.v.judge(seen(me, 0.30), 0.0);
    expectEq(run.v.judge(seen(me, 0.80), 0.1), "face", "same place after a low score is not held");
  }
  {
    Run run(ir());
    run.v.judge(seen(me, 0.80), 0.0);
    run.v.judge(seen(me, 0.80), 0.1);
    expectEq(run.v.judge(seen(frontal(450, 150), 0.80), 0.2), "face", "a jump restarts the run");
    expectEq(run.v.judge(seen(frontal(450, 150), 0.80), 0.3), "face", "second frame after the jump");
    expectEq(run.v.judge(seen(frontal(450, 150), 0.80), 0.4), "match", "third frame after the jump");
  }
  {
    Run run(ir());
    run.v.judge(seen(me, 0.80), 0.0);
    run.v.judge(seen(me, 0.80), 0.1);
    expectEq(run.v.judge(seen(me, 0.80, "away"), 0.2), "attention", "glance away");
    expectEq(run.v.judge(seen(me, 0.80), 0.3), "match", "a glance away does not restart the run");
  }
  {
    Run run(ir());
    run.v.judge(seen(me, 0.80), 0.0);
    run.v.judge(seen(me, 0.80), 0.1);
    expectEq(run.v.judge(seen(me, 0.80, "eyes"), 0.2), "attention", "eyes closed");
    expectEq(run.v.judge(seen(me, 0.80), 0.3), "face", "closed eyes restart the run");
    expectEq(run.v.outcome().result, "attention", "time out after inattention");
    expect(!run.v.outcome().strike, "inattention is no strike");
  }
  {
    Run run(ir());
    Observation peek = seen(face(-1, 135, 115, 265, -7, 231, 49, 225, 13, 277, -4, 320, 40, 318), 0.80);
    for (int i = 0; i < 5; i++) expectEq(run.v.judge(peek, i * 0.1), "noface", "face at the edge is never judged");
    expectEq(run.v.outcome().result, "timeout", "only edge faces: a plain timeout");
  }
  {
    Run run(ir());
    run.v.judge(seen(me, 0.20), 0.0);
    Verifier::Outcome o = run.v.outcome();
    expectEq(o.result, "fail", "a rejected face fails");
    expect(o.strike && !o.sticky, "IR failure is a strike, not sticky");
  }
  {
    Run run(ir());
    run.v.judge(seen(me, 0.80, "ok", -1, 2), 0.0);
    expectEq(run.v.outcome().result, "timeout crowd", "two faces only: timeout crowd");
  }
  {
    Run run(ir(), false);
    expectEq(run.v.judge(seen(me, 0.80), 0.0), "notenrolled", "face with nothing enrolled");
    Run none(ir(), false);
    expectEq(none.v.outcome().result, "unavailable not-enrolled no-face-seen", "nothing enrolled, no face");
  }
  {
    VerifyRules r = ir();
    r.webcam = true;
    r.threshold = 0.60;
    r.needed = 8;
    Run run(r);
    for (int i = 0; i < 7; i++) expectEq(run.v.judge(seen(me, 0.80, "ok", 0.95f), i * 0.1), "face", "live webcam frame");
    expectEq(run.v.judge(seen(me, 0.80, "ok", 0.95f), 0.7), "match", "eighth live webcam frame matches");
  }
  {
    VerifyRules r = ir();
    r.webcam = true;
    r.needed = 8;
    Run run(r);
    // A video of the owner: recognised (0.66), never live.
    for (int i = 0; i < 10; i++) expectEq(run.v.judge(seen(me, 0.66, "ok", 0.0f), i * 0.1), "notlive", "video is not live");
    Verifier::Outcome o = run.v.outcome();
    expectEq(o.result, "fail liveness spoof", "a video fails as a spoof");
    expect(o.strike && o.sticky, "a spoof is a sticky strike");
  }
  {
    VerifyRules r = ir();
    r.webcam = true;
    Run run(r);
    run.v.judge(seen(me, 0.20), 0.0);
    expect(run.v.outcome().sticky, "webcam failure is sticky");
  }
}

}  // namespace

int main() {
  geometry();
  matching();
  galleryFile();
  rules();
  if (failures) {
    printf("%d failed\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
