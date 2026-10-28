// rd_host_service — rd_host.ini 解析实现，见 host_config.h 的格式说明。
#include "host_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "debug_log.h"

namespace rdcfg {
namespace {

std::wstring ExeDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p = path;
  const size_t s = p.find_last_of(L'\\');
  return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::string Trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) {
    return {};
  }
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
  return s;
}

// CreateFileW + ReadFile：MinGW 的 std::ifstream 不认宽路径。
bool ReadWholeFile(const std::wstring& path, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(h, &size) || size.QuadPart > 64 * 1024) {
    CloseHandle(h);  // 64 KiB 上限：配置文件不该更大，防误读坏盘
    return false;
  }
  out->assign(static_cast<size_t>(size.QuadPart), '\0');
  DWORD got = 0;
  const BOOL ok = ReadFile(h, out->data(), static_cast<DWORD>(size.QuadPart),
                          &got, nullptr);
  CloseHandle(h);
  if (!ok) {
    return false;
  }
  out->resize(got);
  return true;
}

// 行内拆 key=value；注释行/空行返回 false。key 统一小写。
bool SplitKeyValue(const std::string& line, std::string* key,
                   std::string* value) {
  const std::string t = Trim(line);
  if (t.empty() || t[0] == '#' || t[0] == ';') {
    return false;
  }
  const size_t eq = t.find('=');
  if (eq == std::string::npos) {
    rdDebug("config", "ignored line (no '='): %s", t.c_str());
    return false;
  }
  *key = Lower(Trim(t.substr(0, eq)));
  *value = Trim(t.substr(eq + 1));
  return !key->empty();
}

// 端口必须全数字且落在 1-65535；其余一律按非法处理。
bool ParsePort(const std::string& s, unsigned short* out) {
  if (s.empty() || s.size() > 5) {
    return false;
  }
  for (char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  const long v = std::strtol(s.c_str(), nullptr, 10);
  if (v < 1 || v > 65535) {
    return false;
  }
  *out = static_cast<unsigned short>(v);
  return true;
}

// stun 值按逗号再拆（允许 "a:1, b:2"）；off/空由调用方先判定。
void AppendStunList(const std::string& value, std::vector<std::string>* out) {
  size_t start = 0;
  for (;;) {
    const size_t comma = value.find(',', start);
    const std::string item =
        Trim(value.substr(start, comma == std::string::npos
                                     ? std::string::npos
                                     : comma - start));
    if (!item.empty()) {
      out->push_back(item);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
}

}  // namespace

HostConfig LoadHostConfig() {
  HostConfig cfg;
  const std::wstring path = ExeDir() + L"\\rd_host.ini";
  std::string text;
  if (!ReadWholeFile(path, &text)) {
    rdDebug("config", "no readable rd_host.ini beside exe — defaults apply "
                      "(port=%u, default stun)", kDefaultSignalPort);
    return cfg;
  }
  cfg.fileExists = true;

  // UTF-8 BOM 跳过；port/stun 值限定 ASCII，多字节内容不会误伤。
  size_t pos = 0;
  if (text.size() >= 3 && (text[0] & 0xFF) == 0xEF &&
      (text[1] & 0xFF) == 0xBB && (text[2] & 0xFF) == 0xBF) {
    pos = 3;
  }

  bool haveStunKey = false;
  bool havePort = false;
  std::vector<std::string> stun;
  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    const std::string line =
        text.substr(pos, eol == std::string::npos ? std::string::npos
                                                  : eol - pos);
    std::string key, value;
    if (SplitKeyValue(line, &key, &value)) {
      if (key == "port") {
        unsigned short p = 0;
        if (ParsePort(value, &p)) {
          cfg.port = p;
          havePort = true;
        } else {
          rdDebug("config", "bad port='%s' — fallback %u", value.c_str(),
                  kDefaultSignalPort);
        }
      } else if (key == "stun") {
        haveStunKey = true;
        const std::string v = Lower(value);
        if (v.empty() || v == "off" || v == "none" || v == "disabled") {
          cfg.stunOff = true;
        } else {
          AppendStunList(value, &stun);
        }
      } else {
        rdDebug("config", "unknown key '%s' ignored", key.c_str());
      }
    }
    if (eol == std::string::npos) {
      break;
    }
    pos = eol + 1;
  }

  if (haveStunKey) {
    // 写了 stun= 就完全以配置为准：off → 空列表；非 off → 只用配置的列表。
    cfg.stunServers = stun;
    if (!cfg.stunOff && stun.empty()) {
      rdDebug("config", "stun key present but no server parsed — treated off");
      cfg.stunOff = true;
      cfg.stunServers.clear();
    }
  }
  rdDebug("config", "loaded port=%u%s stun=%zu servers%s", cfg.port,
          havePort ? "" : "(default)", cfg.stunServers.size(),
          cfg.stunOff ? " (STUN off)" : "");
  return cfg;
}

}  // namespace rdcfg
