// remote_desktop — 输入事件模型。rd::InputEvent 现在是引擎 rdc::InputEvent
// 的别名（字段与旧结构完全一致）；跨端序列化由引擎的二进制控制协议负责
// （engine/src/control_protocol.h），QJson 版本已随 Qt 耦合层一并移除。
#ifndef REMOTE_DESKTOP_CORE_INPUT_EVENT_H_
#define REMOTE_DESKTOP_CORE_INPUT_EVENT_H_

#include "rdc/engine.h"

namespace rd {
using InputEvent = rdc::InputEvent;
}

#endif  // REMOTE_DESKTOP_CORE_INPUT_EVENT_H_
