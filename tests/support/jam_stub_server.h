#pragma once

#include "jam/protocol.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QWebSocketServer>

#include <memory>
#include <vector>

class QWebSocket;

namespace Tests {

// The server's end of the jam protocol for a client under test: accepts connections on localhost
// and keeps what arrives on each.
class JamStubServer : public QObject {
public:
    JamStubServer();
    ~JamStubServer() override;

    QUrl url() const;
    // The same server as a jam server address (http://), as the settings hold it.
    QString serverUrl() const;
    int count() const { return static_cast<int>(connections.size()); }
    QWebSocket& last() { return *connections.back().socket; }
    // What arrived on the last connection.
    QStringList received() const;
    std::vector<Jam::ClientMessage> messages() const;

    void send(const QByteArray& text);
    void welcome(qint64 serverTime = 0);

private:
    struct Connection {
        std::unique_ptr<QWebSocket> socket;
        QStringList received;
    };

    QWebSocketServer server;
    std::vector<Connection> connections;
};

}  // namespace Tests
