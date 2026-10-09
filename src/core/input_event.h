// remote_desktop — input event model shared over the DataChannel.
#ifndef REMOTE_DESKTOP_CORE_INPUT_EVENT_H_
#define REMOTE_DESKTOP_CORE_INPUT_EVENT_H_

#include <cstdint>
#include <string>

#include <QJsonObject>
#include <QMetaType>

namespace rd {

struct InputEvent {
  enum class Type { MouseMove = 0, MouseButton, MouseWheel, Key };

  Type type = Type::MouseMove;
  // Normalised coordinates [0..1] relative to the remote frame.
  double nx = 0;
  double ny = 0;
  int button = 0;     // 1=left 2=right 3=middle
  bool pressed = false;
  int wheelDelta = 0; // +/-120 per notch
  uint32_t keyCode = 0;  // Windows VK code
  bool extended = false;
  std::string text;   // reserved

  QJsonObject toJson() const;
  static bool fromJson(const QJsonObject& o, InputEvent& out);
};

}  // namespace rd

Q_DECLARE_METATYPE(rd::InputEvent)

#endif
