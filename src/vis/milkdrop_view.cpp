#include "vis/milkdrop_view.h"

#include "audio/audio_engine.h"
#include "audio/vis_tap.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QSize>
#include <QStringList>
#include <QSurfaceFormat>
#include <projectM-4/projectM.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Vis {

namespace {
constexpr uint32_t kMaxFramesPerFeed = 4096;
constexpr int kProbeWidth = 32, kProbeHeight = 18;
constexpr int kBlackLevel = 12;

QSurfaceFormat ViewFormat() {
    // projectM 4 needs OpenGL 3.3 core. macOS only gives core profiles when asked.
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    format.setAlphaBufferSize(0);
    format.setSwapInterval(1);
    return format;
}
}  // namespace

QString MilkdropView::OpenGlProblem() {
    static const QString problem = [] {
        QOpenGLContext probe;
        probe.setFormat(ViewFormat());
        if (!probe.create()) {
            return tr("no OpenGL");
        }
        const QSurfaceFormat contextFormat = probe.format();
        if (probe.isOpenGLES()) {
            return tr("only OpenGL ES is here, and OpenGL 3.3 is needed");
        }
        if (contextFormat.majorVersion() * 10 + contextFormat.minorVersion() < 33) {
            return tr("OpenGL 3.3 is needed, and %1.%2 is here")
                .arg(contextFormat.majorVersion())
                .arg(contextFormat.minorVersion());
        }
        return QString();
    }();
    return problem;
}

MilkdropView::MilkdropView(Audio::AudioEngine* engine)
    : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate)
    , audioEngine(engine)
    , pcm(kMaxFramesPerFeed * 2) {
    setFormat(ViewFormat());
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, [this] { update(); });
}

MilkdropView::~MilkdropView() {
    destroyProjectM();
}

void MilkdropView::destroyProjectM() {
    if (!projectM) {
        return;
    }
    // projectM owns GL objects: free them with our context current.
    if (context()) {
        makeCurrent();
    }
    if (probeFramebuffer && QOpenGLContext::currentContext()) {
        QOpenGLExtraFunctions* gl = QOpenGLContext::currentContext()->extraFunctions();
        gl->glDeleteFramebuffers(1, &probeFramebuffer);
        gl->glDeleteTextures(1, &probeTexture);
        probeFramebuffer = probeTexture = 0;
    }
    projectm_destroy(projectM);
    projectM = nullptr;
    if (context()) {
        doneCurrent();
    }
}

void MilkdropView::loadPreset(const QByteArray& milk, PresetTransition transition) {
    pendingPreset = std::make_pair(milk, transition);
    update();
}

void MilkdropView::setBlackWatch(bool on) {
    if (on == blackWatch) {
        return;
    }
    blackWatch = on;
    blackChecks = 0;
    sinceLoad.start();
}

void MilkdropView::captureNextFrame() {
    captureRequested = true;
    update();
}

void MilkdropView::setBlackWatchTiming(int graceMs, int intervalMs, int checks) {
    blackGraceMs = graceMs;
    blackIntervalMs = intervalMs;
    blackChecksNeeded = checks;
}

void MilkdropView::setPresetDuration(double seconds) {
    duration = seconds;
    applySettings();
}

void MilkdropView::setLocked(bool locked) {
    lockEnabled = locked;
    applySettings();
}

void MilkdropView::setTextureSearchPaths(const QStringList& paths) {
    texturePaths = paths;
    texturePathsDirty = true;
    update();
}

void MilkdropView::setRendering(bool on, int fps) {
    if (!on) {
        timer.stop();
        return;
    }
    timer.setInterval(1000 / std::max(1, fps));
    if (!timer.isActive()) {
        visReadCursor = audioEngine->visCursor();
        timer.start();
    }
}

void MilkdropView::applySettings() {
    if (!projectM) {
        return;
    }
    projectm_set_preset_duration(projectM, duration);
    projectm_set_preset_locked(projectM, lockEnabled);
}

