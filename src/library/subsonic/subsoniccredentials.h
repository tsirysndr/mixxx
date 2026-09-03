#pragma once

#include <QString>

/// Synchronous helpers around QtKeychain for storing the Subsonic server
/// password in the OS credential store (macOS Keychain, Windows
/// Credential Store, Secret Service/KWallet on Linux) instead of
/// mixxx.cfg. Each call spins a local event loop until the underlying
/// async QtKeychain job finishes; safe to call from any thread.
namespace subsoniccredentials {

/// Reads the password for the given server/user. Returns an empty string
/// if there is none. `pOk` (optional) reports whether the keychain
/// backend worked (an entry-not-found still counts as failure=false only
/// on real backend errors).
QString read(const QString& host, const QString& username, bool* pOk = nullptr);

/// Stores the password. Returns false if the keychain backend failed
/// (callers should then fall back to config storage).
bool write(const QString& host, const QString& username, const QString& password);

/// Removes a stored password (best effort).
void remove(const QString& host, const QString& username);

} // namespace subsoniccredentials
