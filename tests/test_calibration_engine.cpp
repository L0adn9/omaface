#include "../engine/calibration.h"
#include "../engine/config-settings.h"

#include <cassert>
#include <fstream>
#include <iostream>

namespace {

omaface::Measurement good() {
  omaface::Measurement m;
  m.found = true;
  m.width = 100;
  m.score = 0.75;
  m.attention = "ok";
  m.live = m.liveV2 = m.liveV1SE = 0.95;
  return m;
}

void parserTests() {
  omaface::Settings defaults;
  std::string error;
  std::vector<omaface::SettingChange> changes;
  for (const auto& spec : omaface::kSettings) {
    assert(spec.get(defaults) == spec.fallback);
    double out;
    for (double v : {spec.min, spec.max, spec.fallback}) {
      assert(omaface::settingNumber(spec, omaface::number(v), out));
      assert(out == v);
    }
    assert(!omaface::settingNumber(spec, omaface::number(spec.min - 0.001), out));
    assert(!omaface::settingNumber(spec, omaface::number(spec.max + 0.001), out));
    for (const std::string bad : {"nan", "NaN", "inf", "-inf", "1e9999", "1e-9999", "0.5junk", "0x1p-1", " 0.5", "0.5 ", "", "1=2"})
      assert(!omaface::settingNumber(spec, bad, out));
    if (spec.integer()) assert(!omaface::settingNumber(spec, omaface::number(spec.min + 0.5), out));
    auto loaded = omaface::numericSettings(std::string(spec.key) + '=' + omaface::number(spec.min) + '\n' + spec.key + "=nan\n");
    assert(spec.get(loaded) == spec.fallback);
    loaded = omaface::numericSettings(std::string(spec.key) + '=' + omaface::number(spec.max) + " # retained comment\n");
    assert(spec.get(loaded) == spec.max);
  }
  double out;
  for (const std::string value : {".5", "+0.5", "5e-1", "0.5", "2.0", "2e0"}) assert(omaface::finiteNumber(value, out));
  for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
      {}, {"threshold"}, {"=0.5"}, {"threshold=0.5=x"}, {"threshold=0.5", "threshold=0.6"},
      {"attention=off"}, {"device=/tmp/x"}, {"threshold=NaN"}, {"threshold=.5", "match_frames=0"}})
    assert(!omaface::assignments(bad, false, changes, error));
  assert(omaface::assignments({"threshold=.5", "match_frames=2.0"}, false, changes, error));
  // A TEST may override only the testable settings, and may override none.
  assert(omaface::assignments({"threshold=.5"}, true, changes, error));
  assert(omaface::assignments({}, true, changes, error) && changes.empty());
  assert(!omaface::assignments({"timeout_sudo=4"}, true, changes, error));
  assert(!omaface::assignments({"timeout_choose=30"}, true, changes, error));
  assert(omaface::validRevision("v1-01:file._"));
  for (const std::string value : {"", "x y", "x/y", "x\n", "\xc3\xa9"}) assert(!omaface::validRevision(value));

  using K = omaface::RequestKind;
  assert(omaface::request("VERIFY aman omaface-test").kind == K::Verify);
  assert(omaface::request("CHOOSE face").kind == K::Choose);
  assert(omaface::request("RESET").kind == K::Reset);
  auto test = omaface::request("TEST aman liveness_threshold=0.7 match_frames_rgb=4");
  assert(test.kind == K::Test && test.user == "aman" && test.service == "omaface-test");
  assert(test.changes.size() == 2 && test.changes[0].value == 0.7 && test.changes[1].value == 4);
  test = omaface::request("TEST aman");
  assert(test.kind == K::Test && test.changes.empty());
  // Candidate values never ride on VERIFY, so no real prompt can take them.
  for (const std::string value : {"VERIFY aman omaface-test threshold=.3", "VERIFY aman sudo threshold=.3", "VERIFY aman",
       "RESET anything", "CHOOSE face suffix", "TEST", "TEST aman threshold=NaN", "TEST aman threshold=0.1",
       "TEST aman timeout_sudo=4", "TEST aman threshold=.5 threshold=.6", "TEST aman sudo", "TEST aman threshold=.5\v",
       "CALIBRATE aman 1", "CALIBRATE aman 2"})
    assert(omaface::request(value).kind == K::Invalid);
  omaface::Settings applied;
  omaface::changeSettings(applied, omaface::request("TEST aman threshold_rgb=0.6 timeout_test=4").changes);
  assert(applied.thresholdRgb == 0.6 && applied.timeoutTest == 4 && applied.livenessThreshold == 0.85);
}

