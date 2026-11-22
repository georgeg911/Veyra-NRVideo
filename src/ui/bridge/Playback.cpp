// QML bridge: Playback responsibilities.
#include "../QmlPlayerBridge_p.h"
#include "../LiveTranslation.h"

#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <functional>
#include <memory>
#include <cmath>
#include <regex>
#include "veyra/pipeline/NrReuse.h"

namespace veyra::ui {
// --- playback ---------------------------------------------------------------
QString QmlPlayerBridge::statusText() const { return uiText(impl_->snapshot.status); }
bool QmlPlayerBridge::running() const { return impl_->snapshot.running; }
bool QmlPlayerBridge::paused() const { return impl_->snapshot.transport == engine::TransportState::Paused; }
bool QmlPlayerBridge::failed() const { return impl_->snapshot.failed; }
bool QmlPlayerBridge::isImage() const { return impl_->snapshot.image; }
bool QmlPlayerBridge::isCapture() const { return impl_->snapshot.capture; }
bool QmlPlayerBridge::hasSource() const { return impl_->snapshot.running || impl_->snapshot.image; }
bool QmlPlayerBridge::openingSource() const { return impl_->openingSource; }
double QmlPlayerBridge::position() const { return impl_->snapshot.position; }
double QmlPlayerBridge::duration() const { return impl_->snapshot.duration; }
double QmlPlayerBridge::progress() const {
    const auto& n=impl_->snapshot.network;
    if (n.timeshiftEnabled&&n.timeshiftEndSeconds>n.timeshiftStartSeconds) {
        const double span=n.timeshiftEndSeconds-n.timeshiftStartSeconds;
        return std::clamp((position()-n.timeshiftStartSeconds)/span,0.0,1.0);
    }
    const double d = impl_->snapshot.duration;
    return d > 0.01 ? std::clamp(impl_->snapshot.position / d, 0.0, 1.0) : 0.0;
}
QString QmlPlayerBridge::positionText() const {
    const auto& n=impl_->snapshot.network;
    if(n.timeshiftEnabled&&n.timeshiftEndSeconds>n.timeshiftStartSeconds) {
        const double behind=std::max(0.0,n.timeshiftEndSeconds-position());
        return behind<1 ? tr("直播") : QStringLiteral("−")+mmss(behind);
    }
    return mmss(impl_->snapshot.position);
}
QString QmlPlayerBridge::durationText() const {
    return impl_->snapshot.network.live ? tr("直播") : mmss(impl_->snapshot.duration);
}
bool QmlPlayerBridge::timelineSeekable() const {
    const auto& n=impl_->snapshot.network;
    return n.live ? n.timeshiftEnabled&&n.timeshiftEndSeconds>n.timeshiftStartSeconds : duration()>0&&!isCapture();
}
QVariantMap QmlPlayerBridge::liveTimeshift() const {
    const auto& n=impl_->snapshot.network;
    return {{"enabled",n.timeshiftEnabled},{"available",n.live&&timelineSeekable()},
            {"start",n.timeshiftStartSeconds},{"end",n.timeshiftEndSeconds}};
}
double QmlPlayerBridge::timelineSeconds(double fraction) const {
    const auto& n=impl_->snapshot.network;
    fraction=std::clamp(fraction,0.0,1.0);
    if(n.timeshiftEnabled)return n.timeshiftStartSeconds+fraction*(n.timeshiftEndSeconds-n.timeshiftStartSeconds);
    return fraction*duration();
}
void QmlPlayerBridge::returnToLive() {
    if(!impl_->snapshot.network.timeshiftEnabled||!timelineSeekable()) return;
    seekTo(impl_->snapshot.network.timeshiftEndSeconds);
    if(paused())togglePlayPause();
}

// --- source -----------------------------------------------------------------
QString QmlPlayerBridge::sourceName() const {
    if (impl_->sourceLabel.empty()) return QString();
    if(impl_->liveKind==QLatin1String("url"))return tr("网络视频 · %1").arg(impl_->liveLabel);
    return QFileInfo(utf8Of(impl_->sourceLabel)).fileName();
}
QString QmlPlayerBridge::sourceSummary() const {
    const auto& s = impl_->snapshot;
    if (!s.sourceWidth) return QString();
    // Reported values only: no upscaling claim, no invented bit depth.
    return QStringLiteral("%1x%2").arg(s.sourceWidth).arg(s.sourceHeight);
}

int QmlPlayerBridge::sourceWidth() const { return int(impl_->snapshot.sourceWidth); }
int QmlPlayerBridge::sourceHeight() const { return int(impl_->snapshot.sourceHeight); }
double QmlPlayerBridge::sourceFps() const { return impl_->snapshot.nominalSourceFps; }
int QmlPlayerBridge::sourceRotation() const { return impl_->snapshot.sourceRotationDegrees; }
int QmlPlayerBridge::previewRotation() const { return int(impl_->engine.previewView().userQuarterTurns)*90; }
void QmlPlayerBridge::rotatePictureClockwise() {
    if(impl_->screenCropPreview||!hasSource()||!impl_->engine.rotatePreviewClockwise(impl_->snapshot.sessionId))return;
    // An in-progress protection selection uses the previous coordinate system.
    beginProtectionDraw(QString());
    tickSubtitles();
    if(gameMode()){updateGameGeometry();updateGameMode();emit gameModeChanged();}
    veyra::log::info("ui-rotation",std::format("session={} clockwise={}",impl_->snapshot.sessionId,previewRotation()));
    emit compareChanged();
}
double QmlPlayerBridge::sourceAspect() const {
    const auto& s = impl_->snapshot;
    // A known DAR already includes SAR/rotation. With no DAR, rotate the coded
    // extent as the presenter does; phone files can omit SAR yet carry a matrix.
    engine::PreviewView view;
    view.displayAspect=s.sourceDisplayAspect;
    view.rotationDegrees=s.sourceRotationDegrees;
    return view.aspect(float(s.sourceWidth),float(s.sourceHeight));
}

// --- live performance -------------------------------------------------------
// XeSS generates inside the provider: the engine only submits source frames,
// so its own rate would read 60 while the SDK presents 120. Show the provider's
// reported submissions for XeSS (AppShell: "SDK提交（非屏幕实测）").
static bool xessPresenting(const engine::PlayerSnapshot& s) {
    return s.fgActive && s.applied.frameGenerationBackend == engine::FrameGenerationBackend::XeSS &&
        s.metrics.flow.xessSdkSubmitFps > 0;
}
double QmlPlayerBridge::submitFps() const {
    const auto& s = impl_->snapshot;
    return xessPresenting(s) ? s.metrics.flow.xessSdkSubmitFps : s.submissionFps.value_or(0.0);
}
bool QmlPlayerBridge::submitFpsKnown() const { return xessPresenting(impl_->snapshot) || impl_->snapshot.submissionFps.has_value(); }
// Our own presents only: while the XeSS SDK presents its frames itself the
// intervals between them are not observed here, so there is no 1% low.
double QmlPlayerBridge::submitLowFps() const { return submitLowFpsKnown() ? *impl_->snapshot.submissionLow1Fps : 0.0; }
bool QmlPlayerBridge::submitLowFpsKnown() const { return !xessPresenting(impl_->snapshot) && impl_->snapshot.submissionLow1Fps.has_value(); }
QString QmlPlayerBridge::submitFpsLabel() const { return xessPresenting(impl_->snapshot) ? tr("SDK 提交") : tr("提交"); }
double QmlPlayerBridge::lateMs() const { return impl_->snapshot.lateMs; }
double QmlPlayerBridge::lateP95Ms() const { return impl_->snapshot.lateP95Ms; }
QString QmlPlayerBridge::metricsSummary() const {
    const auto& s = impl_->snapshot;
    // Submit FPS, named as such: it is what we handed to the presenter, not what
    // the panel scanned out. There is no "display FPS" because we cannot measure
    // one, and a number we did not measure must not be shown as if we had.
    if (!s.submissionFps) return tr("未测量");
    return tr("提交 %1").arg(*s.submissionFps, 0, 'f', 1);
}
bool QmlPlayerBridge::nrActive() const { return impl_->snapshot.nrActive; }
bool QmlPlayerBridge::nrAutoActive() const {return impl_->snapshot.nrAutoActive;}
int QmlPlayerBridge::nrAutoPercent() const {return int(impl_->snapshot.nrAutoPercent);}
QString QmlPlayerBridge::nrAutoStatus() const {
    const auto& s=impl_->snapshot;
    if(s.nrAutoActive)return tr("NR 自动 %1% · %2×%3").arg(s.nrAutoPercent).arg(s.nrAutoWidth).arg(s.nrAutoHeight);
    if(s.nrAutoStatus==L"自动NR实例池不可用，保留1080p NR")return tr("自动 NR 实例池不可用，保留固定 1080p 档");
    if(!s.nrAutoStatus.empty())return tr("当前配置未启用自动 NR，保留固定 1080p 档");
    return {};
}
// NR inference reuse: what the running preview graph actually does. Nothing
// while the mode is off or nothing plays; a reason when the mode is on but
// this chain cannot reuse (EnhanceGraph::nrReuseIneligibility, ASCII).
bool QmlPlayerBridge::nrReuseActive() const { return impl_->snapshot.nrReuseActive; }
QString QmlPlayerBridge::nrReuseStatus() const {
    const auto& s=impl_->snapshot;
    if(settings().nrReuse==engine::NrReuseMode::Off)return {};
    if(s.nrReuseActive){
        const unsigned every=pipeline::NrReuseScheduler::framesPerReuse(s.nrReuseLevel);
        const QString state=every==0?tr("跟得上，每帧都推理"):tr("每 %1 帧复用 1 帧").arg(every);
        // No running count: hours of play reach millions of frames.
        return s.nrReuseAutomatic?tr("自动 · ")+state:state;
    }
    const auto& why=s.nrReuseUnavailable;
    if(why.empty())return {};
    QString reason;
    if(why=="not a realtime preview graph"||why=="still image")reason=tr("只在实时预览生效");
    else if(why=="NR not created")reason=tr("NR 未运行");
    else if(why=="NVIDIA NGX NR only")reason=tr("仅支持英伟达 NR");
    else if(why=="SDR only")reason=tr("HDR 处理时不复用");
    else if(why=="NR motion must use optical flow")reason=tr("NR 运动来源需为光流");
    else if(why=="NR split across SR")reason=tr("NR 分在超分前后时不复用");
    else if(why=="protection between NR stages")reason=tr("NR 层之间有保护区域时不复用");
    else if(why=="node mode")reason=tr("节点模式暂不支持");
    else if(why=="automatic NR size pool")reason=tr("自动 NR 尺寸开启时不复用");
    else reason=tr("资源不足");
    return tr("未生效：")+reason;
}
bool QmlPlayerBridge::srActive() const { return impl_->snapshot.srActive; }
bool QmlPlayerBridge::fgActive() const { return impl_->snapshot.fgActive; }
bool QmlPlayerBridge::captureRecovering() const { return impl_->snapshot.captureRecovering; }
double QmlPlayerBridge::captureFps() const { return impl_->snapshot.captureFps; }
double QmlPlayerBridge::captureDropped() const { return double(impl_->snapshot.captureDropped); }
QString QmlPlayerBridge::backendWarning() const { return uiText(impl_->snapshot.backendWarning); }

// --- commands ---------------------------------------------------------------
void QmlPlayerBridge::openFileDialog() {
    veyra::log::info("qml-file", "openFileDialog entered");
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("打开视频或图片"), QString(),
        tr("媒体文件 (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.m2ts *.jpg *.jpeg *.png *.bmp *.webp);;所有文件 (*)"));
    veyra::log::info("qml-file", path.isEmpty() ? "openFileDialog cancelled" : "openFileDialog selected file");
    if (!path.isEmpty()) openPath(path);
}
int QmlPlayerBridge::thumbnailGeneration() const { return impl_->thumbnailGeneration; }

