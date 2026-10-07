#include "ui/jam_window.h"

#include "jam/host_session.h"
#include "skins/skin.h"
#include "yandex/library.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPainter>
#include <QPointer>
#include <QWheelEvent>
#include <qrcodegen.hpp>

#include <algorithm>
#include <utility>

namespace Ui {

namespace {

constexpr int kPadding = 4;
constexpr int kGap = 3;
constexpr int kMaxQrSide = 120;
constexpr int kQuietZone = 4;
constexpr int kEndConfirmMs = 3000;
constexpr int kMaxSearchLength = 100;

QColor Mix(const QColor& a, const QColor& b) {
    return QColor((a.red() + b.red()) / 2, (a.green() + b.green()) / 2, (a.blue() + b.blue()) / 2);
}

QString KindText(Jam::ParticipantKind kind) {
    switch (kind) {
        case Jam::ParticipantKind::Host:
            return QCoreApplication::translate("Ui::JamWindow", "host");
        case Jam::ParticipantKind::Web:
            return QCoreApplication::translate("Ui::JamWindow", "browser");
        case Jam::ParticipantKind::Qiyaa:
            return QCoreApplication::translate("Ui::JamWindow", "app");
    }
    return {};
}

}  // namespace

// Where the page is drawn and what it collects: a cursor down the content area, the colours of
// the skin's playlist, and the controls met on the way.
class JamWindow::Canvas {
public:
    Canvas(QPainter* target, const QRect& area, const QFont& font, const Skins::Skin& skin)
        : painter(target)
        , regular(font)
        , strong(font)
        , metrics(font)
        , left(area.x() + kPadding)
        , right(area.right() + 1 - kPadding)
        , bottom(area.bottom() + 1 - kPadding)
        , y(area.y() + kPadding) {
        strong.setBold(true);
        const Skins::Skin::PlaylistStyle& style = skin.playlistStyle();
        normal = style.normal;
        current = style.current;
        background = style.normalBackground;
        selected = style.selectedBackground;
        dim = Mix(style.normal, style.normalBackground);
        lineHeight = metrics.height() + 1;
    }

    int width() const { return right - left; }
    int textWidth(const QString& text) const { return metrics.horizontalAdvance(text); }
    int buttonHeight() const { return lineHeight + 3; }
    int buttonWidth(const QString& label) const { return textWidth(label) + 8; }

    void text(
        const QRect& rect,
        const QString& content,
        const QColor& color,
        bool bold = false,
        int flags = Qt::AlignLeft | Qt::AlignVCenter
    ) {
        if (!painter) {
            return;
        }
        painter->setFont(bold ? strong : regular);
        painter->setPen(color);
        const QFontMetrics fontMetrics(bold ? strong : regular);
        painter->drawText(
            rect, flags, fontMetrics.elidedText(content, Qt::ElideRight, rect.width())
        );
    }

    // One line across the width; moves down.
    void line(const QString& content, const QColor& color, bool bold = false) {
        text(QRect(left, y, width(), lineHeight), content, color, bold);
        y += lineHeight;
    }

