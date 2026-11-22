#pragma once

#include <QObject>
#include <QTcpServer>
#include <functional>

#include <memory>

namespace veyra::ui {


// Local HTTP endpoint the browser extension talks to.
//
//   GET  /ping          -> {"ok":true,"app":"veyra"}   (extension uses it to detect Veyra)
//   POST /play          -> {"ok":true}                  body: {"url":"https://...","quality":0,"cookie":"..."}
//
// `quality` is the Bilibili qn the page is currently showing (0 = let Veyra pick
// the best). `cookie` is the browser's Bilibili session; it is optional and kept
// only for logging - Veyra already owns a DPAPI-encrypted session of its own.
//
// Loopback only, bounded HTTP framing, extension-origin/Host validation; queued
// handler receives only validated HTTP(S) playback requests. No wildcard CORS.
class PlaybackServer : public QObject {
    Q_OBJECT
public:
    // quality: 0 = best available, otherwise the Bilibili qn from the page.
    // parts: one entry per page of a multi-part Bilibili video, read from the page
    // DOM by the extension so the player can show the site's own titles. Empty for
    // everything else. Each entry is {p: <1-based page number>, title: <text>, url}.
    struct Part { int page = 0; QString title; QString url; };
    using PlayHandler=std::function<void(const QString& url, int quality, const QList<Part>& parts)>;
    explicit PlaybackServer(PlayHandler handler, QObject* parent = nullptr);
    ~PlaybackServer() override;

    bool listen(quint16 port = 0);
    quint16 serverPort() const { return server_.serverPort(); }

private:
    void onNewConnection();
    void handleRequest(QTcpSocket* socket, const QByteArray& request);
    static void respond(QTcpSocket* socket, int status, const QByteArray& body, const QByteArray& origin={});

    PlayHandler handler_;
    QTcpServer server_;
    unsigned activeConnections_=0;
};

} // namespace veyra::ui