QObject* QmlPlayerBridge::playlist() const { return impl_->playlist; }
void QmlPlayerBridge::showPlaylist() { emit playlistRequested(); }
void QmlPlayerBridge::addPlaylistFilesDialog() {
    const auto paths = QFileDialog::getOpenFileNames(nullptr, tr("添加文件到播放列表"), {},
        tr("媒体文件 (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.m2ts *.mts *.m4v *.wmv *.mpg *.mpeg *.vob *.flv *.ogv *.asf *.3gp *.jpg *.jpeg *.png *.bmp *.webp);;所有文件 (*)"));
    if (!paths.isEmpty()) { impl_->playlist->addPaths(paths); emit playlistRequested(); }
}
void QmlPlayerBridge::addPlaylistFolderDialog() {
    const auto path = QFileDialog::getExistingDirectory(nullptr, tr("导入文件夹到播放列表"));
    if (!path.isEmpty()) { impl_->playlist->addPaths({path}); emit playlistRequested(); }
}
void QmlPlayerBridge::openDroppedUrls(const QList<QUrl>& urls) {
    // A single file on the video retains drag-to-open. Folder/multi-file drops
    // append to the playlist; dropping inside PlaylistWindow always appends.
    if (urls.size() == 1) {
        const QUrl url = urls.front();
        if(!url.isLocalFile()){openUrl(url);return;}
        if (url.isLocalFile() && !QFileInfo(url.toLocalFile()).isDir()) { openUrl(url); return; }
    }
    impl_->playlist->addUrls(urls);
    emit playlistRequested();
}
void QmlPlayerBridge::tickPlaylist() {
    const auto& s = impl_->snapshot;
    using T = engine::TransportState;
    if (!impl_->playlistSession) return;
    // liveKind is "url" for a site page as well, so a non-empty liveKind alone cannot
    // mean "not a playlist source" - it is the source label that has to match the row.
    if (utf8Of(impl_->sourceLabel) != impl_->playlistPath) {
        impl_->playlistSession = 0; impl_->playlist->setPlayback({}, {}); return;
    }
    if (s.sessionId != impl_->playlistSession) return; // Ignore old session snapshots after open.
    if (s.capture || s.remotePlay) { impl_->playlistSession = 0; impl_->playlist->setPlayback({}, {}); return; }
    QString state = QStringLiteral("stopped");
    switch (s.transport) {
    case T::Opening: state = QStringLiteral("opening"); break;
    case T::Playing: state = s.image ? QStringLiteral("image") : QStringLiteral("playing"); break;
    case T::Paused: state = QStringLiteral("paused"); break;
    case T::Ended: state = QStringLiteral("ended"); break;
    case T::Failed: state = QStringLiteral("failed"); break;
    default: break;
    }
    impl_->playlist->setPlayback(impl_->playlistPath, state);
    if (s.transport != T::Ended || s.failed || s.image || impl_->openingSource ||
        impl_->playlistCompletedSession == s.sessionId) return;
    impl_->playlistCompletedSession = s.sessionId;
    // Wait for this GUI tick to finish, then recheck the engine session. A stop,
    // new file, or live source selected in between must cancel auto-advance.
    QTimer::singleShot(0, this, [this, session = s.sessionId] {
        const auto latest = impl_->engine.snapshot();
        // 不再用 liveKind 判断：站点页面播放时 liveKind 也是 "url"，那会让自动下一集
        // 永远不触发。playlistSession == session 已经足够说明「这个会话是列表里的条目」。
        // No liveKind test here: it is "url" for a site page too, which made auto-advance
        // impossible. playlistSession == session already means "this session is a list entry".
        if (impl_->playlistSession == session && latest.sessionId == session &&
            latest.transport == T::Ended && !latest.failed && !latest.image)
            impl_->playlist->advanceAfterEnd();
    });
}