    // Wrapped text, at most `maxLines` lines (0: all); moves down.
    void paragraph(
        const QString& content,
        const QColor& color,
        int maxLines = 0,
        int flags = Qt::TextWordWrap,
        int from = -1,
        int to = -1
    ) {
        const int x = from < 0 ? left : from;
        const int w = (to < 0 ? right : to) - x;
        const QRect bounds = metrics.boundingRect(
            QRect(x, y, w, 10'000), Qt::AlignLeft | Qt::AlignTop | flags, content
        );
        int height = bounds.height();
        if (maxLines > 0) {
            height = std::min(height, maxLines * metrics.lineSpacing());
        }
        if (painter) {
            painter->save();
            painter->setClipRect(QRect(x, y, w, height), Qt::IntersectClip);
            painter->setFont(regular);
            painter->setPen(color);
            painter->drawText(
                QRect(x, y, w, 10'000), Qt::AlignLeft | Qt::AlignTop | flags, content
            );
            painter->restore();
        }
        y += height;
    }

    QRect button(
        int x,
        int top,
        const QString& label,
        bool enabled,
        std::function<void()> action,
        bool on = false
    ) {
        const QRect rect(x, top, buttonWidth(label), buttonHeight());
        if (painter) {
            if (on) {
                painter->fillRect(rect, selected);
            }
            painter->setPen(enabled ? normal : dim);
            painter->drawRect(rect.adjusted(0, 0, -1, -1));
            // On the selection colour, like a selected row of the playlist.
            text(rect, label, enabled ? normal : dim, false, Qt::AlignCenter);
        }
        controls << Control{rect, label, enabled, std::move(action)};
        return rect;
    }

    // A small square light: on for online.
    void light(int x, int centerY, bool on) {
        if (!painter) {
            return;
        }
        const QRect rect(x, centerY - 2, 4, 4);
        if (on) {
            painter->fillRect(rect, current);
        } else {
            painter->setPen(dim);
            painter->drawRect(rect.adjusted(0, 0, -1, -1));
        }
    }

    QPainter* painter;
    QFont regular;
    QFont strong;
    QFontMetrics metrics;
    QColor normal, current, background, selected, dim;
    int lineHeight = 0;
    int left, right, bottom, y;
    QList<Control> controls;
};

JamWindow::JamWindow(
    Jam::HostSession* host,
    Yandex::Library* library,
    const Skins::Skin* skin,
    QWidget* parent
)
    : GenWindow(skin, QStringLiteral("JAM"), parent)
    , jamHost(host)
    , yandexLibrary(library) {
    retranslate();
    setFocusPolicy(Qt::StrongFocus);
    endTimer.setSingleShot(true);
    endTimer.setInterval(kEndConfirmMs);
    connect(&endTimer, &QTimer::timeout, this, [this] {
        endArmed = false;
        update();
    });
    connect(jamHost, &Jam::HostSession::changed, this, [this, last = jamHost->phase()]() mutable {
        const Jam::HostPhase now = jamHost->phase();
        if (now != last) {
            currentPage = Page::Jam;
            listScroll = 0;
            endArmed = false;
            startProblem.clear();
            if (focus == Field::Name && now != Jam::HostPhase::None) {
                focus = Field::None;
            }
            last = now;
        }
        update();
    });
}

void JamWindow::setHostName(const QString& name) {
    if (!nameEdited) {
        nameText = name.left(Jam::kMaxHostNameLength);
        update();
    }
}

void JamWindow::setServerName(const QString& server) {
    serverName = server;
    update();
}

void JamWindow::setTextFont(const std::optional<QFont>& font) {
    fontOverride = font;
    update();
}

void JamWindow::showPage(Page page) {
    if (page == currentPage) {
        return;
    }
    currentPage = page;
    listScroll = 0;
    focus = page == Page::Search ? Field::Search : Field::None;
    update();
}

QFont JamWindow::textFont() const {
    if (fontOverride) {
        return *fontOverride;
    }
    QFont font(skin().playlistStyle().font);
    font.setPixelSize(9);
    return font;
}

QList<JamWindow::Control> JamWindow::controls() {
    return render(nullptr);
}

void JamWindow::paintContent(QPainter& painter, const QRect& area) {
    painter.fillRect(area, skin().playlistStyle().normalBackground);
    render(&painter);
}

QList<JamWindow::Control> JamWindow::render(QPainter* painter) {
    Canvas canvas(painter, contentRect(), textFont(), skin());
    switch (jamHost->phase()) {
        case Jam::HostPhase::None: renderStart(canvas); break;
        case Jam::HostPhase::Creating: renderCreating(canvas); break;
        case Jam::HostPhase::Active:
            renderTabs(canvas);
            if (currentPage == Page::Jam) {
                renderJam(canvas);
            } else {
                renderSearch(canvas);
            }
            break;
    }
    return canvas.controls;
}

void JamWindow::renderStart(Canvas& canvas) {
    canvas.line(tr("New jam"), canvas.current, true);
    canvas.paragraph(
        tr("Friends open the link or the QR code on their phones and add tracks to your queue. "
           "You play, they search through your account."),
        canvas.normal
    );
    canvas.y += kGap * 2;
    canvas.line(tr("Your name"), canvas.normal);
    const QRect nameRect(canvas.left, canvas.y, canvas.width(), canvas.buttonHeight());
    field(canvas, nameRect, Field::Name, tr("How the guests will see you"));
    canvas.y += nameRect.height() + kGap;
    const bool ready = !nameText.trimmed().isEmpty() && !serverName.isEmpty();
    canvas.button(canvas.left, canvas.y, tr("Start the jam"), ready, [this] { start(); });
    canvas.y += canvas.buttonHeight() + kGap;
    if (!startProblem.isEmpty()) {
        canvas.paragraph(startProblem, canvas.current);
    }
    canvas.y += kGap * 2;
    canvas.line(
        serverName.isEmpty() ? tr("The jam server is not set") : tr("Server: %1").arg(serverName),
        canvas.normal
    );
    canvas.y += kGap;
    canvas.button(canvas.left, canvas.y, tr("Server settings…"), true, [this] {
        Q_EMIT serverSettingsRequested();
    });
}

void JamWindow::renderCreating(Canvas& canvas) {
    canvas.line(tr("New jam"), canvas.current, true);
    canvas.paragraph(tr("Connecting to the jam server…"), canvas.normal);
    canvas.y += kGap;
    canvas.button(canvas.left, canvas.y, tr("Cancel"), true, [this] { jamHost->cancelCreate(); });
}

void JamWindow::renderTabs(Canvas& canvas) {
    const QRect jamTab = canvas.button(
        canvas.left, canvas.y, tr("Jam"), true, [this] { showPage(Page::Jam); },
        currentPage == Page::Jam
    );
    const QRect searchTab = canvas.button(
        jamTab.right() + 1 + kGap, canvas.y, tr("Add tracks"), true,
        [this] { showPage(Page::Search); }, currentPage == Page::Search
    );
    const bool connected = jamHost->isConnected();
    const Jam::Status status = jamHost->connection();
    const QString state = connected                                          ? tr("Connected")
        : status == Jam::Status::Connecting || status == Jam::Status::Online ? tr("Connecting…")
                                                                             : tr("No connection");
    const int stateWidth = canvas.textWidth(state) + 2;  // advances are fractional
    const int stateX = canvas.right - stateWidth;
    if (stateX - 8 > searchTab.right() + kGap) {
        canvas.light(stateX - 7, jamTab.center().y(), connected);
        canvas.text(
            QRect(stateX, jamTab.y(), stateWidth, jamTab.height()), state,
            connected ? canvas.normal : canvas.current
        );
    }
    canvas.y += canvas.buttonHeight() + kGap;
    if (!connected) {
        canvas.paragraph(
            tr("No connection to the jam server · trying again by itself"), canvas.current
        );
        canvas.y += kGap;
    }
}

void JamWindow::renderJam(Canvas& canvas) {
    const bool connected = jamHost->isConnected();
    const QImage& code = qrCode();
    const int top = canvas.y;
    int textLeft = canvas.left;
    int qrBottom = top;
    if (!code.isNull()) {
        const int side = std::min(kMaxQrSide, canvas.width() / 2 - kGap);
        const int module = std::max(1, side / code.width());
        const QRect qrRect(canvas.left, top, code.width() * module, code.height() * module);
        if (canvas.painter) {
            canvas.painter->drawImage(qrRect, code);
        }
        textLeft = qrRect.right() + 1 + kGap * 2;
        qrBottom = qrRect.bottom() + 1;
    }
    canvas.text(
        QRect(textLeft, canvas.y, canvas.right - textLeft, canvas.lineHeight),
        tr("Link for the guests"), canvas.current, true
    );
    canvas.y += canvas.lineHeight;
    canvas.paragraph(jamHost->joinUrl(), canvas.normal, 3, Qt::TextWrapAnywhere, textLeft);
    canvas.y += kGap;
    const QRect copy = canvas.button(textLeft, canvas.y, tr("Copy"), true, [this] { copyLink(); });
    const QString newLink = tr("New link");
    int newLinkX = copy.right() + 1 + kGap;
    if (newLinkX + canvas.buttonWidth(newLink) > canvas.right) {
        newLinkX = textLeft;
        canvas.y += canvas.buttonHeight() + kGap;
    }
    canvas.button(newLinkX, canvas.y, newLink, connected, [this] {
        send(jamHost->rotateLink(), tr("The guests' link has changed"));
    });
    canvas.y = std::max(qrBottom, canvas.y + canvas.buttonHeight()) + kGap * 2;

    const std::optional<Jam::Room>& room = jamHost->room();
    if (room) {
        const Jam::Settings& settings = room->settings;
        const auto setting = [&](const QString& label, const QString& value,
                                 Jam::SettingsPatch patch) {
            canvas.text(
                QRect(canvas.left, canvas.y, canvas.width(), canvas.buttonHeight()), label,
                canvas.normal
            );
            canvas.button(
                canvas.right - canvas.buttonWidth(value), canvas.y, value, connected,
                [this, patch] { send(jamHost->changeSettings(patch)); }
            );
            canvas.y += canvas.buttonHeight() + 1;
        };
        const bool roundRobin = settings.order == Jam::Order::RoundRobin;
        setting(
            tr("Order"), roundRobin ? tr("Taking turns") : tr("First come"),
            Jam::SettingsPatch{roundRobin ? Jam::Order::Fifo : Jam::Order::RoundRobin, {}, {}, {}}
        );
        setting(
            tr("Guests can skip"), settings.guestsCanSkip ? tr("Yes") : tr("No"),
            Jam::SettingsPatch{{}, !settings.guestsCanSkip, {}, {}}
        );
        setting(
            tr("New guests"), settings.joinOpen ? tr("Let in") : tr("Closed"),
            Jam::SettingsPatch{{}, {}, !settings.joinOpen, {}}
        );
        canvas.y += kGap;
    }

    QList<Jam::Participant> guests;
    if (room) {
        for (const Jam::Participant& participant : room->participants) {
            if (participant.kind != Jam::ParticipantKind::Host) {
                guests << participant;
            }
        }
    }
    canvas.line(tr("Guests · %1").arg(guests.size()), canvas.current, true);
    const int endTop = canvas.bottom - canvas.buttonHeight();
    const int rowHeight = canvas.buttonHeight() + 1;
    listRows = std::max(0, (endTop - kGap - canvas.y) / rowHeight);
    listScroll = std::clamp(listScroll, 0, std::max(0, static_cast<int>(guests.size()) - listRows));
    if (guests.isEmpty()) {
        canvas.paragraph(tr("Nobody yet. Show the guests the QR code or the link."), canvas.dim);
    }
    for (int i = listScroll; i < guests.size() && i < listScroll + listRows; ++i) {
        const Jam::Participant& guest = guests[i];
        const QString kick = tr("Remove");
        const int kickX = canvas.right - canvas.buttonWidth(kick);
        canvas.light(canvas.left, canvas.y + canvas.buttonHeight() / 2, guest.online);
        QString line = guest.name + QStringLiteral(" · ") + KindText(guest.kind);
        if (guest.pending > 0) {
            line += tr(" · waiting: %1").arg(guest.pending);
        }
        canvas.text(
            QRect(canvas.left + 7, canvas.y, kickX - kGap - canvas.left - 7, canvas.buttonHeight()),
            line, guest.online ? canvas.normal : canvas.dim
        );
        const QString publicId = guest.publicId;
        canvas.button(kickX, canvas.y, kick, connected, [this, publicId] {
            send(jamHost->kick(publicId));
        });
        canvas.y += rowHeight;
    }

    canvas.button(
        canvas.left, endTop, endArmed ? tr("Press again to end") : tr("End the jam"), true,
        [this] { end(); }, endArmed
    );
}

void JamWindow::renderSearch(Canvas& canvas) {
    const bool connected = jamHost->isConnected();
    const QString find = tr("Search");
    const int findX = canvas.right - canvas.buttonWidth(find);
    field(
        canvas, QRect(canvas.left, canvas.y, findX - kGap - canvas.left, canvas.buttonHeight()),
        Field::Search, tr("Search tracks for the jam")
    );
    canvas.button(findX, canvas.y, find, !searchText.trimmed().isEmpty(), [this] {
        search(searchText);
    });
    canvas.y += canvas.buttonHeight() + kGap * 2;

    switch (results) {
        case Results::None:
            canvas.paragraph(
                tr("Find a track: “To the jam” puts it in the guests' queue, “Next” plays it "
                   "next."),
                canvas.dim
            );
            return;
        case Results::Loading: canvas.line(tr("Searching…"), canvas.normal); return;
        case Results::Failed:
            canvas.paragraph(tr("The search failed: %1").arg(searchProblem), canvas.current, 3);
            return;
        case Results::Found: break;
    }
    if (foundTracks.isEmpty()) {
        canvas.line(tr("Nothing found"), canvas.normal);
        return;
    }
    const int rowHeight = canvas.buttonHeight() + 1;
    listRows = std::max(0, (canvas.bottom - canvas.y) / rowHeight);
    listScroll =
        std::clamp(listScroll, 0, std::max(0, static_cast<int>(foundTracks.size()) - listRows));
    const QString add = tr("To the jam");
    const QString next = tr("Next");
    for (int i = listScroll; i < foundTracks.size() && i < listScroll + listRows; ++i) {
        const Yandex::Track track = foundTracks[i];
        const int nextX = canvas.right - canvas.buttonWidth(next);
        const int addX = nextX - kGap - canvas.buttonWidth(add);
        QString title = track.title;
        if (!track.artists.isEmpty()) {
            title += QStringLiteral(" — ") + track.artists.join(QStringLiteral(", "));
        }
        canvas.text(
            QRect(canvas.left, canvas.y, addX - kGap - canvas.left, canvas.buttonHeight()), title,
            canvas.normal
        );
        canvas.button(addX, canvas.y, add, connected, [this, track] {
            send(jamHost->add(track), tr("Sent to the jam"));
        });
        canvas.button(nextX, canvas.y, next, connected, [this, track] {
            send(jamHost->playNext(track), tr("Sent to the jam, plays next"));
        });
        canvas.y += rowHeight;
    }
}

void JamWindow::field(Canvas& canvas, const QRect& rect, Field which, const QString& hint) {
    const bool focused = focus == which;
    const QString& content = fieldText(which);
    if (canvas.painter) {
        canvas.painter->setPen(focused ? canvas.current : canvas.normal);
        canvas.painter->drawRect(rect.adjusted(0, 0, -1, -1));
        const QRect inner = rect.adjusted(3, 0, -3, 0);
        if (content.isEmpty() && !focused) {
            canvas.text(inner, hint, canvas.dim);
        } else {
            // The end of a long text stays in sight, where the caret is.
            const QString shown =
                canvas.metrics.elidedText(content, Qt::ElideLeft, inner.width() - 2);
            canvas.text(inner, shown, canvas.current);
            if (focused) {
                const int caretX = inner.x() + canvas.textWidth(shown) + 1;
                canvas.painter->setPen(canvas.current);
                canvas.painter->drawLine(caretX, rect.y() + 2, caretX, rect.bottom() - 2);
            }
        }
    }
    canvas.controls << Control{rect, hint, true, [this, which] {
                                   focus = which;
                                   update();
                               }};
}

const QImage& JamWindow::qrCode() {
    const QString url = jamHost->joinUrl();
    if (url == qrUrl) {
        return qrImage;
    }
    qrUrl = url;
    qrImage = QImage();
    if (url.isEmpty()) {
        return qrImage;
    }
    try {
        const qrcodegen::QrCode code =
            qrcodegen::QrCode::encodeText(url.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int size = code.getSize();
        // Dark on light whatever the skin: not every camera reads an inverted code.
        QImage image(size + kQuietZone * 2, size + kQuietZone * 2, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (code.getModule(x, y)) {
                    image.setPixel(x + kQuietZone, y + kQuietZone, qRgb(0, 0, 0));
                }
            }
        }
        qrImage = image;
    } catch (const qrcodegen::data_too_long&) {
        qWarning("jam: the link is too long for a QR code");
    }
    return qrImage;
}

void JamWindow::start() {
    startProblem.clear();
    if (!jamHost->create(nameText)) {
        startProblem = serverName.isEmpty() ? tr("Set the jam server first")
                                            : tr("Cannot start: check the name and the server");
    }
    focus = Field::None;
    update();
}

void JamWindow::copyLink() {
    QGuiApplication::clipboard()->setText(jamHost->joinUrl());
    Q_EMIT statusText(tr("The link is copied"));
}

void JamWindow::end() {
    if (!endArmed) {
        endArmed = true;
        endTimer.start();
        update();
        return;
    }
    endTimer.stop();
    endArmed = false;
    jamHost->end();
}

void JamWindow::send(bool sent, const QString& done) {
    if (!sent) {
        Q_EMIT statusText(tr("No connection to the jam server"));
    } else if (!done.isEmpty()) {
        Q_EMIT statusText(done);
    }
}

void JamWindow::search(const QString& text) {
    const QString query = text.trimmed();
    if (query.isEmpty()) {
        return;
    }
    searchText = query;
    results = Results::Loading;
    listScroll = 0;
    const int request = ++searchRequest;
    QPointer<JamWindow> self(this);
    yandexLibrary->searchTracks(
        query,
        [self, request](const QList<Yandex::Track>& tracks, const Yandex::RequestError& error) {
            if (!self || request != self->searchRequest) {
                return;
            }
            self->foundTracks.clear();
            if (error.isError()) {
                self->results = Results::Failed;
                self->searchProblem = error.text;
            } else {
                self->results = Results::Found;
                for (const Yandex::Track& track : tracks) {
                    if (track.available && self->foundTracks.size() < Jam::kMaxSearchResults) {
                        self->foundTracks << track;
                    }
                }
            }
            self->update();
        }
    );
    update();
}

QString& JamWindow::fieldText(Field which) {
    return which == Field::Name ? nameText : searchText;
}

bool JamWindow::contentMousePress(QPoint pos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) {
        return false;
    }
    const Field before = focus;
    focus = Field::None;
    for (const Control& control : render(nullptr)) {
        if (control.rect.contains(pos)) {
            if (control.enabled && control.action) {
                control.action();
            }
            update();
            return true;
        }
    }
    if (before != Field::None) {
        update();
    }
    return false;
}

bool JamWindow::event(QEvent* event) {
    // While a field has the focus, the keys type into it instead of driving the player.
    if (event->type() == QEvent::ShortcutOverride && focus != Field::None) {
        auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers modifiers = key->modifiers() & ~Qt::ShiftModifier;
        if (modifiers == Qt::NoModifier || key->matches(QKeySequence::Paste)) {
            event->accept();
            return true;
        }
    }
    return GenWindow::event(event);
}

void JamWindow::keyPressEvent(QKeyEvent* event) {
    if (focus == Field::None) {
        return GenWindow::keyPressEvent(event);
    }
    QString& content = fieldText(focus);
    const int maxLength = focus == Field::Name ? Jam::kMaxHostNameLength : kMaxSearchLength;
    if (event->matches(QKeySequence::Paste)) {
        QString pasted = QGuiApplication::clipboard()->text().simplified();
        content = (content + pasted).left(maxLength);
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (focus == Field::Name) {
            start();
        } else {
            search(content);
        }
    } else if (event->key() == Qt::Key_Backspace) {
        content.chop(1);
    } else if (event->key() == Qt::Key_Escape) {
        focus = Field::None;
    } else if (const QString typed = event->text(); !typed.isEmpty() && typed.at(0).isPrint()) {
        content = (content + typed).left(maxLength);
    } else {
        return GenWindow::keyPressEvent(event);
    }
    if (focus == Field::Name) {
        nameEdited = true;
    }
    update();
}

void JamWindow::wheelEvent(QWheelEvent* event) {
    const int steps = wheelSteps(event);
    if (steps != 0) {
        listScroll = std::max(0, listScroll - steps);
        update();
    }
}

void JamWindow::retranslate() {
    setWindowTitle(tr("QiYaa: jam"));
}

}  // namespace Ui