void MilkdropView::applyTexturePaths() {
    // Needs our context current (paintGL / initializeGL).
    std::vector<QByteArray> utf8Paths;
    std::vector<const char*> pathPointers;
    for (const QString& path : texturePaths) {
        utf8Paths.push_back(path.toUtf8());
    }
    for (const QByteArray& utf8Path : utf8Paths) {
        pathPointers.push_back(utf8Path.constData());
    }
    projectm_set_texture_search_paths(projectM, pathPointers.data(), pathPointers.size());
    texturePathsDirty = false;
}

void MilkdropView::syncWindowSize() {
    // Device pixels: a move to a screen with another scale factor changes them
    // without a resize.
    const QSize devicePixelSize = size() * devicePixelRatio();
    if (devicePixelSize == pixelSize) {
        return;
    }
    pixelSize = devicePixelSize;
    projectm_set_window_size(
        projectM, static_cast<size_t>(devicePixelSize.width()),
        static_cast<size_t>(devicePixelSize.height())
    );
}

void MilkdropView::initializeGL() {
    QOpenGLContext* windowContext = context();
    const QSurfaceFormat contextFormat = windowContext ? windowContext->format() : QSurfaceFormat();
    if (!windowContext || !windowContext->isValid()
        || QOpenGLContext::currentContext() != windowContext) {
        failureReason = tr("no OpenGL");
    } else if (windowContext->isOpenGLES()) {
        failureReason = tr("only OpenGL ES is here, and OpenGL 3.3 is needed");
    } else if (contextFormat.majorVersion() * 10 + contextFormat.minorVersion() < 33) {
        failureReason = tr("OpenGL 3.3 is needed, and %1.%2 is here")
                            .arg(contextFormat.majorVersion())
                            .arg(contextFormat.minorVersion());
    } else {
        projectM = projectm_create();
        if (!projectM) {
            failureReason = tr("projectM did not start");
        }
    }
    if (!projectM) {
        timer.stop();
        Q_EMIT failed(failureReason);
        return;
    }
    pixelSize = {};
    syncWindowSize();
    projectm_set_aspect_correction(projectM, true);
    projectm_set_soft_cut_duration(projectM, 3.0);
    projectm_set_fps(projectM, 60);

    // Both callbacks fire inside projectM calls: hand them to the event loop.
    projectm_set_preset_switch_requested_event_callback(
        projectM,
        [](bool hardCut, void* self) {
            auto* view = static_cast<MilkdropView*>(self);
            const PresetTransition transition =
                hardCut ? PresetTransition::Cut : PresetTransition::Blend;
            QMetaObject::invokeMethod(
                view, [view, transition] { Q_EMIT view->switchRequested(transition); },
                Qt::QueuedConnection
            );
        },
        this
    );
    projectm_set_preset_switch_failed_event_callback(
        projectM,
        [](const char*, const char* message, void* self) {
            auto* view = static_cast<MilkdropView*>(self);
            const QString messageText = QString::fromUtf8(message);
            QMetaObject::invokeMethod(
                view, [view, messageText] { Q_EMIT view->presetFailed(messageText); },
                Qt::QueuedConnection
            );
        },
        this
    );
    applySettings();
    applyTexturePaths();
    visReadCursor = audioEngine->visCursor();
    {
        QOpenGLFunctions* gl = windowContext->functions();
        auto glString = [gl](GLenum name) {
            return QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(name)));
        };
        glInfoText = glString(GL_VERSION) + QStringLiteral(" | ") + glString(GL_RENDERER);
        qInfo("Milkdrop: OpenGL %s", qPrintable(glInfoText));
    }
    sinceLoad.start();
    sinceCheck.start();
    Q_EMIT ready();
}

void MilkdropView::resizeGL(int, int) {
    if (projectM) {
        syncWindowSize();
    }
}

