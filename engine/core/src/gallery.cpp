// The enrolled appearances: file format and matching.
#include "omaface/core.hpp"

#include <opencv2/core/persistence.hpp>

#include <algorithm>
#include <functional>
#include <numeric>

namespace omaface {

std::vector<Appearance> readGallery(const std::string& path) {
  std::vector<Appearance> out;
  try {
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) return out;
    cv::FileNode list = fs["appearances"];
    for (auto it = list.begin(); it != list.end(); ++it) {
      Appearance a;
      (*it)["name"] >> a.name;
      (*it)["created"] >> a.created;
      (*it)["features"] >> a.features;
      (*it)["head_yaw"] >> a.headYaw;
      (*it)["head_pitch"] >> a.headPitch;
      cv::FileNode gaze = (*it)["gaze"];
      if (gaze.isSeq() && gaze.size() == 2) {
        a.gazeYaw = (float)gaze[0];
        a.gazePitch = (float)gaze[1];
        a.hasGaze = true;
      }
      std::string kind;
      (*it)["kind"] >> kind;
      if (kind == "rgb") a.kind = kind;
      if (a.features.empty() || a.features.cols != kFeatureSize || a.features.type() != CV_32F) continue;
      out.push_back(a);
    }
  } catch (const cv::Exception&) {
    out.clear();
  }
  return out;
}

bool writeGallery(const std::string& path, const std::vector<Appearance>& gallery, std::string& err) {
  try {
    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) { err = "cannot write gallery"; return false; }
    fs << "version" << 1;
    fs << "appearances" << "[";
    for (auto& a : gallery)
      fs << "{" << "name" << a.name << "created" << a.created << "kind" << a.kind << "head_yaw" << a.headYaw << "head_pitch" << a.headPitch
         << "gaze" << "[" << a.gazeYaw << a.gazePitch << "]"
         << "features" << a.features << "}";
    fs << "]";
    fs.release();
  } catch (const cv::Exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

// How well a face matches its closest appearance: the mean of its five best
// samples there, not the single best one. One lucky sample out of 25 is how
// a stranger gets close; five at once are not. Against 13,000 public faces
// (LFW, also turned grey and IR-like) the top-5 mean kept strangers lower
// for the same genuine scores: the IR-like tail at one in 10,000 fell from
// 0.53 to 0.48, while the owner's live IR frames lost only about 0.01.
double bestScore(const std::vector<Appearance>& gallery, const cv::Mat& feat, size_t* which) {
  const int kTop = 5;
  double best = -1;
  for (size_t i = 0; i < gallery.size(); i++) {
    cv::Mat scores = gallery[i].features * feat.t();
    std::vector<float> s(scores.begin<float>(), scores.end<float>());
    size_t k = std::min<size_t>(kTop, s.size());
    std::partial_sort(s.begin(), s.begin() + k, s.end(), std::greater<float>());
    double mean = std::accumulate(s.begin(), s.begin() + k, 0.0) / k;
    if (mean > best) {
      best = mean;
      if (which) *which = i;
    }
  }
  return best;
}

}  // namespace omaface