void QmlPlayerBridge::openPath(const QString& path) {
    if (path.isEmpty()) return;
    // A veyra:// request is an instruction for this program, not a file: FFmpeg has no
    // such protocol and failed with "Protocol not found". The protocol handler has
    // already turned the real page address into an open by the time this runs, so the
    // only correct thing here is to refuse it and let the handler do its job.
    if (path.startsWith(QLatin1String("veyra://"), Qt::CaseInsensitive)) {
        veyra::log::warn("ui-open", "ignoring a veyra:// address passed to openPath");
        return;
    }
    // A playlist entry can now be a site page: a multi-part Bilibili video puts one
    // URL per part in the list so the series plays through. Those have to go through
    // the resolver - FFmpeg has no idea what an HTML page is. Checked here as well as
    // at the call sites because a page URL can arrive from the playlist, the recent
    // list or the command line.
    if (path.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
        path.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        if (media::isBilibiliPageUrl(path.toStdWString())) {
            // 先做一次状态清理再去解析。解析要 1-2 秒，这期间用户可能还停在屏幕裁剪
            // 编辑态或独占移动态；原来直接 return，这些状态要等到真正 openPath(CDN)
            // 才被清掉，而且源标签/画质也会短暂不一致。
            // Clear per-open UI state before handing off to the resolver: resolution takes
            // a second or two, and returning straight away left crop-edit / exclusion-move
            // states (and the source label) stale until the resolved open ran.
            impl_->screenCropPreview = false;
            endExclusionMove(false);
            impl_->exclusionSelected = -1;
            ++impl_->liveOpenGen;
            impl_->pendingLiveText.clear();
            openNetworkAddress(path, impl_->pendingBiliQuality);
            return;
        }
    }
    impl_->screenCropPreview = false;
    // A resolved Bilibili stream keeps its DASH audio address and the CDN's
    // Referer/UA for exactly this one open. Anything else must start clean, or
    // a local file would inherit the previous video's second audio track.
    const bool keepBilibiliStream = impl_->bilibiliOpenPending;
    impl_->bilibiliOpenPending = false;
    const QString biliTitle = impl_->pendingBiliTitle;
    impl_->pendingBiliTitle.clear();
    // Danmaku belongs to the source that loaded it. openPath() runs for every new
    // source, so a Bilibili comment track would otherwise keep flying over the next
    // local file (it did: 637 items stayed on screen). The resolved-open path sets
    // Bilibili stream state first and reloads its own track through
    // applyBilibiliExtras(), so only non-Bilibili opens clear here.
    if (!keepBilibiliStream) {
        if (impl_->danmakuWindow) {
            impl_->danmakuWindow->clearItems();
            impl_->danmakuWindow->hide();
        }
        impl_->danmakuEnabled = false;
        impl_->danmakuLastSeconds = -1.0;
    }
    endExclusionMove(false);
    impl_->exclusionSelected=-1;
    flushHdrMemory();impl_->hdrTitleApplied.clear();
    const bool network=media::networkMediaInput(wideOf(path));
    if (QFileInfo(path).isDir()) {
        impl_->playlist->addPaths({path}); emit playlistRequested(); return;
    }
    if (!network&&PlaylistModel::supported(path) && !impl_->playlist->containsPath(path)) impl_->playlist->addPaths({path});
    impl_->playlistPath = path;
    impl_->playlist->setPlayback(path, QStringLiteral("opening"));
    rememberPosition(true);
    impl_->exportQueue.setSelectedId(0);
    ++impl_->liveOpenGen; impl_->pendingLiveText.clear();   // a waiting live open is dropped
    impl_->exportTrimStartSeconds = 0.0;
    impl_->exportTrimEndSeconds = 0.0;
    emit exportChanged();
    emit mediaOpenRequested();
    impl_->openingSource = true;
    impl_->openingSessionId = 0;
    const std::wstring wide = wideOf(path);
    impl_->sourceLabel = wide;
    impl_->liveKind.clear();
    impl_->liveLabel.clear();
    if(network){impl_->liveKind=QStringLiteral("url");impl_->liveLabel=
        keepBilibiliStream?(biliTitle.isEmpty()?QStringLiteral("www.bilibili.com"):biliTitle):QUrl(path).host();}
    ++impl_->thumbnailGeneration;
    emit thumbnailSourceChanged(network?QString():path);
    emit thumbnailGenerationChanged();
    if(!network)impl_->facade.noteRecentFile(wide);
    emit recentFilesChanged();
    // Switch to a page with a video area, then settle the native window's
    // geometry, and only then open. The presenter samples the window's client
    // size once at initialisation; opening before that point is what produced a
    // 1x1 swapchain.
    if (impl_->currentPage.isEmpty() || impl_->currentPage == QLatin1String("home"))
        emit navigate(QStringLiteral("pro"));
    // A veyra:// URL is an instruction for this program, not a file and not a media
    // address: FFmpeg has no such protocol, so replaying it on the next launch failed
    // with "Protocol not found" (five times in the 2026-11-21 field log). The protocol
    // handler already resolved the real address by the time openPath runs, so what is
    // worth remembering is that resolved address, not the shell of the request.
    const bool protocolRequest = path.startsWith(QStringLiteral("veyra://"), Qt::CaseInsensitive);
    if(!network && !protocolRequest) rememberSource(QStringLiteral("file"), path);
    if (impl_->preOpen) impl_->preOpen();
    auto options = impl_->options;
    if (!keepBilibiliStream) {
        // The values live on impl_->options too, so clearing only the copy would
        // bring them back with the next settings rebuild.
        clearSiteStream(options);
        clearSiteStream(impl_->options);
    }
    options.softwareDecode = impl_->prefString("decode") == QLatin1String("software");
    options.hardwareDecodeOnly = impl_->prefString("decode") == QLatin1String("hardware");
    options.deinterlace=deinterlaceAvailable()&&impl_->prefBool("deinterlace",true);
    // Network / server playback (#48): read at open, like the decode choice above.
    options.network.level=media::networkBufferLevelFromId(impl_->prefString("networkBuffer").toStdString());
    options.network.startupPrebufferSeconds=impl_->prefs.value(QStringLiteral("networkPrebuffer"),-1).toInt();
    options.network.autoReconnect=impl_->prefBool("networkAutoReconnect",true);
    options.network.timeshiftSeconds=impl_->prefInt("networkTimeshift",0);
    options.network.corrupt=media::corruptPolicyFromId(impl_->prefString("networkCorrupt").toStdString());
    impl_->engine.open(impl_->videoWindow, wide, options);
    impl_->openingSessionId = impl_->engine.snapshot().sessionId;
    // 播放列表状态跟踪：站点页面（B 站视频，现在每个分集都是列表条目）需要它，
    // tickPlaylist() 靠它驱动「正在播放」和行高亮。这里拿到的是解析后的 CDN 地址
    // （页面地址在上面已经被转到 openNetworkAddress 了），所以「是不是站点页面」在这里
    // 判不出来；tickBilibiliResolve() 在 openPath 之后会把 playlistPath/sourceLabel
    // 指回页面并重新绑定会话，那才是让高亮生效的地方。
    // Playlist state tracking: a site page needs it (tickPlaylist drives the badge and the
    // row highlight). What reaches here is the resolved CDN address - the page URL was
    // already handed to openNetworkAddress above - so "is this a site page" cannot be
    // decided here; tickBilibiliResolve() re-points playlistPath/sourceLabel at the page
    // after openPath and rebinds the session, which is what actually makes it work.
    impl_->playlistSession = network ? 0 : impl_->openingSessionId;
    impl_->resumeSession = 0;
    if ((impl_->prefBool("rememberPosition", true)||impl_->prefBool("autoResume",false))) {
        const auto saved = impl_->prefs.value("positions").toMap().value(path).toMap();
        if (!saved.isEmpty()) { impl_->resumeSession = impl_->openingSessionId; impl_->resumeAt = saved.value("at").toDouble(); }
    }
    refreshSubtitles(network?std::wstring():wide);
}

