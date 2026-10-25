#ifndef OMAFACE_CALIBRATION_H
#define OMAFACE_CALIBRATION_H

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace omaface {

struct Settings {
  double threshold, thresholdRgb, livenessThreshold;
  int matchFrames, matchFramesRgb, minFaceWidth;
  double timeoutTest, timeoutSudo, timeoutPolkit, timeoutLock, timeoutLogin, timeoutChoose;
  Settings();
};

struct SettingSpec {
  const char* key;
  double fallback, min, max, step;
  double Settings::*real;
  int Settings::*whole;
  bool testable;  // may be overridden for one TEST request
  bool integer() const { return whole != nullptr; }
  double get(const Settings& s) const { return real ? s.*real : s.*whole; }
  void set(Settings& s, double v) const {
    if (real) s.*real = v;
    else s.*whole = static_cast<int>(v);
  }
};

inline const std::array<SettingSpec, 12> kSettings{{
  {"threshold",          0.50, 0.30, 0.95, 0.01, &Settings::threshold,          nullptr, true},
  {"threshold_rgb",      0.55, 0.30, 0.95, 0.01, &Settings::thresholdRgb,       nullptr, true},
  {"match_frames",          2,    1,    5,    1, nullptr, &Settings::matchFrames, true},
  {"match_frames_rgb",      8,    3,   30,    1, nullptr, &Settings::matchFramesRgb, true},
  {"liveness_threshold", 0.85, 0.50, 0.99, 0.01, &Settings::livenessThreshold, nullptr, true},
  {"min_face_width",       70,   30,  300,    1, nullptr, &Settings::minFaceWidth, true},
  {"timeout_test",          8,    1,   60,    1, &Settings::timeoutTest,        nullptr, true},
  {"timeout_sudo",          4,    1,   15,    1, &Settings::timeoutSudo,        nullptr, false},
  {"timeout_polkit",        5,    1,   15,    1, &Settings::timeoutPolkit,      nullptr, false},
  {"timeout_lock",          6,    1,   15,    1, &Settings::timeoutLock,        nullptr, false},
  {"timeout_login",         6,    1,   15,    1, &Settings::timeoutLogin,       nullptr, false},
  {"timeout_choose",       30,    5,  120,    1, &Settings::timeoutChoose,      nullptr, false},
}};

inline Settings::Settings() {
  for (const auto& spec : kSettings) spec.set(*this, spec.fallback);
}

inline const SettingSpec* setting(const std::string& key) {
  for (const auto& spec : kSettings) if (key == spec.key) return &spec;
  return nullptr;
}

inline bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

inline std::string trim(const std::string& s) {
  size_t first = 0, last = s.size();
  while (first < last && space(s[first])) ++first;
  while (last > first && space(s[last - 1])) --last;
  return s.substr(first, last - first);
}

inline bool tokens(const std::string& line, std::vector<std::string>& out) {
  out.clear();
  size_t first = 0;
  while (first < line.size()) {
    while (first < line.size() && space(line[first])) ++first;
    size_t last = first;
    while (last < line.size() && !space(line[last])) {
      unsigned char ch = line[last];
      if (ch < 0x21 || ch > 0x7e) return false;
      ++last;
    }
    if (last > first) out.push_back(line.substr(first, last - first));
    first = last;
  }
  return true;
}

// Decimal only, consumed in full, without locale-dependent or non-finite forms.
inline bool finiteNumber(const std::string& text, double& out) {
  if (text.empty() || text.size() > 64) return false;
  size_t i = 0;
  if (text[i] == '+' || text[i] == '-') ++i;
  size_t digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
  }
  if (!digits) return false;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    size_t exponent = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == exponent) return false;
  }
  if (i != text.size()) return false;
  const char* first = text.data() + (text.front() == '+' ? 1 : 0);
  auto parsed = std::from_chars(first, text.data() + text.size(), out, std::chars_format::general);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && std::isfinite(out);
}

inline bool settingNumber(const SettingSpec& spec, const std::string& text, double& out) {
  return finiteNumber(text, out) && out >= spec.min && out <= spec.max &&
         (!spec.integer() || std::floor(out) == out);
}

inline std::string number(double value) {
  if (!std::isfinite(value)) return "null";
  char buf[64];
  auto result = std::to_chars(buf, buf + sizeof buf, value);
  return result.ec == std::errc() ? std::string(buf, result.ptr) : "null";
}

struct SettingChange { const SettingSpec* spec; double value; };

// key=value arguments, each known, in bounds and given once. testOnly limits
// them to the settings a TEST request may override, and allows none.
inline bool assignments(const std::vector<std::string>& args, bool testOnly,
                        std::vector<SettingChange>& changes, std::string& error) {
  changes.clear();
  std::array<bool, kSettings.size()> seen{};
  if (args.empty() && !testOnly) { error = "no settings supplied"; return false; }
  for (const auto& arg : args) {
    size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0 || arg.find('=', eq + 1) != std::string::npos) {
      error = "expected key=value"; return false;
    }
    const SettingSpec* spec = setting(arg.substr(0, eq));
    if (!spec || (testOnly && !spec->testable)) { error = "unknown setting: " + arg.substr(0, eq); return false; }
    size_t index = spec - kSettings.data();
    if (seen[index]) { error = "duplicate setting: " + std::string(spec->key); return false; }
    seen[index] = true;
    double value;
    if (!settingNumber(*spec, arg.substr(eq + 1), value)) {
      error = "invalid value for " + std::string(spec->key); return false;
    }
    changes.push_back({spec, value});
  }
  return true;
}

