// rd_host_service — 命名管道协议（helper 服务端 / 控制面板客户端共用）。
//
// 帧：uint32 小端长度 + UTF-8 JSON 负载（上限 1MB）。消息一律是扁平 JSON
// 对象；数组只用于 info 消息的 screenIds/screenTitles 两个平行数组。
// 这里自带一个"够用就好"的迷你 JSON 读写器：只支持扁平对象 + 标量/字符串/
// 两个平行数组，SDP 里的 \r\n 等控制符会被转义/还原。
#ifndef REMOTE_DESKTOP_SERVICE_PIPE_PROTO_H_
#define REMOTE_DESKTOP_SERVICE_PIPE_PROTO_H_

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rdpipe {

// SCM 里的服务注册名。服务本体（注册/卸载）与 GUI 面板（状态探测）共用，
// 避免改名时两处失同步。
constexpr wchar_t kServiceName[] = L"RdHostService";
// 面板管道：服务本体（引擎所在）做服务端。引擎上收进服务后，面板协议
// （hello/startHost/stopHost/setQuality/setDefaultScreen 与 info/answer/
// state/secure/error/stats 事件）保持字节级不变，GUI 无需改动。
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\rdc-host-ctrl";
// helper 控制管道：服务（客户端）↔ helper（服务端），只跑采集启停/输入
// 注入/锁屏上报/屏幕信息。与面板管道分开：面板面向"服务"，控制面向
// "helper"，两者生命周期与会话切换行为完全不同。
constexpr wchar_t kHelperPipeName[] = L"\\\\.\\pipe\\rdc-host-helper-ctl";
constexpr DWORD kMaxMessage = 1u << 20;  // SDP 几 KB，留足余量

// ---------- 构造 ----------

// 字符串 -> 带引号的 JSON 字符串字面量。仅转义必需字符，非 ASCII 按原样
// UTF-8 直出（JSON 合法，免去 \u 往返）。
inline std::string JStr(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += "\"";
  return out;
}

// ---------- 解析（只支持扁平对象） ----------

// 取 key 对应的原始值文本（字符串含引号；数字/布尔原样；数组/对象为平衡
// 括号内的原文）。找不到返回空串。
inline std::string JsonRawValue(const std::string& json,
                                const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  while (p != std::string::npos) {
    // 确认是键名而非字符串值的一部分：其后第一个非空白必须是 ':'。
    size_t q = p + needle.size();
    while (q < json.size() && (json[q] == ' ' || json[q] == '\t')) {
      ++q;
    }
    if (q < json.size() && json[q] == ':') {
      ++q;
      while (q < json.size() && (json[q] == ' ' || json[q] == '\t')) {
        ++q;
      }
      if (q >= json.size()) {
        return {};
      }
      if (json[q] == '"') {
        size_t e = q + 1;
        while (e < json.size()) {
          if (json[e] == '\\') {
            e += 2;
            continue;
          }
          if (json[e] == '"') {
            break;
          }
          ++e;
        }
        return json.substr(q, e - q + 1);
      }
      if (json[q] == '[' || json[q] == '{') {
        const char open = json[q];
        const char close = open == '[' ? ']' : '}';
        int depth = 0;
        bool inStr = false;
        size_t e = q;
        for (; e < json.size(); ++e) {
          char c = json[e];
          if (inStr) {
            if (c == '\\') {
              ++e;
            } else if (c == '"') {
              inStr = false;
            }
            continue;
          }
          if (c == '"') {
            inStr = true;
          } else if (c == open) {
            ++depth;
          } else if (c == close) {
            if (--depth == 0) {
              return json.substr(q, e - q + 1);
            }
          }
        }
        return {};
      }
      // 数字/布尔/null：读到分隔符为止。
      size_t e = q;
      while (e < json.size() && json[e] != ',' && json[e] != '}' &&
             json[e] != ' ' && json[e] != '\n' && json[e] != '\r') {
        ++e;
      }
      return json.substr(q, e - q);
    }
    p = json.find(needle, p + needle.size());
  }
  return {};
}

// 字符串值去引号并还原转义。
inline std::string JsonGetString(const std::string& json,
                                 const std::string& key) {
  std::string raw = JsonRawValue(json, key);
  if (raw.size() < 2 || raw.front() != '"') {
    return {};
  }
  std::string out;
  for (size_t i = 1; i + 1 < raw.size(); ++i) {
    char c = raw[i];
    if (c != '\\') {
      out += c;
      continue;
    }
    char n = raw[++i];
    switch (n) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        if (i + 4 < raw.size()) {
          unsigned cp = strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
          i += 4;
          if (cp < 0x80) {
            out += static_cast<char>(cp);
          } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
          }
        }
        break;
      }
      default: out += n; break;  // \" \\ \/
    }
  }
  return out;
}

inline long JsonGetInt(const std::string& json, const std::string& key,
                       long def = 0) {
  std::string raw = JsonRawValue(json, key);
  if (raw.empty() || raw.front() == '"') {
    return def;
  }
  return strtol(raw.c_str(), nullptr, 10);
}