void gateTests() {
  omaface::Settings settings;
  auto ir = omaface::requirements(settings, false, true);
  auto rgb = omaface::requirements(settings, true, true);
  assert(ir.requiredFrames == 3 && ir.threshold == 0.50 && ir.minWidth == 70);
  assert(rgb.requiredFrames == 8 && rgb.threshold == 0.55 && rgb.liveThreshold == 0.85);
  assert(omaface::requirements(settings, false, false).requiredFrames == 2);
  assert(omaface::scaledMinFace(70, 320) == 35);
  assert(omaface::scaledMinFace(70, 0) == 70);
  assert(omaface::scaledMinFace(30, 320) == 30);
  omaface::Streak streak;
  auto m = good();
  assert(!streak.judge(ir, m).passed);
  assert(!streak.judge(ir, m).passed);
  assert(streak.judge(ir, m).passed);
  streak.reset();
  for (int i = 1; i <= 8; ++i) {
    auto result = streak.judge(rgb, m);
    assert(result.frames == i && result.passed == (i == 8));
  }
  streak.reset();
  streak.judge(ir, m);
  auto skip = omaface::Measurement{};
  skip.skipped = true;
  auto result = streak.judge(ir, skip);
  assert(result.frames == 1 && !result.passed);
  result = streak.judge(ir, omaface::Measurement{});
  assert(result.frames == 0 && result.gate == "noface");

  auto reject = [&](omaface::Measurement input, const std::string& gate, bool webcam = true) {
    streak.judge(webcam ? rgb : ir, m);
    auto result = streak.judge(webcam ? rgb : ir, input);
    assert(!result.passed && result.frames == 0 && result.gate == gate);
  };
  auto bad = m; bad.width = 69; reject(bad, "small");
  bad = m; bad.score = 0.54; bad.attention = "eyes"; bad.live = 0.1; reject(bad, "nomatch");
  bad = m; bad.attention = "eyes"; bad.live = 0.1; reject(bad, "attention");
  for (const std::string state : {"head", "away", "off"}) { bad = m; bad.attention = state; reject(bad, "attention"); }
  bad = m; bad.attention = "unavailable"; reject(bad, "models");
  bad = m; bad.live = 0.84; reject(bad, "notlive");
  bad = m; bad.enrolled = false; reject(bad, "not-enrolled");
  bad = m; bad.models = false; reject(bad, "models");
  for (double value : {omaface::missing(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    bad = m; bad.score = value; reject(bad, "models");
    bad = m; bad.width = value; reject(bad, "models");
    bad = m; bad.live = value; reject(bad, "models");
    bad = m; bad.liveV2 = value; reject(bad, "models");
    bad = m; bad.liveV1SE = value; reject(bad, "models");
  }
  for (double value : {-0.01, 1.01}) { bad = m; bad.liveV2 = value; reject(bad, "models"); }
  for (double value : {-1.01, 1.01}) { bad = m; bad.score = value; reject(bad, "models"); }
  bad = m; bad.score = 1.000001; streak.reset(); assert(streak.judge(ir, bad).gate == "pass");
  bad = m; bad.live = bad.liveV2 = bad.liveV1SE = omaface::missing();
  streak.reset(); assert(streak.judge(ir, bad).gate == "pass");
  bad = m; bad.attention = "off";
  streak.reset(); assert(streak.judge(omaface::requirements(settings, false, false), bad).gate == "pass");

  for (const auto& spec : omaface::kSettings) {
    if (!spec.testable) continue;
    for (double v : {spec.min, spec.max}) {
      auto edge = settings;
      spec.set(edge, v);
      auto required = omaface::requirements(edge, true, true);
      auto face = good();
      face.width = 300; face.score = 0.99; face.live = face.liveV2 = face.liveV1SE = 1;
      streak.reset();
      for (int i = 1; i <= required.requiredFrames; ++i) assert(streak.judge(required, face).passed == (i == required.requiredFrames));
    }
  }
}

void configTests(const std::string& path) {
  const std::string text = "# retain\nlock=off\n  threshold = 0.5  # identity\nthreshold=0.55# duplicate\ndevice=auto\nunknown=x";
  { std::ofstream f(path); f << text; }
  omaface::ConfigDocument original, readback;
  std::string error;
  assert(omaface::readConfigDocument(path, original, error));
  std::vector<omaface::SettingChange> changes;
  assert(omaface::assignments({"threshold=0.65", "timeout_test=10"}, false, changes, error));
  assert(omaface::applySettings(path, original.revision, changes, readback, error));
  assert(readback.text == "# retain\nlock=off\n  threshold = 0.65  # identity\nthreshold=0.65 # duplicate\ndevice=auto\nunknown=x\ntimeout_test=10\n");
  assert(readback.revision != original.revision);
  auto parsed = omaface::numericSettings(readback.text);
  assert(parsed.threshold == 0.65 && parsed.timeoutTest == 10);
  struct stat st{};
  assert(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
  assert(!omaface::applySettings(path, original.revision, changes, original, error));
  assert(error.find("stale") != std::string::npos);
  const auto saved = readback.text;
  const auto revision = readback.revision;
  changes.push_back({omaface::setting("min_face_width"), 1});
  assert(!omaface::applySettings(path, "-", changes, original, error));
  assert(omaface::readConfigDocument(path, original, error) && original.text == saved && original.revision == revision);
  changes.pop_back();
  assert(omaface::applySettings(path, "-", changes, readback, error));
  assert(readback.revision == revision);
  assert(!omaface::applySettings(path, "../bad", changes, readback, error));
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  parserTests();
  gateTests();
  configTests(argv[1]);
  std::cout << "calibration core tests passed\n";
}