bool QmlPlayerBridge::deinterlaceAvailable() const { return media::FFmpegDeinterlacer::available(); }
void QmlPlayerBridge::openNetworkDialog(){emit navigate(QStringLiteral("network"));}
bool QmlPlayerBridge::openNetworkAddress(const QString& address){
    return openNetworkAddress(address, 0);
}

bool QmlPlayerBridge::openNetworkAddress(const QString& address, int quality){
    const QString trimmed = address.trimmed();
    if (quality < 0 || quality > 127) quality = 0;
    // A Bilibili page URL is not a media URL: FFmpeg cannot open
    // https://www.bilibili.com/video/BV... directly. Resolve it to the CDN
    // address first (view?bvid -> cid, playurl -> DASH renditions). Both entry
    // points come through here: the paste dialog and the Chrome/Veyra protocol
    // handler (apps/veyra-qml/main.cpp).
    const std::wstring page = trimmed.toStdWString();
    if (media::isBilibiliPageUrl(page)) {
        if (impl_->biliResolveBusy) { emit notice(tr("上一个 B 站链接还在解析，请稍候"), false); return false; }
        const std::wstring cookie = media::loadBilibiliCookie();
        impl_->biliResolveSignedIn = media::bilibiliCookieLooksSignedIn(cookie);
        impl_->biliResolveBusy = true;
        impl_->biliResolveGen = impl_->liveOpenGen;
        const bool wantDanmaku = impl_->prefBool("biliDanmaku", false);
        const bool wantSubtitles = impl_->prefBool("biliSubtitles", false) && !cookie.empty();
        // Stash the page address for tickBilibiliResolve: that is where the stream is
        // finally accepted, and it is the page (resolvable again later) that belongs in
        // lastSource rather than the CDN address, whose signature expires.
        impl_->pendingBiliPage = trimmed;
        impl_->pendingBiliQuality = quality;
        // 每次打开都从「还没排过分集」开始。原来这面旗在解析成功路径的末尾复位，
        // 于是解析失败（网络错、需要登录、BV 不存在）或用户中途换片源时它残留 true，
        // 下一次真正成功的解析会跳过 API 兜底排队。作废点与赋值点放在一起才安全。
        // Start every open from "parts not queued yet". This used to be reset at the end
        // of the success path only, so a failed resolve (network error, login required,
        // bad BV) or an interrupted open left it true and the next successful resolve
        // skipped its API fallback. Reset where it is armed, not where it succeeds.
        impl_->webPartsQueued = false;
        // zh / en / both; the extras fall back to the other language when a video
        // only carries one of them.
        const QString langPref = impl_->prefString("biliSubtitleLang");
        const std::wstring subtitleMode = langPref.isEmpty() ? std::wstring(L"zh") : langPref.toStdWString();
        veyra::log::info("bilibili", std::format("resolving page URL (cookie: {}, qn: {}, danmaku: {}, subtitles: {}, subtitleMode: {})",
            impl_->biliResolveSignedIn ? "signed in" : "absent", quality, wantDanmaku, wantSubtitles,
            langPref.isEmpty() ? std::string("zh") : langPref.toStdString()));
        // The two overlay switches are read once, here: the same worker pass that
        // resolves the page also fetches the comment track and the CC subtitles for
        // the part it resolved, so the player never shows a video first and then
        // starts a second round-trip for its overlays.
        // std::async 可能抛 std::system_error（线程创建失败）。不接住的话异常会一路
        // 冒到 Qt 事件循环里变成 terminate，而且 biliResolveBusy 已经置 true、又没有
        // future，本进程内再也解析不了任何 B 站链接。异常时把整组状态一起回滚。
        // std::async can throw std::system_error when the thread cannot be created. Left
        // uncaught it reaches Qt's event loop and terminates, and biliResolveBusy is
        // already true with no future attached - no Bilibili link would ever resolve again
        // in this process. Roll the whole set of flags back on failure.
        try {
            impl_->biliResolve = std::async(std::launch::async,
                [page, cookie, quality, wantDanmaku, wantSubtitles, subtitleMode] {
                BilibiliBundle bundle;
                try { bundle.stream = media::resolveBilibiliStream(page, cookie, quality); }
                catch (...) { bundle.stream.error = L"Bilibili resolution failed"; return bundle; }
                if (!bundle.stream.ok) return bundle;
                bundle.wantDanmaku = wantDanmaku;
                bundle.wantSubtitles = wantSubtitles;
                std::wstring error;
                if (wantDanmaku && !media::fetchBiliDanmaku(bundle.stream.cid, bundle.danmaku, error))
                    bundle.extrasError = L"弹幕：" + error;
                error.clear();
                if (wantSubtitles &&
                    !media::fetchBiliSubtitle(bundle.stream.aid, bundle.stream.cid, cookie,
                                              subtitleMode,
                                              bundle.cues, bundle.subtitleStyle,
                                              bundle.subtitleLanguage, error))
                    bundle.extrasError += std::wstring(bundle.extrasError.empty() ? L"" : L"；") +
                                          L"字幕：" + error;
                return bundle;
            });
        } catch (const std::exception& e) {
            veyra::log::error("bilibili", std::format("could not start the resolve thread: {}", e.what()));
            impl_->biliResolveBusy = false;
            impl_->pendingBiliPage.clear();
            impl_->pendingBiliQuality = 0;
            emit notice(tr("无法启动解析线程，请稍后重试"), true);
            return false;
        }
        emit notice(tr("正在解析 B 站链接…"), false);
        return true;
    }
    const QUrl url(trimmed,QUrl::StrictMode);
    // A Bilibili page this build cannot resolve (bangumi, live, dynamics, a
    // homepage...) would otherwise go to FFmpeg as if it were a media file and
    // fail with "cannot open video". Say what is supported instead.
    if (url.isValid()) {
        const QString host = url.host().toLower();
        if (host == QLatin1String("bilibili.com") || host.endsWith(QStringLiteral(".bilibili.com")) ||
            host == QLatin1String("b23.tv")) {
            veyra::log::info("bilibili", std::format(
                "unsupported page host={} (only video/BV pages and b23.tv short links are resolved)",
                host.toStdString()));
            emit notice(tr("暂不支持这种 B 站页面，请用视频页（bilibili.com/video/BV…）或 b23.tv 短链"), true);
            return false;
        }
    }
    // rtp/udp are datagram transports: a port is part of the address, not optional.
    const bool datagram = url.scheme()==QLatin1String("rtp") || url.scheme()==QLatin1String("udp");
    if(!url.isValid()||url.host().isEmpty()||(datagram&&url.port()<=0)
       ||!media::networkMediaInput(wideOf(url.scheme()+QStringLiteral(":")))){
        emit notice(tr("请输入有效的 HTTP、HTTPS、RTSP、RTP 或 UDP 视频地址"),true);return false;
    }
    openPath(url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded));return true;
}

