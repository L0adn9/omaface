#ifndef OMAFACE_CONFIG_SETTINGS_H
#define OMAFACE_CONFIG_SETTINGS_H

#include "calibration.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omaface {

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return fd_; }
 private:
  int fd_;
};

struct ConfigDocument {
  std::string text, revision;
  bool exists = false;
};

inline std::string hex(uint64_t value) {
  char buf[32];
  auto out = std::to_chars(buf, buf + sizeof buf, value, 16);
  return std::string(buf, out.ptr);
}

inline bool readConfigDocument(const std::string& path, ConfigDocument& document, std::string& error) {
  document = {};
  FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0 && errno != ENOENT) { error = "cannot read config: " + std::string(strerror(errno)); return false; }
  struct stat st{};
  if (fd.get() >= 0) {
    if (fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) { error = "config is not a regular file"; return false; }
    document.exists = true;
    char buf[4096];
    while (true) {
      ssize_t count = read(fd.get(), buf, sizeof buf);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) { error = "cannot read config"; return false; }
      if (!count) break;
      document.text.append(buf, count);
      if (document.text.size() > 1024 * 1024) { error = "config is too large"; return false; }
    }
  }
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : document.text) { hash ^= c; hash *= 1099511628211ULL; }
  document.revision = "v1-" + hex(hash) + '-' + hex(st.st_dev) + '-' + hex(st.st_ino) + '-' +
                      hex(st.st_mtim.tv_sec) + '-' + hex(st.st_mtim.tv_nsec) + '-' +
                      hex(st.st_ctim.tv_sec) + '-' + hex(st.st_ctim.tv_nsec) + (document.exists ? "-file" : "-missing");
  return true;
}

inline bool configLine(const std::string& line, std::string& key, std::string& value) {
  auto hash = line.find('#');
  std::string body = line.substr(0, hash);
  auto eq = body.find('=');
  if (eq == std::string::npos) return false;
  key = trim(body.substr(0, eq));
  value = trim(body.substr(eq + 1));
  return true;
}

inline Settings numericSettings(const std::string& text) {
  Settings out;
  size_t first = 0;
  while (first < text.size()) {
    size_t end = text.find('\n', first);
    std::string key, value;
    if (configLine(text.substr(first, end == std::string::npos ? end : end - first), key, value)) {
      if (const SettingSpec* spec = setting(key)) {
        double v;
        spec->set(out, settingNumber(*spec, value, v) ? v : spec->fallback);
      }
    }
    if (end == std::string::npos) break;
    first = end + 1;
  }
  return out;
}

inline std::string replaceSettings(const std::string& text, const std::vector<SettingChange>& changes) {
  std::vector<bool> seen(changes.size(), false);
  std::string out;
  size_t first = 0;
  while (first < text.size()) {
    size_t end = text.find('\n', first);
    std::string line = text.substr(first, end == std::string::npos ? end : end - first);
    std::string key, value;
    if (configLine(line, key, value)) {
      for (size_t i = 0; i < changes.size(); ++i) {
        if (key != changes[i].spec->key) continue;
        seen[i] = true;
        size_t start = line.find('=') + 1, stop = line.find('#');
        if (stop == std::string::npos) stop = line.size();
        while (start < stop && (line[start] == ' ' || line[start] == '\t')) ++start;
        size_t tail = stop;
        while (tail > start && space(line[tail - 1])) --tail;
        std::string suffix = line.substr(tail);
        if (!suffix.empty() && suffix.front() == '#') suffix.insert(0, " ");
        line = line.substr(0, start) + number(changes[i].value) + suffix;
        break;
      }
    }
    out += line;
    if (end == std::string::npos) break;
    out += '\n';
    first = end + 1;
  }
  for (size_t i = 0; i < changes.size(); ++i) {
    if (seen[i]) continue;
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += std::string(changes[i].spec->key) + '=' + number(changes[i].value) + '\n';
  }
  return out;
}

inline bool settingsWriterAllowed() {
#ifdef OMAFACE_TEST_SETTINGS_ROOT
  // Only the test driver defines this; no production or OMAFACE_DEV bypass.
  return true;
#else
  return geteuid() == 0;
#endif
}

inline bool applySettings(const std::string& path, const std::string& revision,
                          const std::vector<SettingChange>& changes, ConfigDocument& readback,
                          std::string& error) {
  if (!settingsWriterAllowed()) { error = "apply-settings must run as root"; return false; }
  if (!validRevision(revision)) { error = "invalid config revision"; return false; }
  if (changes.empty()) { error = "no settings supplied"; return false; }
  // Even C++ callers cannot bypass the authoritative bounds before writing.
  std::vector<std::string> args;
  for (const auto& change : changes) {
    if (!change.spec) { error = "unknown setting"; return false; }
    bool known = false;
    for (const auto& spec : kSettings) if (&spec == change.spec) known = true;
    if (!known) { error = "unknown setting"; return false; }
    args.push_back(std::string(change.spec->key) + '=' + number(change.value));
  }
  std::vector<SettingChange> checked;
  if (!assignments(args, false, checked, error)) return false;

  size_t slash = path.rfind('/');
  if (slash == std::string::npos) { error = "config path must be absolute"; return false; }
  std::string parent = slash == 0 ? "/" : path.substr(0, slash), name = path.substr(slash + 1);
  FileDescriptor dir(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  struct stat st{};
  if (dir.get() < 0 || fstat(dir.get(), &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
    error = "config directory must be owned by the writer and not writable by others"; return false;
  }
  FileDescriptor lock(::openat(dir.get(), ".config.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
  if (lock.get() < 0 || fstat(lock.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) {
    error = "cannot open config lock"; return false;
  }
  // flock interoperates with the existing Bash conf_set writer.
  while (flock(lock.get(), LOCK_EX) != 0) {
    if (errno == EINTR) continue;
    error = "cannot lock config"; return false;
  }
  ConfigDocument current;
  if (!readConfigDocument(path, current, error)) return false;
  if (revision != "-" && revision != current.revision) { error = "stale config revision; reload settings"; return false; }
  std::string text = replaceSettings(current.text, checked);
  if (text.size() > 1024 * 1024) { error = "config is too large"; return false; }
  if (text == current.text) { readback = current; return true; }

  uint64_t random;
  if (getrandom(&random, sizeof random, 0) != sizeof random) { error = "cannot create config temporary file"; return false; }
  std::string tmp = ".config.tmp." + hex(random);
  FileDescriptor fd(::openat(dir.get(), tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (fd.get() < 0) { error = "cannot create config temporary file"; return false; }
  auto fail = [&](const char* msg) { unlinkat(dir.get(), tmp.c_str(), 0); error = msg; return false; };
  size_t offset = 0;
  while (offset < text.size()) {
    ssize_t count = write(fd.get(), text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return fail("cannot write config");
    offset += count;
  }
  if (fchmod(fd.get(), 0644) != 0 || fsync(fd.get()) != 0) return fail("cannot prepare config");
  if (renameat(dir.get(), tmp.c_str(), dir.get(), name.c_str()) != 0) return fail("cannot install config");
  if (fsync(dir.get()) != 0) { error = "cannot sync config directory (config was saved)"; return false; }
  return readConfigDocument(path, readback, error);
}

}  // namespace omaface
#endif
