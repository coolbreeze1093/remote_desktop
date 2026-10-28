// rd_host_service — rd_host.ini 配置读取（服务本体与 helper 共用，无 Qt）。
//
// 配置文件固定在 exe 同目录（服务与 helper 是同一个 exe，路径天然一致），
// 纯窄字节解析（配置值限定 ASCII；文件允许 UTF-8 BOM 与 #/; 注释行）：
//   port=48010              信令监听端口（1-65535；缺失/非法回退默认并落日志）
//   stun=stun.miwifi.com:3478
//                           ICE STUN 服务器，与 rdc::Config::iceServers 同格式
//                           （可带或不带 stun: 前缀）。可逗号分隔多个，或写
//                           多行 stun=（累加）。
//   stun=off （或 stun= 留空）
//                           禁用 STUN（调用方据此设 RD_NO_STUN=1，见 engine.cc）。
// 任何键都未提供时，取值与历史硬编码完全一致（48010 / stun.miwifi.com:3478），
// 即不放假配置文件也行为不变。
#ifndef REMOTE_DESKTOP_SERVICE_HOST_CONFIG_H_
#define REMOTE_DESKTOP_SERVICE_HOST_CONFIG_H_

#include <string>
#include <vector>

namespace rdcfg {

constexpr unsigned short kDefaultSignalPort = 48010;

struct HostConfig {
  unsigned short port = kDefaultSignalPort;
  // 未写 stun= 时保持历史默认；写了则完全以配置为准（含 off → 空）。
  std::vector<std::string> stunServers = {"stun.miwifi.com:3478"};
  bool stunOff = false;      // stun=off / stun= 留空
  bool fileExists = false;   // 诊断：ini 是否存在
};

// 读 exe 同目录 rd_host.ini。永不失败：文件缺失、字段非法均回退默认值，
// 一切异常以 rdDebug("config", ...) 落日志（rd_debug.log）。
HostConfig LoadHostConfig();

}  // namespace rdcfg

#endif  // REMOTE_DESKTOP_SERVICE_HOST_CONFIG_H_
