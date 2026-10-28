// remote_desktop — HostWindow / ViewerWindow 两个独立程序的公共窗口基类。
// 持有 RemoteDesktopSession 并接线公共回调（失败弹窗、断开提示、每秒管道
// 诊断）；角色相关的页面与行为留给子类实现。
#ifndef REMOTE_DESKTOP_UI_SESSION_WINDOW_H_
#define REMOTE_DESKTOP_UI_SESSION_WINDOW_H_

#include <QMainWindow>
#include <QString>

#include "session/remote_desktop_session.h"

class QLabel;
class QTimer;

namespace rd {

class SessionWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit SessionWindow(QWidget* parent = nullptr);

 protected:
  // 状态栏消息 + 页面常驻状态行（子类构造时把自己的标签赋给 stateLabel_）。
  void setStatus(const QString& text);

  RemoteDesktopSession* session_ = nullptr;
  QLabel* stateLabel_ = nullptr;  // 子类页面的状态行

  // 会话事件钩子：连接建立/断开时除状态提示外的角色行为
  // （Viewer 关闭全屏视频窗口；Host 无额外动作）。
  virtual void onSessionConnected();
  virtual void onSessionDisconnected();

  // 引擎报错（OnError/Failed）：默认弹窗；子类可覆盖（如自动重连期间
  // 只改状态栏不弹模态框，避免重试被弹窗阻断）。
  virtual void onSessionFailed(const QString& reason);

  // updateDiag() 每秒生成管道诊断文本后的额外出口
  // （Viewer 转发给全屏窗口与内嵌预览；Host 无视频面，无需出口）。
  virtual void applyDiagText(const QString& text) { (void)text; }

 private:
  void updateDiag();

  QTimer* diagTimer_ = nullptr;
};

}  // namespace rd

#endif
