#include "library/subsonic/subsoniccredentials.h"

#include <QEventLoop>
#include <QtDebug>
// qtkeychain is vendored via FetchContent; its headers live at the crate
// source root (see the SUBSONIC block in CMakeLists.txt).
#include <keychain.h>

namespace {

const QString kService = QStringLiteral("org.mixxx.subsonic");

QString keyFor(const QString& host, const QString& username) {
    return username + QChar('@') + host;
}

/// Runs a QtKeychain job to completion on the current thread.
bool runJob(QKeychain::Job* pJob) {
    QEventLoop loop;
    QObject::connect(pJob, &QKeychain::Job::finished, &loop, [&loop](QKeychain::Job*) {
        loop.quit();
    });
    pJob->start();
    loop.exec();
    return pJob->error() == QKeychain::NoError;
}

} // anonymous namespace

namespace subsoniccredentials {

QString read(const QString& host, const QString& username, bool* pOk) {
    QKeychain::ReadPasswordJob job(kService);
    job.setAutoDelete(false);
    job.setKey(keyFor(host, username));
    const bool ok = runJob(&job);
    if (pOk) {
        // A missing entry is a valid "no password stored" result, not a
        // broken keychain backend.
        *pOk = ok || job.error() == QKeychain::EntryNotFound;
    }
    if (!ok && job.error() != QKeychain::EntryNotFound) {
        qWarning() << "Subsonic: keychain read failed:" << job.errorString();
    }
    return ok ? job.textData() : QString();
}

bool write(const QString& host, const QString& username, const QString& password) {
    QKeychain::WritePasswordJob job(kService);
    job.setAutoDelete(false);
    job.setKey(keyFor(host, username));
    job.setTextData(password);
    const bool ok = runJob(&job);
    if (!ok) {
        qWarning() << "Subsonic: keychain write failed:" << job.errorString();
    }
    return ok;
}

void remove(const QString& host, const QString& username) {
    QKeychain::DeletePasswordJob job(kService);
    job.setAutoDelete(false);
    job.setKey(keyFor(host, username));
    if (!runJob(&job) && job.error() != QKeychain::EntryNotFound) {
        qWarning() << "Subsonic: keychain delete failed:" << job.errorString();
    }
}

} // namespace subsoniccredentials