void QmlPlayerBridge::tickBilibiliResolve() {
    if (!impl_->biliResolveBusy) return;
    if (impl_->biliResolve.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    // Custom: .get() rethrows anything the worker raised (bad_alloc, format_error in
    // the danmaku/subtitle stage that the inner try/catch does not cover). Letting it
    // escape a Qt timer callback terminates the process, and biliResolveBusy would
    // stay true so no later resolve could ever run.
    BilibiliBundle bundle;
    try {
        bundle = impl_->biliResolve.get();
    } catch (const std::exception& e) {
        impl_->biliResolveBusy = false;
        veyra::log::error("bilibili", std::format("resolution threw: {}", e.what()));
        emit notice(tr("B 站链接解析出错：%1").arg(QString::fromUtf8(e.what())), true);
        return;
    } catch (...) {
        impl_->biliResolveBusy = false;
        veyra::log::error("bilibili", "resolution threw an unknown exception");
        emit notice(tr("B 站链接解析出错"), true);
        return;
    }
    impl_->biliResolveBusy = false;
    const media::BilibiliStream& stream = bundle.stream;
    if (impl_->biliResolveGen != impl_->liveOpenGen) {
        // Two API round-trips take a second or two; if the user opened anything
        // else meanwhile, that is what they want to watch.
        veyra::log::info("bilibili", "resolved stream dropped: the source has changed since the request");
        return;
    }
    if (!stream.ok) {
        veyra::log::warn("bilibili", QString::fromStdWString(stream.error).toStdString());
        emit notice(tr("B 站链接解析失败：%1").arg(QString::fromStdWString(stream.error)), true);
        return;
    }
    veyra::log::info("bilibili", std::format("resolved {}x{} qn={} audio={}",
        stream.width, stream.height, stream.quality, stream.audioUrl.empty() ? "muxed" : "dash"));
    // The CDN needs the site's Referer and User-Agent; they ride on
    // options.network (NetworkSettings), which EngineController copies to the
    // demux session and networkMediaHeaders() turns into the "headers" /
    // "user_agent" FFmpeg options. (od.networkHeaders is a leftover field that
    // nothing reads.) The audio address travels separately: a DASH video
    // container carries no audio track of its own.
    impl_->options.network.requestHeaders = stream.headers;
    impl_->options.network.userAgent = stream.userAgent;
    impl_->options.audioPath = stream.audioUrl;
    // The playurl response lists several live CDN edges for the same audio
    // rendition. Hand them to the engine so a dropped edge is replaced instead of
    // reopening the dead one until the buffering gate gives up.
    impl_->options.network.audioFallbacks = stream.audioFallbacks;
    impl_->bilibiliOpenPending = true;
    // The source label reads this while openPath() runs; a bare CDN host name
    // tells the user nothing about what is playing.
    impl_->pendingBiliTitle = QString::fromStdWString(stream.title);
    QString info = QStringLiteral("%1x%2").arg(stream.width).arg(stream.height);
    if (!stream.codec.empty()) info += QStringLiteral(" ") + QString::fromStdWString(stream.codec);
    if (!impl_->biliResolveSignedIn) info += tr("（未登录，画质受限）");
    emit notice(tr("正在播放：%1  %2").arg(QString::fromStdWString(stream.title), info), false);
    // Remember the page and quality, not the CDN address. A resolved stream address
    // carries a signature that expires within the hour, so replaying it on the next
    // launch would fail; the page can always be resolved again. "url" is its own kind
    // so resumeLastSource routes it back through openNetworkAddress (the resolver)
    // instead of handing it to FFmpeg as a file.
    {
        QVariantMap value{{"kind", "url"}, {"label", impl_->pendingBiliPage},
                          {"quality", impl_->pendingBiliQuality}};
        if (!impl_->pendingBiliPage.isEmpty() &&
            impl_->prefs.value(QStringLiteral("lastSource")).toMap() != value) {
            impl_->prefs[QStringLiteral("lastSource")] = value;
            if (!impl_->savePrefs()) veyra::log::warn("ui-prefs", "last source not saved");
            emit preferencesChanged();
        }
    }
    openPath(QString::fromStdWString(stream.url));
    // The engine is opened with the resolved CDN address, but the playlist stores site
    // *pages* - that is what can be re-resolved next launch. Point the playlist state at
    // the page again, otherwise tickPlaylist() compares the CDN address against the
    // stored page, decides the source is not in the list, and clears the highlight.
    if (!impl_->pendingBiliPage.isEmpty()) {
        const QString page = impl_->pendingBiliPage;
        impl_->playlistPath = page;
        impl_->sourceLabel = page.toStdWString();
        // Take the session id from the snapshot openPath() just started, not from the
        // value captured earlier: tickPlaylist() drops every frame whose sessionId does
        // not match, so a stale id froze the row at "opening" and it never reached
        // "playing".
        const auto live = impl_->engine.snapshot();
        impl_->openingSessionId = live.sessionId;
        impl_->playlistSession = live.sessionId;
        impl_->playlist->setPlayback(page, QStringLiteral("opening"));
        veyra::log::info("bilibili", std::format("playlist session bound to engine session {}",
                                                 live.sessionId));
    }
    // The overlays go on after the open: openPath() replaces the subtitle tracks,
    // so installing the site's CC track before it would be thrown away.
    applyBilibiliExtras(bundle);
    // Multi-part video: queue one playlist entry per page, so the series plays
    // through instead of stopping after the part that was opened. The browser
    // extension already sent the page's own titles (queueWebParts ran before this
    // open), so this API path is only the fallback for when it could not read the
    // page - otherwise it would add the same addresses twice with plainer names.
    if (stream.parts.size() > 1 && !impl_->webPartsQueued) {
        std::vector<std::wstring> titles;
        titles.reserve(stream.parts.size());
        for (const auto& part : stream.parts) titles.push_back(part.title);
        queueBilibiliParts(titles, stream.currentPart, impl_->pendingBiliPage);
    }
}

void QmlPlayerBridge::queueWebParts(const QList<WebPart>& parts, const QString& currentUrl) {
    if (parts.size() <= 1) return;
    impl_->webPartsQueued = true;   // the resolver must not queue a second copy
    // Deferred deliberately: this runs in the same tick as the open that follows it,
    // and inserting rows makes the QML playlist view rebuild while the native video
    // window is being re-created. That combination crashed in Qt6Qml before 2a6051b1.
    QTimer::singleShot(0, this, [this, parts, currentUrl]() {
        // 顺序很重要：先按「本次全部 parts」清理旧视频，再做去重添加。
        // 反过来的话，已存在的分集会被去重过滤掉、随后又被 drop 删走，而且不会再
        // 加回来，列表就从 N 集塌成 1 集（合集的每个分集是不同 BV，最容易被误删）。
        // Order matters: prune against the whole part list first, then de-duplicate and
        // add. The other way round, already-present episodes were filtered out by
        // dedupe, then deleted by the prune, and never re-added - an N-episode list
        // collapsed to one row (a collection is many BVs, so this hit hardest).
        QStringList keepPaths;
        keepPaths.reserve(parts.size() + 1);
        keepPaths << currentUrl;
        for (const auto& part : parts) {
            if (!part.url.trimmed().isEmpty()) keepPaths << part.url;
        }
        const int before = impl_->playlist->count();
        impl_->playlist->retainSiteVideos(keepPaths);
        const int dropped = before - impl_->playlist->count();

        QStringList urls;
        QHash<QString, QString> names;
        for (const auto& part : parts) {
            if (part.url.trimmed().isEmpty()) continue;
            if (impl_->playlist->indexOfPath(part.url) >= 0) continue;
            urls << part.url;
            if (!part.title.trimmed().isEmpty()) names.insert(part.url, part.title.trimmed());
        }
        if (!urls.isEmpty()) impl_->playlist->addPathsWithNames(urls, names);
        veyra::log::info("bilibili", std::format(
            "queued {} part(s) from the page DOM (titles included), dropped {} old page(s)",
            urls.size(), dropped));
        // Highlight the part that is actually playing. addPathsWithNames() only queues
        // a job, so the row does not exist yet on the first pass; and when the video is
        // already queued (playing P10 after P4) there is nothing to add at all. Both
        // cases need the retry below: it stops as soon as the row can be found.
        auto* playlist = impl_->playlist;
        auto attempts = std::make_shared<int>(0);
        // 用 weak_ptr 打破自引用环：lambda 需要能重新排定自己，但如果它按值持有自己
        // 的 shared_ptr，引用计数永远归零不了（重试结束后整份闭包 + URL 都泄漏）。
        // weak_ptr 让重试链在放弃时自然释放。
        // Break the self-reference: the lambda must be able to re-arm itself, but holding
        // its own shared_ptr by value keeps the refcount above zero forever - the closure
        // and its URL leaked after every send. A weak_ptr lets the chain die on give-up.
        auto highlight = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakHighlight = highlight;
        *highlight = [this, playlist, currentUrl, attempts, weakHighlight]() {
            // Match by what identifies the video, not by exact string: the address the
            // extension sent may carry tracking parameters that the queued entries do
            // not, and query-item order is not stable. setPlayback then needs the
            // address actually stored in the row, not the one that was searched for.
            const int row = playlist->indexOfSitePage(currentUrl);
            if (row >= 0) {
                playlist->setPlayback(playlist->pathAtRow(row), QStringLiteral("playing"));
                return;
            }
            if (++*attempts >= 60) return;   // ~3 s; give up quietly
            QTimer::singleShot(50, this, [this, weakHighlight]() {
                if (const auto alive = weakHighlight.lock()) (*alive)();
            });
        };
        // The first pass runs immediately: on the already-queued path (sending another
        // episode of the same collection) there is nothing to add, the row already
        // exists, and waiting 50 ms only gave tickPlaylist() time to clear the state.
        (*highlight)();
    });

}
void QmlPlayerBridge::queueBilibiliParts(const std::vector<std::wstring>& partTitles,
                                         int currentPart, const QString& pageAddress) {
    if (partTitles.size() <= 1 || pageAddress.isEmpty()) return;
    const QUrl base(pageAddress, QUrl::StrictMode);
    if (!base.isValid()) return;
    // Rebuild each part's address from the page that was opened, so the resolver runs
    // again per entry and the CDN signature (which expires) is never stored.
    QStringList urls;
    urls.reserve(int(partTitles.size()));
    for (size_t k = 0; k < partTitles.size(); ++k) {
        QUrl part = base;
        QUrlQuery query(base);
        query.removeAllQueryItems(QStringLiteral("p"));
        query.addQueryItem(QStringLiteral("p"), QString::number(k + 1));
        part.setQuery(query);
        urls << part.toString(QUrl::FullyEncoded);
    }
    const int current = std::clamp(currentPart, 1, int(partTitles.size()));
    // Deferred deliberately. This runs inside the open that openPath() just started,
    // and inserting rows makes the QML playlist view rebuild; doing that while the
    // native video window is being re-created is what crashed in Qt6Qml. One
    // event-loop turn later the open has settled and the rebuild is safe.
    QTimer::singleShot(0, this, [this, urls, current]() {
        // 与 DOM 路径同样的顺序：先按本次全部分集清理，再去重添加。
        // Same order as the DOM path: prune against the whole part list, then dedupe.
        impl_->playlist->retainSiteVideos(urls);
        QStringList fresh;
        for (const auto& url : urls) {
            if (url.trimmed().isEmpty()) continue;
            if (impl_->playlist->indexOfPath(url) >= 0) continue;
            fresh << url;
        }
        if (!fresh.isEmpty()) impl_->playlist->addPaths(fresh);
        // Mark the part that is actually playing, so auto-advance continues from
        // there instead of jumping back to page 1.
        impl_->playlist->setPlayback(urls.value(current - 1), QStringLiteral("playing"));
        veyra::log::info("bilibili", std::format(
            "queued {} part(s) into the playlist (current=P{})", fresh.size(), current));
    });
}

// Danmaku and CC subtitles for the part that just opened. A failed request stays
// silent by design: the video plays either way, and the user asked for these
// overlays, not for error dialogs about them.
void QmlPlayerBridge::applyBilibiliExtras(const BilibiliBundle& bundle) {
    if (!bundle.extrasError.empty())
        veyra::log::warn("bilibili", QString::fromStdWString(bundle.extrasError).toStdString());

    // Danmaku: build the overlay on first use, then just replace its script.
    if (bundle.wantDanmaku && !bundle.danmaku.empty()) {
        if (!impl_->danmakuWindow) impl_->danmakuWindow = std::make_unique<media::DanmakuWindow>();
        if (!impl_->danmakuWindow->valid() && impl_->videoWindow) {
            if (!impl_->danmakuWindow->create(impl_->videoWindow)) {
                veyra::log::warn("danmaku", "overlay creation failed");
                impl_->danmakuWindow.reset();
            }
        }
        if (impl_->danmakuWindow) {
            std::vector<media::DanmakuItem> items;
            items.reserve(bundle.danmaku.size());
            // BiliDanmaku and DanmakuItem are the same shape; copy field by field
            // rather than teaching the engine type about the site's response.
            for (const auto& one : bundle.danmaku)
                items.push_back({one.time, one.mode, one.color, one.text});
            veyra::log::info("danmaku", std::format("loaded items={}", items.size()));
            impl_->danmakuWindow->setItems(std::move(items));
            impl_->danmakuEnabled = true;
            impl_->danmakuLastSeconds = -1.0;
        }
    } else if (impl_->danmakuWindow) {
        impl_->danmakuWindow->clearItems();
        impl_->danmakuWindow->hide();
        impl_->danmakuEnabled = false;
    }

    if (bundle.wantSubtitles && !bundle.cues.empty()) {
        // The site's own display parameters, kept as the user asked; the styles are
        // applied through the track's SubtitleStyle the same way an ASS file would.
        installBiliSubtitle(bundle);
    }
}

// Installs the site's CC track as an external subtitle track. The subtitles are
// written as SRT and handed to the existing loader rather than built in memory:
// the engine's parser owns global state and must run on this thread, and going
// through the loader keeps language detection, indexing and the offset UI working.
void QmlPlayerBridge::installBiliSubtitle(const BilibiliBundle& bundle) {
    auto& i = *impl_;
    auto stamp = [](double seconds) {
        const long long ms = std::llround(seconds * 1000.0);
        return QStringLiteral("%1:%2:%3,%4")
            .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
            .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'))
            .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
            .arg(ms % 1000, 3, 10, QLatin1Char('0'));
    };
    QString srt;
    for (size_t k = 0; k < bundle.cues.size(); ++k)
        srt += QStringLiteral("%1\n%2 --> %3\n%4\n\n")
                   .arg(k + 1).arg(stamp(bundle.cues[k].from), stamp(bundle.cues[k].to))
                   .arg(QString::fromStdWString(bundle.cues[k].content));

    const QString dir = QDir(QString::fromStdWString(i.dataDir.wstring())).filePath(QStringLiteral("subs"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("bili-%1.srt").arg(bundle.stream.cid));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        veyra::log::warn("subtitle", "bilibili subtitle: cannot write the SRT file");
        return;
    }
    file.write(srt.toUtf8());
    file.close();

    engine::SubtitleTrack track = engine::loadSubtitleFile(path.toStdWString());
    if (!track.usable()) {
        veyra::log::warn("subtitle", "bilibili subtitle: the SRT did not parse");
        return;
    }
    track.name = L"B 站字幕";
    track.language = bundle.subtitleLanguage;

    // The site's own display parameters, kept as asked. Only the fields the
    // existing style actually carries are mapped: colour, background+alpha,
    // alignment and whether there is an outline.
    const auto& bcc = bundle.subtitleStyle;
    if (bcc.present) {
        auto rgb = [](const std::wstring& hex, uint32_t fallback) -> uint32_t {
            if (hex.size() >= 7 && hex[0] == L'#') {
                unsigned v = 0;
                if (swscanf_s(hex.c_str() + 1, L"%06x", &v) == 1) return 0xFF000000u | (v & 0xFFFFFFu);
            }
            return fallback;
        };
        const uint32_t background = rgb(bcc.backgroundColor, 0x00000000u) & 0xFFFFFFu;
        const uint32_t alpha = uint32_t(std::clamp(bcc.backgroundAlpha, 0.0, 1.0) * 255.0 + 0.5);
        engine::SubtitleStyle style = track.styles.empty() ? engine::SubtitleStyle{} : track.styles.front();
        style.primary = rgb(bcc.fontColor, 0xFFFFFFFFu);
        style.back = (alpha << 24) | background;
        style.background = alpha > 0;
        style.alignment = (bcc.location >= 1 && bcc.location <= 9) ? bcc.location : 2;
        if (!bcc.stroke.empty() && bcc.stroke != L"none") style.outlineWidth = std::max(style.outlineWidth, 1.0);
        if (track.styles.empty()) track.styles.push_back(style);
        else track.styles.front() = style;
    }

    i.subTracks.insert(i.subTracks.begin(), std::move(track));
    ++i.subExternal;
    i.subPrimary = 0;
    i.subPrimaryChosen = true;   // the user asked for this track by switching it on
    i.subStatus = tr("B 站字幕（%1）").arg(QString::fromStdWString(bundle.subtitleLanguage));
    veyra::log::info("subtitle", std::format("bilibili subtitle installed lan={} cues={}",
        QString::fromStdWString(bundle.subtitleLanguage).toStdString(), bundle.cues.size()));
    emit subtitlesChanged();
}

// Renders one danmaku frame. Called from the poll loop while a video plays.
void QmlPlayerBridge::tickDanmaku() {
    auto& i = *impl_;
    if (!i.danmakuEnabled || !i.danmakuWindow) return;
    const auto& s = i.snapshot;
    if (!s.running || s.image || s.capture || s.duration <= 0) {
        i.danmakuWindow->hide();
        return;
    }
    HWND video = i.videoWindow;
    if (!video) { i.danmakuWindow->hide(); return; }
    // A jump means the user seeked: the tracks must be rebuilt or comments would
    // arrive from where the old position left them.
    if (i.danmakuLastSeconds >= 0 &&
        (s.position < i.danmakuLastSeconds - 0.5 || s.position > i.danmakuLastSeconds + 2.0))
        i.danmakuWindow->noteSeek();
    i.danmakuLastSeconds = s.position;
    i.danmakuWindow->follow(video);
    media::DanmakuView view;
    view.scale = double(i.prefInt("danmakuScale", 100)) / 100.0;
    view.opacity = double(i.prefInt("danmakuOpacity", 80)) / 100.0;
    view.speedFactor = double(i.prefInt("danmakuSpeed", 100)) / 100.0;
    view.area = double(i.prefInt("danmakuArea", 50)) / 100.0;
    view.videoWidth = int(s.sourceWidth);
    view.videoHeight = int(s.sourceHeight);
    i.danmakuWindow->render(s.position, view);
}

void QmlPlayerBridge::bringToFront() {
    // Windows 只允许前台进程直接 SetForegroundWindow。点击发生在浏览器里，
    // Veyra 是后台进程，所以第一次调用会被拒绝（任务栏闪烁）。
    // 依次尝试：直接调用 -> 挂到前台线程 -> TOPMOST 切换强制改 Z 序。
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        if (!w->isVisible() || w->flags().testFlag(Qt::Tool)) continue;
        const auto hwnd = reinterpret_cast<HWND>(w->winId());
        if (hwnd == nullptr) continue;

        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        w->show();
        w->raise();
        w->requestActivate();
        if (SetForegroundWindow(hwnd)) {
            log::info("playback-server", "window brought to front");
            break;
        }

        const HWND fg = GetForegroundWindow();
        const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const DWORD selfThread = GetCurrentThreadId();
        if (fgThread != 0 && fgThread != selfThread) {
            AttachThreadInput(selfThread, fgThread, TRUE);
            SetForegroundWindow(hwnd);
            AttachThreadInput(selfThread, fgThread, FALSE);
        }
        if (GetForegroundWindow() == hwnd) {
            log::info("playback-server", "window brought to front");
            break;
        }

        // 系统仍拒绝：短暂置顶再取消，强制改变 Z 序后再抢一次前台。
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        if (GetForegroundWindow() == hwnd) {
            log::info("playback-server", "window brought to front (topmost)");
        } else {
            log::warn("playback-server", "window could not be activated; flashing instead");
            FlashWindow(hwnd, TRUE);
        }
        break;
    }
}

