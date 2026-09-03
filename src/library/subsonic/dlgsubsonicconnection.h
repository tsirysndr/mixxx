#pragma once

#include <QDialog>
#include <QFuture>
#include <QFutureWatcher>

#include "preferences/usersettings.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

/// Modal dialog for entering and testing the Subsonic/Navidrome server
/// connection. Host/username/TLS setting are persisted to the [Subsonic]
/// config group; the password is stored in the OS credential store via
/// QtKeychain (see subsoniccredentials.h), falling back to a plaintext
/// config value only when no keychain backend is available.
class DlgSubsonicConnection : public QDialog {
    Q_OBJECT
  public:
    explicit DlgSubsonicConnection(
            UserSettingsPointer pConfig, QWidget* pParent = nullptr);
    ~DlgSubsonicConnection() override;

  private slots:
    void slotTestConnection();
    void slotTestFinished();
    void slotAccept();

  private:
    UserSettingsPointer m_pConfig;
    QLineEdit* m_pHostEdit;
    QLineEdit* m_pUsernameEdit;
    QLineEdit* m_pPasswordEdit;
    QCheckBox* m_pIgnoreTlsErrors;
    QPushButton* m_pTestButton;
    QLabel* m_pStatusLabel;
    QFutureWatcher<QString> m_testWatcher;
    // The keychain is queried asynchronously: a synchronous read in the
    // constructor would block the dialog from appearing whenever macOS
    // decides to show a keychain authorization prompt.
    QFutureWatcher<QString> m_passwordWatcher;
};
