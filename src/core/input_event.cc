#include "core/input_event.h"

#include <QJsonObject>
#include <QJsonValue>

namespace rd {

QJsonObject InputEvent::toJson() const {
  QJsonObject o;
  o["t"] = static_cast<int>(type);
  o["x"] = nx;
  o["y"] = ny;
  o["b"] = button;
  o["p"] = pressed;
  o["w"] = wheelDelta;
  o["k"] = static_cast<double>(keyCode);
  o["e"] = extended;
  return o;
}

bool InputEvent::fromJson(const QJsonObject& o, InputEvent& out) {
  if (!o.contains("t")) {
    return false;
  }
  const int t = o["t"].toInt(-1);
  if (t < 0 || t > 3) {
    return false;
  }
  out.type = static_cast<Type>(t);
  out.nx = o["x"].toDouble();
  out.ny = o["y"].toDouble();
  out.button = o["b"].toInt();
  out.pressed = o["p"].toBool();
  out.wheelDelta = o["w"].toInt();
  out.keyCode = static_cast<uint32_t>(o["k"].toDouble());
  out.extended = o["e"].toBool();
  return true;
}

}  // namespace rd
