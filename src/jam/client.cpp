#include "jam/client.h"

#include "jam/codec.h"

#include <QDateTime>
#include <QWebSocket>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace Jam {

namespace {

constexpr auto kNormalClose = QWebSocketProtocol::CloseCodeNormal;

bool EndsTheJam(const ServerMessage& message) {
    if (std::holds_alternative<Ended>(message) || std::holds_alternative<Kicked>(message)) {
        return true;
    }
    const auto* rejected = std::get_if<Rejected>(&message);
    return rejected && rejected->reason == QLatin1String("update-required");
}

// The server refuses a hello whose version is not 1 to 32 of [0-9A-Za-z.+_-], and a client that
// keeps sending one is banned: anything else is dropped, and an empty version says so.
QString HelloVersion(const QString& version) {
    QString kept;
    for (const QChar character : version) {
        const bool allowed = (character.isLetterOrNumber() && character.unicode() < 0x80)
            || QStringLiteral(".+_-").contains(character);
        if (allowed && kept.size() < 32) {
            kept += character;
        }
    }
    return kept.isEmpty() ? QStringLiteral("unknown") : kept;
}

}  // namespace

Client::Client(ClientOptions clientOptions, QObject* parent)
    : QObject(parent)
    , options(std::move(clientOptions)) {
    if (!options.clock) {
        options.clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
    }
    options.appVersion = HelloVersion(options.appVersion);
    if (options.reconnectDelaysMs.empty()) {
        options.reconnectDelaysMs = {1'000};
    }
    retryTimer.setSingleShot(true);
    connect(&retryTimer, &QTimer::timeout, this, &Client::connectSocket);
    pingTimer.setInterval(options.pingIntervalMs);
    connect(&pingTimer, &QTimer::timeout, this, &Client::ping);
}

Client::~Client() {
    dropSocket();
}

qint64 Client::serverNow() const {
    return options.clock() + offsetMs;
}

void Client::start(const QUrl& url) {
    serverUrl = url;
    attempt = 0;
    retryTimer.stop();
    connectSocket();
}

void Client::stop() {
    retryTimer.stop();
    dropSocket();
    setStatus(Status::Stopped);
}

bool Client::send(const ClientMessage& message) {
    if (!socket || currentStatus != Status::Online) {
        return false;
    }
    const QByteArray text = Encode(message);
    const Decoded<ClientMessage> check = DecodeClient(text);
    if (!check.isValid()) {
        qWarning(
            "jam: not sending a %s that the server would refuse: %s",
            qUtf8Printable(TypeOf(message)), qUtf8Printable(check.problem)
        );
        return false;
    }
    socket->sendTextMessage(QString::fromUtf8(text));
    return true;
}

void Client::started(const QString& itemId, bool sendNow) {
    if ((!sendNow || !send(Started{itemId})) && !pendingStarted.contains(itemId)) {
        setOutbox(pendingStarted + QStringList{itemId});
    }
}

void Client::restoreOutbox(const QStringList& itemIds) {
    setOutbox(itemIds);
}

void Client::clearOutbox() {
    setOutbox({});
}

void Client::networkBack() {
    if (currentStatus != Status::Offline) {
        return;
    }
    retryTimer.stop();
    attempt = 0;
    connectSocket();
}

QUrl Client::SocketUrl(const QString& serverUrl) {
    QString base = serverUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    if (base.startsWith(QLatin1String("https://"))) {
        base = QStringLiteral("wss://") + base.mid(8);
    } else if (base.startsWith(QLatin1String("http://"))) {
        base = QStringLiteral("ws://") + base.mid(7);
    } else if (!base.startsWith(QLatin1String("ws://"))
               && !base.startsWith(QLatin1String("wss://"))) {
        base = QStringLiteral("wss://") + base;
    }
    return QUrl(base + QStringLiteral("/ws"));
}

void Client::connectSocket() {
    dropSocket();
    setStatus(Status::Connecting);
    auto* created = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    socket = created;
    connect(created, &QWebSocket::connected, this, [this, created] {
        if (created == socket) {
            created->sendTextMessage(
                QString::fromUtf8(Encode(Hello{kProtocol, App::Desktop, options.appVersion}))
            );
        }
    });
    connect(created, &QWebSocket::textMessageReceived, this, [this, created](const QString& text) {
        if (created == socket) {
            receive(text);
        }
    });
    connect(created, &QWebSocket::pong, this, [this, created] {
        if (created == socket) {
            awaitingPong = false;
        }
    });
    connect(
        created, &QWebSocket::stateChanged, this,
        [this, created](QAbstractSocket::SocketState state) {
            if (state == QAbstractSocket::UnconnectedState) {
                socketClosed(created);
            }
        }
    );
    awaitingPong = false;
    pingTimer.start();
    created->open(serverUrl);
}

void Client::dropSocket() {
    pingTimer.stop();
    if (!socket) {
        return;
    }
    QWebSocket* old = std::exchange(socket, nullptr);
    old->disconnect(this);
    old->close(kNormalClose);
    old->deleteLater();
}

void Client::socketClosed(QWebSocket* closed) {
    if (closed != socket) {
        return;
    }
    dropSocket();
    if (currentStatus == Status::Stopped) {
        return;
    }
    setStatus(Status::Offline);
    const auto delays = options.reconnectDelaysMs;
    const int delayMs = delays[std::min<size_t>(static_cast<size_t>(attempt), delays.size() - 1)];
    ++attempt;
    retryTimer.start(delayMs);
}

void Client::receive(const QString& text) {
    const Decoded<ServerMessage> decoded = DecodeServer(text.toUtf8());
    if (!decoded.message) {
        if (!decoded.unknownType) {
            qWarning("jam: ignored an invalid message: %s", qUtf8Printable(decoded.problem));
        }
        return;
    }
    const ServerMessage& message = *decoded.message;
    if (const auto* welcome = std::get_if<Welcome>(&message)) {
        offsetMs = welcome->serverTime - options.clock();
        attempt = 0;
        setStatus(Status::Online);
        Q_EMIT welcomed();
        return;
    }
    if (const auto* state = std::get_if<State>(&message)) {
        offsetMs = state->serverTime - options.clock();
    }
    const bool ends = EndsTheJam(message);
    Q_EMIT messageReceived(message);
    if (ends) {
        stop();
    }
}

void Client::ping() {
    if (!socket) {
        return;
    }
    if (awaitingPong) {
        qInfo("jam: no pong in %d ms, the connection is dead", options.pingIntervalMs);
        socket->abort();
        return;
    }
    awaitingPong = true;
    socket->ping();
}

void Client::setStatus(Status status) {
    if (status == currentStatus) {
        return;
    }
    currentStatus = status;
    Q_EMIT statusChanged(status);
}

void Client::setOutbox(QStringList itemIds) {
    if (itemIds == pendingStarted) {
        return;
    }
    pendingStarted = std::move(itemIds);
    Q_EMIT outboxChanged(pendingStarted);
}

}  // namespace Jam
