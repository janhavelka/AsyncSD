#include "SdFat.h"

#include <errno.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Arduino.h"

namespace {

struct Entry {
  bool isDir = false;
  std::vector<uint8_t> data{};
};

std::unordered_map<std::string, Entry> g_entries;
bool g_mounted = false;
SdFs* g_activeFs = nullptr;
int32_t g_lastSdError = 0;
uint32_t g_renameDelayMs = 0;
int g_forcedRenameErrno = 0;

void setFsError(int err) {
  errno = err;
  g_lastSdError = err;
}

std::string normalizePath(const char* path) {
  if (!path || path[0] == '\0') {
    return std::string{};
  }
  std::string normalized(path);
  if (normalized[0] != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

void ensureRoot() {
  if (g_entries.find("/") == g_entries.end()) {
    g_entries["/"] = Entry{true, {}};
  }
}

std::string parentPath(const std::string& path) {
  if (path.empty() || path == "/") {
    return std::string{};
  }
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return std::string{};
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

bool hasChildren(const std::string& path) {
  const std::string prefix = (path == "/") ? "/" : (path + "/");
  for (const auto& kv : g_entries) {
    if (kv.first.size() > prefix.size() &&
        kv.first.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  return false;
}

Entry* getEntry(const std::string& path) {
  const auto it = g_entries.find(path);
  if (it == g_entries.end()) {
    return nullptr;
  }
  return &it->second;
}

const Entry* getEntryConst(const std::string& path) {
  const auto it = g_entries.find(path);
  if (it == g_entries.end()) {
    return nullptr;
  }
  return &it->second;
}

bool createDirectoryRecursive(const std::string& target) {
  if (target.empty() || target == "/") {
    return true;
  }
  std::string cursor;
  size_t start = 1;
  while (start < target.size()) {
    const size_t next = target.find('/', start);
    if (next == std::string::npos) {
      cursor = target;
      start = target.size();
    } else {
      cursor = target.substr(0, next);
      start = next + 1;
    }
    Entry* existing = getEntry(cursor);
    if (!existing) {
      g_entries[cursor] = Entry{true, {}};
    } else if (!existing->isDir) {
      setFsError(ENOTDIR);
      return false;
    }
  }
  return true;
}

}  // namespace

uint8_t SdCard::type() const {
  return SD_CARD_TYPE_SDHC;
}

uint32_t SdCard::sectorCount() const {
  return 4U * 1024U * 1024U / 512U;
}

uint32_t SdCard::status() const {
  return 0;
}

bool SdCard::readOCR(uint32_t* out) const {
  if (!out) {
    return false;
  }
  *out = 0x00FF8000UL;
  return true;
}

bool SdCard::readCID(cid_t* out) const {
  if (!out) {
    return false;
  }
  for (size_t i = 0; i < sizeof(out->raw); ++i) {
    out->raw[i] = static_cast<uint8_t>(i);
  }
  return true;
}

bool SdCard::readCSD(csd_t* out) const {
  if (!out) {
    return false;
  }
  for (size_t i = 0; i < sizeof(out->raw); ++i) {
    out->raw[i] = static_cast<uint8_t>(0xA0 + i);
  }
  return true;
}

bool SdCard::readSCR(scr_t* out) const {
  if (!out) {
    return false;
  }
  for (size_t i = 0; i < sizeof(out->raw); ++i) {
    out->raw[i] = static_cast<uint8_t>(0xB0 + i);
  }
  return true;
}

bool SdCard::readSDS(sds_t* out) const {
  if (!out) {
    return false;
  }
  for (size_t i = 0; i < sizeof(out->raw); ++i) {
    out->raw[i] = static_cast<uint8_t>(0xC0 + (i % 16));
  }
  return true;
}

bool FsFile::open(const char* path, uint8_t flags) {
  if (!g_mounted || !g_activeFs) {
    setFsError(ENODEV);
    return false;
  }

  ensureRoot();
  const std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    setFsError(EINVAL);
    return false;
  }

  const bool wantRead = ((flags & O_RDWR) == O_RDWR) || ((flags & O_READ) != 0);
  const bool wantWrite = ((flags & O_RDWR) == O_RDWR) || ((flags & O_WRITE) != 0);
  const bool create = (flags & O_CREAT) != 0;
  const bool exclusive = (flags & O_EXCL) != 0;
  const bool truncate = (flags & O_TRUNC) != 0;
  const bool append = (flags & O_APPEND) != 0;

  Entry* entry = getEntry(normalized);
  if (!entry) {
    if (!create) {
      setFsError(ENOENT);
      return false;
    }
    const std::string parent = parentPath(normalized);
    const Entry* parentEntry = getEntryConst(parent.empty() ? "/" : parent);
    if (!parentEntry || !parentEntry->isDir) {
      setFsError(ENOENT);
      return false;
    }
    g_entries[normalized] = Entry{false, {}};
    entry = getEntry(normalized);
  } else if (create && exclusive) {
    setFsError(EEXIST);
    return false;
  }

  if (!entry) {
    setFsError(EIO);
    return false;
  }

  if (entry->isDir && (wantWrite || truncate)) {
    setFsError(EISDIR);
    return false;
  }

  if (!entry->isDir && truncate && wantWrite) {
    entry->data.clear();
  }

  _path = normalized;
  _open = true;
  _readable = wantRead || (!wantWrite && !wantRead);
  _writable = wantWrite;
  _pos = append && !entry->isDir ? entry->data.size() : 0;
  setFsError(0);
  return true;
}

void FsFile::close() {
  _open = false;
  _readable = false;
  _writable = false;
  _path.clear();
  _pos = 0;
}

int32_t FsFile::read(uint8_t* dst, uint32_t len) {
  if (!_open || !_readable || !dst) {
    setFsError(EBADF);
    return -1;
  }
  Entry* entry = getEntry(_path);
  if (!entry) {
    setFsError(ENOENT);
    return -1;
  }
  if (entry->isDir) {
    setFsError(EISDIR);
    return -1;
  }
  if (_pos >= entry->data.size()) {
    setFsError(0);
    return 0;
  }
  const uint64_t remaining = static_cast<uint64_t>(entry->data.size()) - _pos;
  const uint32_t toRead = (remaining < len) ? static_cast<uint32_t>(remaining) : len;
  memcpy(dst, entry->data.data() + _pos, toRead);
  _pos += toRead;
  setFsError(0);
  return static_cast<int32_t>(toRead);
}

size_t FsFile::write(const uint8_t* src, uint32_t len) {
  if (!_open || !_writable || !src) {
    setFsError(EBADF);
    return 0;
  }
  Entry* entry = getEntry(_path);
  if (!entry) {
    setFsError(ENOENT);
    return 0;
  }
  if (entry->isDir) {
    setFsError(EISDIR);
    return 0;
  }
  if (_pos > entry->data.size()) {
    entry->data.resize(static_cast<size_t>(_pos), 0);
  }
  const uint64_t endPos = _pos + len;
  if (endPos > entry->data.size()) {
    entry->data.resize(static_cast<size_t>(endPos));
  }
  memcpy(entry->data.data() + _pos, src, len);
  _pos = endPos;
  setFsError(0);
  return len;
}

bool FsFile::seekSet(uint64_t pos) {
  if (!_open) {
    setFsError(EBADF);
    return false;
  }
  _pos = pos;
  setFsError(0);
  return true;
}

bool FsFile::seekEnd() {
  if (!_open) {
    setFsError(EBADF);
    return false;
  }
  const Entry* entry = getEntryConst(_path);
  if (!entry || entry->isDir) {
    setFsError(EBADF);
    return false;
  }
  _pos = entry->data.size();
  setFsError(0);
  return true;
}

bool FsFile::sync() {
  if (!_open) {
    setFsError(EBADF);
    return false;
  }
  setFsError(0);
  return true;
}

uint64_t FsFile::fileSize() const {
  const Entry* entry = getEntryConst(_path);
  if (!entry || entry->isDir) {
    return 0;
  }
  return entry->data.size();
}

bool FsFile::isDir() const {
  const Entry* entry = getEntryConst(_path);
  return entry ? entry->isDir : false;
}

bool SdFs::begin(const SdSpiConfig& /*cfg*/) {
  ensureRoot();
  g_mounted = true;
  g_activeFs = this;
  setFsError(0);
  return true;
}

void SdFs::end() {
  g_mounted = false;
  if (g_activeFs == this) {
    g_activeFs = nullptr;
  }
  setFsError(0);
}

SdCard* SdFs::card() {
  return &_card;
}

uint8_t SdFs::fatType() const {
  return 32;
}

uint32_t SdFs::sectorsPerCluster() const {
  return 8;
}

uint32_t SdFs::bytesPerCluster() const {
  return 4096;
}

uint32_t SdFs::clusterCount() const {
  return 1024;
}

int32_t SdFs::freeClusterCount() const {
  return 768;
}

bool SdFs::exists(const char* path) const {
  const std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    return false;
  }
  return g_entries.find(normalized) != g_entries.end();
}

bool SdFs::mkdir(const char* path, bool recursive) {
  ensureRoot();
  const std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    setFsError(EINVAL);
    return false;
  }
  if (normalized == "/") {
    setFsError(0);
    return true;
  }

  Entry* existing = getEntry(normalized);
  if (existing) {
    if (existing->isDir) {
      setFsError(0);
      return true;
    }
    setFsError(EEXIST);
    return false;
  }

  if (recursive) {
    if (!createDirectoryRecursive(normalized)) {
      return false;
    }
    setFsError(0);
    return true;
  }

  const std::string parent = parentPath(normalized);
  const Entry* parentEntry = getEntryConst(parent.empty() ? "/" : parent);
  if (!parentEntry || !parentEntry->isDir) {
    setFsError(ENOENT);
    return false;
  }
  g_entries[normalized] = Entry{true, {}};
  setFsError(0);
  return true;
}

bool SdFs::remove(const char* path) {
  const std::string normalized = normalizePath(path);
  if (normalized.empty() || normalized == "/") {
    setFsError(EINVAL);
    return false;
  }
  Entry* entry = getEntry(normalized);
  if (!entry) {
    setFsError(ENOENT);
    return false;
  }
  if (entry->isDir) {
    setFsError(EISDIR);
    return false;
  }
  g_entries.erase(normalized);
  setFsError(0);
  return true;
}

bool SdFs::rmdir(const char* path) {
  const std::string normalized = normalizePath(path);
  if (normalized.empty() || normalized == "/") {
    setFsError(EINVAL);
    return false;
  }
  Entry* entry = getEntry(normalized);
  if (!entry) {
    setFsError(ENOENT);
    return false;
  }
  if (!entry->isDir) {
    setFsError(ENOTDIR);
    return false;
  }
  if (hasChildren(normalized)) {
    setFsError(ENOTEMPTY);
    return false;
  }
  g_entries.erase(normalized);
  setFsError(0);
  return true;
}

bool SdFs::rename(const char* fromPath, const char* toPath) {
  if (g_renameDelayMs > 0) {
    arduino_stub::advanceMs(g_renameDelayMs);
  }
  if (g_forcedRenameErrno != 0) {
    setFsError(g_forcedRenameErrno);
    return false;
  }

  const std::string from = normalizePath(fromPath);
  const std::string to = normalizePath(toPath);
  if (from.empty() || to.empty() || from == "/" || to == "/") {
    setFsError(EINVAL);
    return false;
  }
  Entry* srcEntry = getEntry(from);
  if (!srcEntry) {
    setFsError(ENOENT);
    return false;
  }
  if (getEntry(to)) {
    setFsError(EEXIST);
    return false;
  }
  const std::string parent = parentPath(to);
  const Entry* parentEntry = getEntryConst(parent.empty() ? "/" : parent);
  if (!parentEntry || !parentEntry->isDir) {
    setFsError(ENOENT);
    return false;
  }

  Entry moved = *srcEntry;
  g_entries.erase(from);
  g_entries[to] = moved;

  if (moved.isDir) {
    const std::string fromPrefix = from + "/";
    const std::string toPrefix = to + "/";
    std::vector<std::pair<std::string, Entry>> descendants;
    descendants.reserve(g_entries.size());
    for (const auto& kv : g_entries) {
      if (kv.first.compare(0, fromPrefix.size(), fromPrefix) == 0) {
        std::string dst = toPrefix + kv.first.substr(fromPrefix.size());
        descendants.push_back({dst, kv.second});
      }
    }
    for (const auto& kv : descendants) {
      const std::string oldPath = fromPrefix + kv.first.substr(toPrefix.size());
      g_entries.erase(oldPath);
      g_entries[kv.first] = kv.second;
    }
  }

  setFsError(0);
  return true;
}

int32_t SdFs::sdErrorCode() const {
  return g_lastSdError;
}

namespace sdfat_stub {

void reset() {
  g_entries.clear();
  ensureRoot();
  g_mounted = false;
  g_activeFs = nullptr;
  g_lastSdError = 0;
  g_renameDelayMs = 0;
  g_forcedRenameErrno = 0;
  errno = 0;
}

void setRenameDelayMs(uint32_t delayMs) {
  g_renameDelayMs = delayMs;
}

void setForcedRenameErrno(int err) {
  g_forcedRenameErrno = err;
}

bool pathExists(const char* path) {
  const std::string normalized = normalizePath(path);
  if (normalized.empty()) {
    return false;
  }
  return g_entries.find(normalized) != g_entries.end();
}

}  // namespace sdfat_stub
