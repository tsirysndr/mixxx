#include "library/subsonic/dlgsubsonicconnection.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "library/subsonic/subsoniccredentials.h"
#include "moc_dlgsubsonicconnection.cpp"
#include "rust/cxx.h"
#include "subsonic-bridge/subsonic_bridge.h"

namespace {

const QString kConfigGroup = QStringLiteral("[Subsonic]");
const QString kSuccessPrefix = QStringLiteral("OK\x1f");

} // anonymous namespace

DlgSubsonicConnection::DlgSubsonicConnection(
        UserSettingsPointer pConfig, QWidget* pParent)
        : QDialog(pParent),
          m_pConfig(pConfig) {
    setWindowTitle(tr("Subsonic / Navidrome Server"));
    setModal(true);
    // Wide enough to show a full server URL without scrolling.
    setMinimumWidth(480);

    m_pHostEdit = new QLineEdit(this);
    m_pHostEdit->setMinimumWidth(320);
    m_pHostEdit->setPlaceholderText(
            QStringLiteral("https://navidrome.rocksky.app"));
    m_pHostEdit->setText(m_pConfig->getValue(
            ConfigKey(kConfigGroup, QStringLiteral("Host")), QString()));

    m_pUsernameEdit = new QLineEdit(this);
    m_pUsernameEdit->setText(m_pConfig->getValue(
            ConfigKey(kConfigGroup, QStringLiteral("Username")), QString()));

    m_pPasswordEdit = new QLineEdit(this);
    m_pPasswordEdit->setEchoMode(QLineEdit::Password);
    // Prefer the keychain; fall back to a legacy plaintext config value.
    QString password = subsoniccredentials::read(
            m_pHostEdit->text().trimmed(), m_pUsernameEdit->text().trimmed());
    if (password.isEmpty()) {
        password = m_pConfig->getValue(
                ConfigKey(kConfigGroup, QStringLiteral("Password")), QString());
    }
    m_pPasswordEdit->setText(password);

    m_pIgnoreTlsErrors = new QCheckBox(tr("Ignore TLS certificate errors (insecure)"), this);
    m_pIgnoreTlsErrors->setChecked(!m_pConfig->getValue(
            ConfigKey(kConfigGroup, QStringLiteral("VerifyTls")), true));

    auto* pFormLayout = new QFormLayout();
    pFormLayout->addRow(tr("Server URL"), m_pHostEdit);
    pFormLayout->addRow(tr("Username"), m_pUsernameEdit);
    pFormLayout->addRow(tr("Password"), m_pPasswordEdit);
    pFormLayout->addRow(QString(), m_pIgnoreTlsErrors);

    m_pTestButton = new QPushButton(tr("Test Connection"), this);
    m_pStatusLabel = new QLabel(this);
    m_pStatusLabel->setWordWrap(true);

    auto* pButtonBox = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    pButtonBox->addButton(m_pTestButton, QDialogButtonBox::ActionRole);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addLayout(pFormLayout);
    pLayout->addWidget(m_pStatusLabel);
    pLayout->addWidget(pButtonBox);

    connect(m_pTestButton,
            &QPushButton::clicked,
            this,
            &DlgSubsonicConnection::slotTestConnection);
    connect(pButtonBox, &QDialogButtonBox::accepted, this, &DlgSubsonicConnection::slotAccept);
    connect(pButtonBox, &QDialogButtonBox::rejected, this, &DlgSubsonicConnection::reject);
    connect(&m_testWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &DlgSubsonicConnection::slotTestFinished);
}

DlgSubsonicConnection::~DlgSubsonicConnection() {
    m_testWatcher.waitForFinished();
}

void DlgSubsonicConnection::slotTestConnection() {
    const QString host = m_pHostEdit->text().trimmed();
    if (host.isEmpty()) {
        m_pStatusLabel->setText(tr("Please enter a server URL."));
        return;
    }
    const QString username = m_pUsernameEdit->text().trimmed();
    const QString password = m_pPasswordEdit->text();
    const bool verifyTls = !m_pIgnoreTlsErrors->isChecked();

    m_pTestButton->setEnabled(false);
    m_pStatusLabel->setText(tr("Connecting..."));

    m_testWatcher.setFuture(QtConcurrent::run(
            [host, username, password, verifyTls]() -> QString {
                try {
                    subsonic::ConnectionConfig config{
                            rust::String(host.toStdString()),
                            rust::String(username.toStdString()),
                            rust::String(password.toStdString()),
                            verifyTls,
                    };
                    const rust::Box<subsonic::Client> client =
                            subsonic::new_client(config);
                    const rust::String version = subsonic::ping(*client);
                    return kSuccessPrefix +
                            QString::fromUtf8(version.data(),
                                    static_cast<int>(version.size()));
                } catch (const rust::Error& e) {
                    return QString::fromUtf8(e.what());
                }
            }));
}

void DlgSubsonicConnection::slotTestFinished() {
    m_pTestButton->setEnabled(true);
    const QString result = m_testWatcher.result();
    if (result.startsWith(kSuccessPrefix)) {
        m_pStatusLabel->setText(
                tr("Connected. Server API version: %1")
                        .arg(result.mid(kSuccessPrefix.size())));
    } else {
        m_pStatusLabel->setText(tr("Connection failed: %1").arg(result));
    }
}

void DlgSubsonicConnection::slotAccept() {
    const QString host = m_pHostEdit->text().trimmed();
    if (host.isEmpty()) {
        m_pStatusLabel->setText(tr("Please enter a server URL."));
        return;
    }
    const QString username = m_pUsernameEdit->text().trimmed();
    m_pConfig->setValue(
            ConfigKey(kConfigGroup, QStringLiteral("Host")), host);
    m_pConfig->setValue(
            ConfigKey(kConfigGroup, QStringLiteral("Username")), username);
    m_pConfig->setValue(ConfigKey(kConfigGroup, QStringLiteral("VerifyTls")),
            !m_pIgnoreTlsErrors->isChecked());
    if (subsoniccredentials::write(host, username, m_pPasswordEdit->text())) {
        // Drop any legacy plaintext password from mixxx.cfg.
        m_pConfig->setValue(
                ConfigKey(kConfigGroup, QStringLiteral("Password")), QString());
    } else {
        // No usable keychain backend; keep the previous plaintext behavior
        // so the feature still works.
        m_pConfig->setValue(ConfigKey(kConfigGroup, QStringLiteral("Password")),
                m_pPasswordEdit->text());
    }
    accept();
}
