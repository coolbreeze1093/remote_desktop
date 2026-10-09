#include "net/control_channel.h"

#include <QJsonDocument>
#include <QMetaObject>

#include <atomic>

#include "debug_log.h"

namespace rd {

ControlChannel::ControlChannel(QObject* parent)
    : QObject(parent), alive_(std::make_shared<std::atomic_bool>(true)) {}

ControlChannel::~ControlChannel() {
  alive_->store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mtx_);
  dc_.reset();
}

void ControlChannel::createOn(rtc::PeerConnection& pc) {
  auto dc = pc.createDataChannel("control");
  {
    std::lock_guard<std::mutex> lock(mtx_);
    dc_ = dc;
  }
  wire(*dc);
}

void ControlChannel::acceptOn(rtc::PeerConnection& pc) {
  const auto alive = alive_;
  pc.onDataChannel([this, alive](std::shared_ptr<rtc::DataChannel> dc) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mtx_);
      dc_ = dc;
    }
    wire(*dc);
  });
}

void ControlChannel::wire(rtc::DataChannel& dc) {
  auto* self = this;
  const auto alive = alive_;
  dc.onOpen([self, alive]() {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    QMetaObject::invokeMethod(self, "opened", Qt::QueuedConnection);
  });
  dc.onClosed([self, alive]() {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    QMetaObject::invokeMethod(self, "closed", Qt::QueuedConnection);
  });
  dc.onError([self, alive](std::string err) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    QMetaObject::invokeMethod(self, "errorText", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(err)));
  });
  dc.onMessage([self, alive](rtc::message_variant msg) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    if (!std::holds_alternative<rtc::binary>(msg)) {
      const auto& text = std::get<std::string>(msg);
      const QJsonDocument doc = QJsonDocument::fromJson(
          QByteArray::fromRawData(text.data(), static_cast<int>(text.size())));
      if (!doc.isObject()) {
        return;
      }
      const QJsonObject obj = doc.object();
      const QString type = obj["type"].toString();
      if (type == QStringLiteral("input")) {
        InputEvent ev;
        if (InputEvent::fromJson(obj["event"].toObject(), ev)) {
          QMetaObject::invokeMethod(self, "inputReceived",
                                    Qt::QueuedConnection,
                                    Q_ARG(rd::InputEvent, ev));
        }
      } else if (type == QStringLiteral("meta")) {
        const int w = obj["w"].toInt();
        const int h = obj["h"].toInt();
        QMetaObject::invokeMethod(self, "frameMetaReceived",
                                  Qt::QueuedConnection, Q_ARG(int, w),
                                  Q_ARG(int, h));
      } else if (type == QStringLiteral("stats")) {
        QMetaObject::invokeMethod(
            self, "videoStatsReceived", Qt::QueuedConnection,
            Q_ARG(int, obj["recv"].toInt()), Q_ARG(int, obj["dec"].toInt()),
            Q_ARG(int, obj["fail"].toInt()));
      } else if (type == QStringLiteral("quality")) {
        QMetaObject::invokeMethod(self, "qualityModeSelected",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, obj["mode"].toInt()));
      }
    }
  });
}

void ControlChannel::send(const QJsonObject& obj) {
  std::shared_ptr<rtc::DataChannel> dc;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    dc = dc_;
  }
  {
    static std::atomic<int> dbg{0};
    if (dbg.fetch_add(1) < 10) {
      rdDebug("ctrl", "send dc=%s open=%s", dc ? "yes" : "no",
              (dc && dc->isOpen()) ? "yes" : "no");
    }
  }
  if (!dc || !dc->isOpen()) {
    return;
  }
  const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  const std::string text(bytes.constData(), static_cast<size_t>(bytes.size()));
  try {
    dc->send(text);
  } catch (const std::exception&) {
    // Channel closed between isOpen() and send(); drop the message.
  }
}

void ControlChannel::sendInput(const InputEvent& ev) {
  QJsonObject obj;
  obj["type"] = QStringLiteral("input");
  obj["event"] = ev.toJson();
  send(obj);
}

void ControlChannel::sendFrameMeta(int width, int height) {
  QJsonObject obj;
  obj["type"] = QStringLiteral("meta");
  obj["w"] = width;
  obj["h"] = height;
  send(obj);
}

void ControlChannel::sendVideoStats(int received, int decoded,
                                    int decodeFailed) {
  QJsonObject obj;
  obj["type"] = QStringLiteral("stats");
  obj["recv"] = received;
  obj["dec"] = decoded;
  obj["fail"] = decodeFailed;
  send(obj);
}

void ControlChannel::sendQualityMode(int mode) {
  QJsonObject obj;
  obj["type"] = QStringLiteral("quality");
  obj["mode"] = mode;
  send(obj);
}

bool ControlChannel::isOpen() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return dc_ && dc_->isOpen();
}

}  // namespace rd