void QmlPlayerBridge::togglePlayPause() {
    // AppShell's Play button: a paused session is still "running" (the worker
    // stays alive), so the decision must come from the transport state.
    const auto& s = impl_->snapshot;
    using T = engine::TransportState;
    if (s.image || s.transport == T::Opening || s.transport == T::Stopping) return;
    if(impl_->liveKind==QLatin1String("url")&&(s.transport==T::Ended||
       (s.transport==T::Paused&&s.duration<=0&&!s.network.timeshiftEnabled))){
        openNetworkAddress(QString::fromStdWString(impl_->sourceLabel));return;
    }
    // Capture cards and streams pause too (field request 2026-10-02): the engine stops
    // reading and enhancing, keeps the last picture and resets the source's queue, so
    // play resumes with the newest frame rather than a backlog. The connection stays up.
    if (s.capture && s.running) {
        const bool pause = s.transport == T::Playing;
        impl_->engine.pause(pause);
        veyra::log::info("ui-transport", pause ? "live pause" : "live resume");
        emit notice(pause ? tr("已暂停：画面停在当前帧，不再处理新画面；点播放继续") : tr("已继续"), false);
        return;
    }
    if (!s.running || s.transport == T::Ended) {
        // Only a real file is reopened; device/stream labels are not paths.
        const QString file = QString::fromStdWString(impl_->sourceLabel);
        if (!file.isEmpty() && QFileInfo(file).isFile()) openPath(file);
        return;
    }
    const bool pause = s.transport == T::Playing;
    impl_->engine.pause(pause);
    veyra::log::info("ui-transport", pause ? "pause" : "resume");
}
void QmlPlayerBridge::stopPlayback() {
    impl_->screenCropPreview = false;
    flushHdrMemory();
    impl_->playlistSession = 0;
    impl_->playlist->setPlayback(impl_->playlistPath, QStringLiteral("stopped"));
    rememberPosition(true);
    ++impl_->liveOpenGen; impl_->pendingLiveText.clear();
    impl_->openingSource = false;
    impl_->openingSessionId = 0;
    impl_->engine.stop();
    stopPs5Control();
}
void QmlPlayerBridge::seekTo(double seconds) {
    if(!timelineSeekable())return;
    liveTranslation_->setPlayback(impl_->snapshot.sessionId, false, true);
    impl_->engine.seek(seconds);
}
void QmlPlayerBridge::seekBy(double seconds) {
    seekTo(std::max(0.0, impl_->snapshot.position + seconds));
}
void QmlPlayerBridge::openUrl(const QUrl& url) {
    if(!url.isLocalFile()){openNetworkAddress(url.toString(QUrl::FullyEncoded));return;}
    if (!url.isLocalFile()) {
        emit notice(tr("只能打开本地文件"), true);
        return;
    }
    // Opened inside the drop, as AppShell's WM_DROPFILES does.
    veyra::log::info("ui-drop", url.toLocalFile().toStdString());
    openPath(QDir::toNativeSeparators(url.toLocalFile()));
}
void QmlPlayerBridge::logUi(const QString& channel, const QString& text) {
    const std::string c = channel.toStdString();
    veyra::log::info(c.c_str(), text.toStdString());
}
void QmlPlayerBridge::setWindowMask(QObject* window, const QRectF& rect) {
    auto* w = qobject_cast<QWindow*>(window);
    if (!w) return;
    w->setMask(QRegion(rect.toAlignedRect()));
}
void QmlPlayerBridge::holdOriginal(bool held) {
    // Releasing the key returns to whatever the 显示 page chose (off or split).
    if (held) impl_->engine.comparison(1, false);
    else impl_->engine.comparison(impl_->compareMode, impl_->compareBase, float(impl_->compareSplit));
    veyra::log::info("ui-compare", held ? "hold original on" : "hold original off");
}
void QmlPlayerBridge::stepFrame(int direction) {
    // A frame step is a small seek. The engine owns the real frame duration, so
    // this is honest about being approximate rather than pretending to be exact.
    if (!direction) return;
    impl_->engine.seek(std::max(0.0, impl_->snapshot.position + (direction > 0 ? 1.0 / 60.0 : -1.0 / 60.0)));
}