inline double JsonGetDouble(const std::string& json, const std::string& key,
                            double def = 0) {
  const std::string raw = JsonRawValue(json, key);
  if (raw.empty() || raw.front() == '"') {
    return def;
  }
  return strtod(raw.c_str(), nullptr);
}

// 平行数组：[v1,v2,...]（数字数组或字符串数组）。字符串数组返回去引号
// 且还原转义后的各元素。
inline bool JsonGetArrayInt(const std::string& json, const std::string& key,
                            std::vector<long long>* out) {
  out->clear();
  std::string raw = JsonRawValue(json, key);
  if (raw.size() < 2 || raw.front() != '[') {
    return false;
  }
  size_t i = 1;
  while (i < raw.size()) {
    char c = raw[i];
    if (c == ',' || c == ' ') {
      ++i;
      continue;
    }
    if (c == ']') {
      break;
    }
    size_t e = i;
    while (e < raw.size() && raw[e] != ',' && raw[e] != ']') {
      ++e;
    }
    out->push_back(strtoll(raw.substr(i, e - i).c_str(), nullptr, 10));
    i = e;
  }
  return true;
}

inline bool JsonGetArrayString(const std::string& json, const std::string& key,
                               std::vector<std::string>* out) {
  out->clear();
  std::string raw = JsonRawValue(json, key);
  if (raw.size() < 2 || raw.front() != '[') {
    return false;
  }
  size_t i = 1;
  while (i < raw.size() && raw[i] != ']') {
    if (raw[i] != '"') {
      ++i;
      continue;
    }
    size_t e = i + 1;
    while (e < raw.size()) {
      if (raw[e] == '\\') {
        e += 2;
        continue;
      }
      if (raw[e] == '"') {
        break;
      }
      ++e;
    }
    // 解码元素内转义（含 \r\n 等——SDP/标题会用到）。
    out->emplace_back();
    for (size_t j = i + 1; j < e; ++j) {
      char c = raw[j];
      if (c != '\\') {
        out->back() += c;
        continue;
      }
      char n = raw[++j];
      switch (n) {
        case 'n': out->back() += '\n'; break;
        case 'r': out->back() += '\r'; break;
        case 't': out->back() += '\t'; break;
        default: out->back() += n; break;
      }
    }
    i = e + 1;
  }
  return true;
}

// ---------- Base64（光标形态位图走 JSON 管道时的二进制承载） ----------

inline std::string Base64Encode(const uint8_t* data, size_t size) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kTable[(v >> 18) & 0x3f];
    out += kTable[(v >> 12) & 0x3f];
    out += kTable[(v >> 6) & 0x3f];
    out += kTable[v & 0x3f];
  }
  if (i < size) {
    const uint32_t v = data[i] << 16;
    out += kTable[(v >> 18) & 0x3f];
    out += kTable[(v >> 12) & 0x3f];
    out += (i + 1 < size) ? kTable[(((data[i + 1] << 8) | v) >> 6) & 0x3f]
                          : '=';
    out += '=';
  }
  return out;
}

inline bool Base64Decode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\r' || c == '\n') {
      continue;
    }
    const int v = val(c);
    if (v < 0) {
      out->clear();
      return false;
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
    }
  }
  return true;
}

// ---------- 帧收发（字节型管道；读取方负责粘包缓冲） ----------

inline void AppendFrame(std::string* buf, const std::string& payload) {
  uint32_t len = static_cast<uint32_t>(payload.size());
  buf->append(reinterpret_cast<const char*>(&len), 4);
  buf->append(payload);
}

// 追加管道上当前可读的所有字节到 buf。
inline bool DrainAvailable(HANDLE h, std::string* buf) {
  DWORD avail = 0;
  if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
    return false;  // 管道断开
  }
  while (avail > 0) {
    char tmp[4096];
    DWORD toRead = avail < sizeof(tmp) ? avail : sizeof(tmp);
    DWORD got = 0;
    if (!ReadFile(h, tmp, toRead, &got, nullptr) || got == 0) {
      return false;
    }
    buf->append(tmp, got);
    avail -= got;
  }
  return true;
}

// 从 buf 头部取一帧；不足一帧时返回 false（数据保留在 buf）。
inline bool PopFrame(std::string* buf, std::string* payload) {
  if (buf->size() < 4) {
    return false;
  }
  uint32_t len = 0;
  memcpy(&len, buf->data(), 4);
  if (len > kMaxMessage) {
    buf->clear();  // 流损坏，丢弃重来
    return false;
  }
  if (buf->size() < 4 + len) {
    return false;
  }
  payload->assign(buf->data() + 4, len);
  buf->erase(0, 4 + len);
  return true;
}

inline bool WriteFrame(HANDLE h, const std::string& payload) {
  std::string frame;
  AppendFrame(&frame, payload);
  DWORD written = 0;
  return WriteFile(h, frame.data(), static_cast<DWORD>(frame.size()),
                   &written, nullptr) &&
         written == frame.size();
}

}  // namespace rdpipe

#endif  // REMOTE_DESKTOP_SERVICE_PIPE_PROTO_H_
