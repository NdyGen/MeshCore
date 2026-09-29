#pragma once

// Just enough JSON for test/rdm_vectors: objects, arrays, strings, integers, true/false/null.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minijson {

struct Value {
  enum class Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = Type::NUL;
  bool b = false;
  int64_t num = 0;
  std::string str;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  bool isNull() const { return type == Type::NUL; }
  bool has(const std::string& key) const {
    for (auto& kv : obj) if (kv.first == key) return true;
    return false;
  }
  const Value& operator[](const std::string& key) const {
    for (auto& kv : obj) if (kv.first == key) return kv.second;
    throw std::runtime_error("missing key " + key);
  }
  const Value& operator[](size_t i) const { return arr.at(i); }
  size_t size() const { return type == Type::ARR ? arr.size() : obj.size(); }
  int64_t asInt() const {
    if (type != Type::NUM) throw std::runtime_error("not a number");
    return num;
  }
  const std::string& asStr() const {
    if (type != Type::STR) throw std::runtime_error("not a string");
    return str;
  }
};

class Parser {
public:
  explicit Parser(const std::string& s) : _s(s) {}

  Value parse() {
    Value v = value();
    ws();
    if (_i != _s.size()) fail("trailing data");
    return v;
  }

private:
  const std::string& _s;
  size_t _i = 0;

  [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + " at " + std::to_string(_i)); }
  void ws() { while (_i < _s.size() && (_s[_i] == ' ' || _s[_i] == '\n' || _s[_i] == '\r' || _s[_i] == '\t')) _i++; }
  char peek() { ws(); if (_i >= _s.size()) fail("unexpected end"); return _s[_i]; }
  void expect(char c) { if (peek() != c) fail("unexpected character"); _i++; }
  bool literal(const char* lit) {
    size_t n = strlen(lit);
    if (_s.compare(_i, n, lit) != 0) return false;
    _i += n;
    return true;
  }

  Value value() {
    char c = peek();
    Value v;
    if (c == '{') {
      v.type = Value::Type::OBJ;
      _i++;
      if (peek() == '}') { _i++; return v; }
      for (;;) {
        std::string k = string();
        expect(':');
        v.obj.emplace_back(k, value());
        if (peek() == ',') { _i++; continue; }
        expect('}');
        return v;
      }
    }
    if (c == '[') {
      v.type = Value::Type::ARR;
      _i++;
      if (peek() == ']') { _i++; return v; }
      for (;;) {
        v.arr.push_back(value());
        if (peek() == ',') { _i++; continue; }
        expect(']');
        return v;
      }
    }
    if (c == '"') { v.type = Value::Type::STR; v.str = string(); return v; }
    if (literal("true")) { v.type = Value::Type::BOOL; v.b = true; return v; }
    if (literal("false")) { v.type = Value::Type::BOOL; return v; }
    if (literal("null")) return v;
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = _i;
      if (_s[_i] == '-') _i++;
      while (_i < _s.size() && _s[_i] >= '0' && _s[_i] <= '9') _i++;
      if (_i < _s.size() && (_s[_i] == '.' || _s[_i] == 'e' || _s[_i] == 'E')) fail("only integers supported");
      v.type = Value::Type::NUM;
      v.num = strtoll(_s.c_str() + start, nullptr, 10);
      return v;
    }
    fail("unexpected value");
  }

  std::string string() {
    expect('"');
    std::string out;
    while (_i < _s.size() && _s[_i] != '"') {
      char c = _s[_i++];
      if (c != '\\') { out += c; continue; }
      if (_i >= _s.size()) fail("bad escape");
      char e = _s[_i++];
      switch (e) {
        case '"': case '\\': case '/': out += e; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'u': {
          if (_i + 4 > _s.size()) fail("bad \\u escape");
          long cp = strtol(_s.substr(_i, 4).c_str(), nullptr, 16);
          _i += 4;
          if (cp >= 0x80) fail("only ASCII \\u escapes supported");
          out += (char)cp;
          break;
        }
        default: fail("bad escape");
      }
    }
    expect('"');
    return out;
  }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

}