void QmlPlayerBridge::takeScreenshot() {
    if (!hasSource()) { emit notice(tr("没有正在播放的画面，无法截图"), true); return; }
    if(!impl_->pendingScreenshot.isEmpty()||!impl_->pendingFrameExport.isEmpty()||imageBatchActive()){emit notice(tr("请等待当前图片导出完成"),true);return;}
    const QString folder = screenshotDirectory();
    if (!QDir().mkpath(folder)) { emit notice(tr("截图目录不可用：%1").arg(folder), true); return; }
    const QString name = QStringLiteral("veyra-%1.png")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    const QString path = QDir(folder).filePath(name);
    impl_->engine.saveFrame(wideOf(QDir::toNativeSeparators(path)));
    impl_->pendingScreenshot=path;
    impl_->pendingScreenshotDeadline=QDateTime::currentMSecsSinceEpoch()+30000;
    impl_->lastScreenshot.clear();
    veyra::log::info("ui-screenshot", "requested path=" + path.toStdString());
    emit preferencesChanged();
}

// ---- network / server playback (#48) ------------------------------------------------
bool QmlPlayerBridge::networkActive() const { return impl_->snapshot.network.active; }
bool QmlPlayerBridge::networkHolding() const {
    const auto& n = impl_->snapshot.network;
    return n.active && (n.holding() || n.state == media::NetworkStats::State::Reconnecting);
}
QString QmlPlayerBridge::networkSummary() const {
    const auto& n = impl_->snapshot.network;
    if (!n.active) return {};
    QStringList parts;
    parts << tr("缓冲 %1 s").arg(n.bufferedSeconds, 0, 'f', 1);
    parts << (n.downloadBytesPerSecond > 0 ? tr("下载 %1 MB/s").arg(n.downloadBytesPerSecond / 1048576.0, 0, 'f', 1)
                                           : tr("下载 —"));
    parts << tr("重连 %1").arg(n.reconnects);
    if (const uint64_t lost = std::max(n.corruptPackets, n.corruptFrames); lost > 0) parts << tr("丢包 %1").arg(qulonglong(lost));
    return parts.join(QStringLiteral(" · "));
}
QString QmlPlayerBridge::networkNotice() const {
    const auto& n = impl_->snapshot.network;
    return n.active ? uiText(n.message) : QString();
}


} // namespace veyra::ui