void MilkdropView::paintGL() {
    if (!projectM) {
        if (QOpenGLContext* currentContext = QOpenGLContext::currentContext();
            currentContext && currentContext == context()) {
            currentContext->functions()->glClearColor(0, 0, 0, 1);
            currentContext->functions()->glClear(GL_COLOR_BUFFER_BIT);
        }
        return;
    }
    syncWindowSize();
    if (texturePathsDirty) {
        applyTexturePaths();
    }
    if (pendingPreset) {
        const auto [milk, transition] = *std::exchange(pendingPreset, std::nullopt);
        projectm_load_preset_data(
            projectM, milk.constData(), transition == PresetTransition::Blend
        );
        sinceLoad.start();
        blackChecks = 0;
        sawPicture = false;
    }
    const Audio::VisReadResult newSamples = audioEngine->readNewVisSamples(visReadCursor, pcm);
    visReadCursor = newSamples.cursor;
    if (newSamples.frames > 0) {
        projectm_pcm_add_float(projectM, pcm.data(), newSamples.frames, PROJECTM_STEREO);
    }
    projectm_opengl_render_frame(projectM);
    makeOpaque();
    ++frameCount;
    watchForBlack();
    if (std::exchange(captureRequested, false)) {
        QImage frame(pixelSize, QImage::Format_RGBA8888);
        QOpenGLFunctions* gl = context()->functions();
        gl->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        gl->glReadPixels(
            0, 0, frame.width(), frame.height(), GL_RGBA, GL_UNSIGNED_BYTE, frame.bits()
        );
        frame = frame.mirrored();  // OpenGL rows start at the bottom
        QMetaObject::invokeMethod(
            this, [this, frame] { Q_EMIT frameCaptured(frame); }, Qt::QueuedConnection
        );
    }
}

void MilkdropView::makeOpaque() {
    // projectM leaves a preset's alpha (often 0); a window that got an alpha channel anyway
    // (an ARGB visual on X11/XWayland) would composite those pixels as see-through.
    QOpenGLFunctions* gl = context()->functions();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    gl->glClearColor(0, 0, 0, 1);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void MilkdropView::watchForBlack() {
    if (!blackWatch || sinceLoad.elapsed() < blackGraceMs
        || sinceCheck.elapsed() < blackIntervalMs) {
        return;
    }
    sinceCheck.start();
    if (!pictureIsBlack()) {
        blackChecks = 0;
        if (!std::exchange(sawPicture, true)) {
            QMetaObject::invokeMethod(
                this, [this] { Q_EMIT drawsPicture(); }, Qt::QueuedConnection
            );
        }
        return;
    }
    if (++blackChecks < blackChecksNeeded) {
        return;
    }
    blackChecks = 0;
    sinceLoad.start();
    QMetaObject::invokeMethod(this, [this] { Q_EMIT staysBlack(); }, Qt::QueuedConnection);
}

bool MilkdropView::pictureIsBlack() {
    QOpenGLExtraFunctions* gl = context()->extraFunctions();
    if (!probeFramebuffer) {
        gl->glGenTextures(1, &probeTexture);
        gl->glBindTexture(GL_TEXTURE_2D, probeTexture);
        gl->glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, kProbeWidth, kProbeHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
            nullptr
        );
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glGenFramebuffers(1, &probeFramebuffer);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, probeFramebuffer);
        gl->glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, probeTexture, 0
        );
    }
    const GLuint windowFramebuffer = defaultFramebufferObject();
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, windowFramebuffer);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, probeFramebuffer);
    gl->glBlitFramebuffer(
        0, 0, pixelSize.width(), pixelSize.height(), 0, 0, kProbeWidth, kProbeHeight,
        GL_COLOR_BUFFER_BIT, GL_LINEAR
    );
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, probeFramebuffer);
    std::array<quint8, kProbeWidth * kProbeHeight * 4> pixels{};
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glReadPixels(0, 0, kProbeWidth, kProbeHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl->glBindFramebuffer(GL_FRAMEBUFFER, windowFramebuffer);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (std::max({pixels[i], pixels[i + 1], pixels[i + 2]}) >= kBlackLevel) {
            return false;
        }
    }
    return true;
}

void MilkdropView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        Q_EMIT doubleClicked();
    }
}

void MilkdropView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        Q_EMIT contextMenuRequested(event->globalPosition().toPoint());
    }
}

void MilkdropView::keyPressEvent(QKeyEvent* event) {
    Q_EMIT keyPressed(event->key(), event->modifiers());
}

}  // namespace Vis