inline void changeSettings(Settings& settings, const std::vector<SettingChange>& changes) {
  for (const auto& change : changes) change.spec->set(settings, change.value);
}

inline std::string valuesJson(const Settings& s) {
  std::string out = "{";
  for (const auto& spec : kSettings) {
    if (out.size() > 1) out += ',';
    out += '"' + std::string(spec.key) + "\":" + number(spec.get(s));
  }
  return out + '}';
}

inline std::string boundsJson() {
  std::string out = "{";
  for (const auto& spec : kSettings) {
    if (out.size() > 1) out += ',';
    out += '"' + std::string(spec.key) + "\":{\"min\":" + number(spec.min) +
           ",\"max\":" + number(spec.max) + ",\"step\":" + number(spec.step) +
           ",\"integer\":" + (spec.integer() ? "true" : "false") + '}';
  }
  return out + '}';
}

inline bool validRevision(const std::string& revision) {
  if (revision.empty() || revision.size() > 256) return false;
  for (char c : revision) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == ':' || c == '-')) return false;
  }
  return true;
}

enum class RequestKind { Invalid, Verify, Test, Choose, Reset };
struct Request {
  RequestKind kind = RequestKind::Invalid;
  std::string user, service;
  std::vector<SettingChange> changes;  // TEST only: this scan's settings
};

// VERIFY <user> <service> | TEST <user> [key=value...] | CHOOSE <choice> | RESET.
// TEST is the self-test with candidate settings. It has its own verb so an
// engine that predates it refuses the request instead of ignoring the values.
inline Request request(const std::string& line) {
  std::vector<std::string> parts;
  Request out;
  if (!tokens(line, parts) || parts.empty()) return out;
  if (parts[0] == "VERIFY" && parts.size() == 3) out.kind = RequestKind::Verify;
  else if (parts[0] == "CHOOSE" && parts.size() == 2) out.kind = RequestKind::Choose;
  else if (parts[0] == "RESET" && parts.size() == 1) out.kind = RequestKind::Reset;
  else if (parts[0] == "TEST" && parts.size() >= 2) {
    std::string error;
    if (!assignments({parts.begin() + 2, parts.end()}, true, out.changes, error)) return out;
    out.kind = RequestKind::Test;
    out.user = parts[1];
    out.service = "omaface-test";
    return out;
  }
  if (parts.size() > 1) out.user = parts[1];
  if (parts.size() > 2) out.service = parts[2];
  return out;
}

inline int scaledMinFace(int minFaceWidth, uint32_t cameraWidth) {
  return std::max(30, int(minFaceWidth * (cameraWidth ? cameraWidth : 640) / 640.0));
}

struct Requirements {
  double threshold, liveThreshold;
  int minWidth, requiredFrames;
  bool rgb, attention;
};

inline Requirements requirements(const Settings& s, bool rgb, bool attention, uint32_t width = 640) {
  return {rgb ? s.thresholdRgb : s.threshold, s.livenessThreshold, scaledMinFace(s.minFaceWidth, width),
          rgb ? s.matchFramesRgb : attention ? std::max(s.matchFrames, 3) : s.matchFrames, rgb, attention};
}

inline double missing() { return std::numeric_limits<double>::quiet_NaN(); }
struct Measurement {
  bool skipped = false, found = false, enrolled = true, models = true;
  double width = missing(), score = missing(), live = missing(), liveV2 = missing(), liveV1SE = missing();
  std::string attention = "off";
};

struct GateResult {
  std::string gate;
  int frames, requiredFrames;
  bool passed;
};

class Streak {
 public:
  void reset() { frames_ = 0; }
  int frames() const { return frames_; }
  GateResult judge(const Requirements& r, const Measurement& m) {
    if (m.skipped) return {"noface", frames_, r.requiredFrames, false};
    std::string gate;
    if (!m.found) gate = "noface";
    else if (!std::isfinite(m.width) || m.width <= 0 || !std::isfinite(r.threshold) ||
             !std::isfinite(r.liveThreshold) || r.requiredFrames < 1 || r.minWidth < 30) gate = "models";
    else if (m.width < r.minWidth) gate = "small";
    else if (!m.enrolled) gate = "not-enrolled";
    else if (!m.models || !std::isfinite(m.score) || m.score < -1.00001 || m.score > 1.00001) gate = "models";
    else if (m.score < r.threshold) gate = "nomatch";
    else if (r.attention && m.attention == "unavailable") gate = "models";
    else if (r.attention && m.attention != "ok") gate = "attention";
    else if (r.rgb && (!unit(m.live) || !unit(m.liveV2) || !unit(m.liveV1SE))) gate = "models";
    else if (r.rgb && m.live < r.liveThreshold) gate = "notlive";
    else gate = "pass";
    if (gate == "pass") frames_ = std::min(frames_ + 1, r.requiredFrames);
    else reset();
    return {gate, frames_, r.requiredFrames, gate == "pass" && frames_ >= r.requiredFrames};
  }
 private:
  static bool unit(double v) { return std::isfinite(v) && v >= 0 && v <= 1; }
  int frames_ = 0;
};

}  // namespace omaface
#endif
