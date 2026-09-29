#include <FS.h>

#include <algorithm>

#include "Sim.h"

namespace sim { Simulator* activeSimulator(); }

namespace fs {

std::vector<uint8_t>* File::data() const {
  if (!_h || _h->is_dir) return nullptr;
  auto it = _h->store->files.find(_h->path);
  return it == _h->store->files.end() ? nullptr : &it->second;
}

File File::openFile(sim::SimFS& store, const std::string& path, bool readable, bool writable, bool append) {
  File f;
  f._h = std::make_shared<Handle>(Handle{&store, path, 0, readable, writable, append, false, {}, 0});
  if (append) f._h->pos = store.files[path].size();
  return f;
}

File File::openDir(sim::SimFS& store, const std::string& path) {
  std::string prefix = path;
  if (prefix.empty() || prefix.back() != '/') prefix += '/';
  File f;
  f._h = std::make_shared<Handle>(Handle{&store, path, 0, false, false, false, true, {}, 0});
  for (const auto& e : store.files) {
    if (e.first.compare(0, prefix.size(), prefix) == 0 && e.first.find('/', prefix.size()) == std::string::npos) {
      f._h->dir_entries.push_back(e.first);
    }
  }
  return f;
}

size_t File::write(const uint8_t* buf, size_t len) {
  if (!_h || !_h->writable) return 0;
  std::vector<uint8_t>* d = data();
  if (!d) d = &_h->store->files[_h->path];
  sim::SimFS& s = *_h->store;
  if (_h->append) _h->pos = d->size();

  size_t n = len;
  if (s.write_budget >= 0) n = std::min<size_t>(n, (size_t)s.write_budget);
  size_t end = _h->pos + n;
  size_t growth = end > d->size() ? end - d->size() : 0;
  if (s.used_bytes() + growth > s.capacity_bytes) {
    size_t room = s.capacity_bytes > s.used_bytes() ? s.capacity_bytes - s.used_bytes() : 0;
    n = std::min(n, (d->size() - std::min(d->size(), _h->pos)) + room);
    end = _h->pos + n;
  }
  if (end > d->size()) d->resize(end);
  uint32_t off = (uint32_t)_h->pos;
  std::copy(buf, buf + n, d->begin() + _h->pos);
  _h->pos = end;
  if (s.write_budget >= 0) s.write_budget -= (long)n;
  s.report_write(_h->path, off, (uint32_t)n, n == len);
  return n;
}

int File::available() {
  std::vector<uint8_t>* d = data();
  if (!d || !_h->readable) return 0;
  return _h->pos < d->size() ? (int)(d->size() - _h->pos) : 0;
}

int File::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}

int File::peek() {
  std::vector<uint8_t>* d = data();
  if (!d || !_h->readable || _h->pos >= d->size()) return -1;
  return (*d)[_h->pos];
}

size_t File::read(uint8_t* buf, size_t len) {
  std::vector<uint8_t>* d = data();
  if (!d || !_h->readable || _h->pos >= d->size()) return 0;
  size_t n = std::min(len, d->size() - _h->pos);
  std::copy(d->begin() + _h->pos, d->begin() + _h->pos + n, buf);
  _h->pos += n;
  return n;
}

bool File::seek(uint32_t pos, SeekMode mode) {
  if (!_h) return false;
  size_t base = mode == SeekSet ? 0 : (mode == SeekCur ? _h->pos : size());
  _h->pos = base + pos;
  return true;
}

size_t File::size() const {
  std::vector<uint8_t>* d = data();
  return d ? d->size() : 0;
}

const char* File::name() const {
  if (!_h) return "";
  size_t slash = _h->path.rfind('/');
  return _h->path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

File File::openNextFile(const char*) {
  if (!_h || !_h->is_dir || _h->dir_next >= _h->dir_entries.size()) return File();
  return openFile(*_h->store, _h->dir_entries[_h->dir_next++], true, false, false);
}

static bool isDirPath(sim::SimFS& s, const std::string& path) {
  std::string prefix = path;
  if (prefix.empty() || prefix.back() != '/') prefix += '/';
  for (const auto& e : s.files) {
    if (e.first.compare(0, prefix.size(), prefix) == 0) return true;
  }
  return path == "/";
}

File FS::open(const char* path, const char* mode, bool create) {
  sim::SimFS& s = store();
  std::string p(path);
  std::string m(mode ? mode : "r");
  bool plus = m.find('+') != std::string::npos;
  if (m[0] == 'r') {
    if (!s.exists(p)) return isDirPath(s, p) ? File::openDir(s, p) : File();
    return File::openFile(s, p, true, plus, false);
  }
  if (m[0] == 'w') {
    s.files[p].clear();
    return File::openFile(s, p, plus, true, false);
  }
  if (m[0] == 'a') return File::openFile(s, p, plus, true, true);
  return File();
}

bool FS::exists(const char* path) { return store().exists(path) || isDirPath(store(), path); }

bool FS::remove(const char* path) { return store().files.erase(path) != 0; }

bool FS::rename(const char* from, const char* to) {
  auto& files = store().files;
  auto it = files.find(from);
  if (it == files.end()) return false;
  std::vector<uint8_t> data = std::move(it->second);
  files.erase(it);
  files[to] = std::move(data);
  return true;
}

bool FS::info(FSInfo& info) {
  info.totalBytes = store().capacity_bytes;
  info.usedBytes = store().used_bytes();
  return true;
}

sim::SimFS& CurrentNodeFS::store() {
  static sim::SimFS none;
  sim::Simulator* s = sim::activeSimulator();
  if (!s || !s->current()) return none;
  return s->current()->fs();
}

}
