// Local control socket ($XDG_RUNTIME_DIR/roadboard.sock).
// Protocol: one request per line.
//   <script>          -> "OK <n>" or "ERR <message>"
//   ?<parameter>      -> value or "ERR unknown"
// Also used to keep a single running instance ("roadboard --cmd ..." talks to it).
#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <functional>

namespace road {

class RemoteServer : public QObject {
    Q_OBJECT
public:
    explicit RemoteServer(QObject* parent = nullptr);
    bool listen();
    static QString socketPath();
    // optional getter for values that are not parameters (levels...): returns false if unknown
    static std::function<bool(const QString& key, QString* out)> extraGetter;
    // client side helper: send one request, return the reply ("" on failure)
    static QString request(const QString& line, int timeoutMs = 1500, bool* connected = nullptr);

private:
    void onConnection();
    QLocalServer server_;
};

} // namespace road
