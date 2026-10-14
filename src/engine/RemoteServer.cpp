#include "engine/RemoteServer.h"

#include <QDir>
#include <QLocalSocket>
#include <QStandardPaths>

#include "core/RemoteScript.h"

namespace road {

std::function<bool(const QString&, QString*)> RemoteServer::extraGetter;

RemoteServer::RemoteServer(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, &RemoteServer::onConnection);
}

QString RemoteServer::socketPath() {
    const QString custom = qEnvironmentVariable("ROADBOARD_SOCKET");
    if (!custom.isEmpty()) return custom;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    return dir + "/roadboard.sock";
}

bool RemoteServer::listen() {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (server_.listen(socketPath())) return true;
    // stale socket from a crashed instance?
    bool alive = false;
    request("?strip[0].gain", 300, &alive);
    if (alive) return false;
    QLocalServer::removeServer(socketPath());
    return server_.listen(socketPath());
}

void RemoteServer::onConnection() {
    while (QLocalSocket* s = server_.nextPendingConnection()) {
        connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QLocalSocket::readyRead, s, [s] {
            while (s->canReadLine()) {
                const QString line = QString::fromUtf8(s->readLine()).trimmed();
                if (line.isEmpty()) continue;
                QString reply;
                if (line.startsWith('?')) {
                    std::string out;
                    QString extra;
                    if (extraGetter && extraGetter(line.mid(1), &extra)) reply = extra;
                    else reply = remote().get(line.mid(1).toStdString(), &out) ? QString::fromStdString(out) : QStringLiteral("ERR unknown");
                } else {
                    std::string err;
                    const int n = remote().execute(line.toStdString(), &err);
                    reply = err.empty() ? QStringLiteral("OK %1").arg(n)
                                        : QStringLiteral("ERR ") + QString::fromStdString(err).trimmed().replace('\n', "; ");
                }
                s->write(reply.toUtf8() + "\n");
                s->flush();
            }
        });
    }
}

QString RemoteServer::request(const QString& line, int timeoutMs, bool* connected) {
    QLocalSocket s;
    s.connectToServer(socketPath());
    if (!s.waitForConnected(timeoutMs)) { if (connected) *connected = false; return {}; }
    if (connected) *connected = true;
    s.write(line.toUtf8() + "\n");
    s.flush();
    QByteArray buf;
    while (!buf.contains('\n') && s.waitForReadyRead(timeoutMs)) buf += s.readAll();
    return QString::fromUtf8(buf).trimmed();
}

} // namespace road
