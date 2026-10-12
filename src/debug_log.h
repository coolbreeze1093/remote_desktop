// remote_desktop — 常开诊断日志（只用于低频调用点）。
// rdDebug 现在是引擎 rdcDebug 的别名（engine/include/rdc/log.h）：同一份
// 实现、同一个 exe 旁 rd_debug.log，GUI 与引擎与 CLI 工具共写一份日志。
#ifndef REMOTE_DESKTOP_DEBUG_LOG_H_
#define REMOTE_DESKTOP_DEBUG_LOG_H_

#include "rdc/log.h"

#define rdDebug rdcDebug

#endif  // REMOTE_DESKTOP_DEBUG_LOG_H_
