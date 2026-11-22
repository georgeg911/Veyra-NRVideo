#include "veyra/ui/PlaylistModel.h"
#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <utility>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace veyra::ui {
namespace {
constexpr int maxEntries = 10000, maxJobs = 16, maxVisited = 100000;
bool reparse(const QFileInfo& info) {
    if (info.isSymLink()) return true;
#ifdef Q_OS_WIN
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(info.absoluteFilePath().utf16()));
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}
}

PlaylistModel::PlaylistModel(QString storage, QObject* parent)
    : QAbstractListModel(parent), storage_(std::move(storage)) {
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(350);
    connect(&saveTimer_, &QTimer::timeout, this, [this] { save(); });
    load();
    worker_ = std::jthread([this](std::stop_token stop) { workerLoop(stop); });
}
PlaylistModel::~PlaylistModel() {
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (saveTimer_.isActive()) save();
}
int PlaylistModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }
bool PlaylistModel::isNetworkPath(const QString& path) {
    // Anything with a scheme the engine can open. Checked before any QFileInfo/QDir
    // call, because those mangle a URL: cleanPath turns "https://host/a?p=2" into
    // "https:/host/a?p=2", and the entry could never be reopened.
    const int colon = path.indexOf(QLatin1Char(':'));
    if (colon <= 1 || colon > 8) return false;   // "C:\..." has the colon at 1
    static const QStringList schemes{
        QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("rtsp"),
        QStringLiteral("rtp"), QStringLiteral("udp"), QStringLiteral("rtmp")};
    return schemes.contains(path.left(colon).toLower());
}
QString PlaylistModel::normalizedPath(const QString& path) {
    if (isNetworkPath(path)) return path;   // addresses are stored verbatim
    return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());
}
QString PlaylistModel::keyOf(const QString& path) {
    auto value = normalizedPath(path);
    // A site page is identified by the video and the part, not by the exact string: the
    // browser appends tracking parameters (vd_source, spm_id_from, trackid) and they are
    // not stable, so the same episode could be added twice as two different rows. A page
    // with no p at all is part 1 (the Bilibili URL for the first part usually omits it).
    if (isNetworkPath(value)) {
        const QUrl url(value, QUrl::StrictMode);
        if (url.isValid() && !url.host().isEmpty()) {
            auto part = QUrlQuery(url).queryItemValue(QStringLiteral("p")).trimmed();
            if (part.isEmpty()) part = QStringLiteral("1");
            // Bilibili uses both "/video/BVxxxx" and "/video/BVxxxx/" for one page, so
            // the trailing slash cannot be part of the identity either.
            QString pagePath = url.path();
            while (pagePath.endsWith(QLatin1Char('/'))) pagePath.chop(1);
            auto key = url.host().toLower() + pagePath + QLatin1Char('#') + part;
#ifdef Q_OS_WIN
            key = key.toCaseFolded();
#endif
            return key;
        }
    }
#ifdef Q_OS_WIN
    value = value.toCaseFolded();
#endif
    return value;
}
bool PlaylistModel::supported(const QString& path) {
    static const QSet<QString> suffixes{
        "mp4", "mkv", "mov", "avi", "webm", "ts", "m2ts", "mts", "m4v", "wmv",
        "mpg", "mpeg", "vob", "flv", "ogv", "asf", "3gp", "jpg", "jpeg", "png", "bmp", "webp"
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}
QVariant PlaylistModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto& entry = entries_[index.row()];
    const bool network = isNetworkPath(entry.path);
    switch (role) {
    case Qt::DisplayRole: case NameRole: return entry.name;
    // 网络地址不能过 toNativeSeparators：Windows 上它会把 // 变成 \\，QML 拿这个
    // 变形串去 indexOfPath 就再也对不上，选中后的回车播放 / 删除 / 拖动排序全部失效。
    // A URL must not go through toNativeSeparators: on Windows it turns the slashes into
    // backslashes, and QML then feeds that mangled string to indexOfPath - which no longer
    // matches, so select-then-play / delete / drag-reorder silently stop working.
    case PathRole: return network ? entry.path : QDir::toNativeSeparators(entry.path);
    // A site page has no file size and no modified time; showing "0 字节 · 1970-01-01"
    // reads like a broken file. Say what it actually is.
    case SizeRole: return network ? tr("网络视频") : QLocale().formattedDataSize(entry.size);
    case ModifiedRole: return network ? QString() : QDateTime::fromMSecsSinceEpoch(entry.modified).toString("yyyy-MM-dd HH:mm");
    case CurrentRole: return entry.key == currentKey_;
    case StateRole: return entry.key == currentKey_ ? state_ : QString();
    default: return {};
    }
}
QHash<int, QByteArray> PlaylistModel::roleNames() const {
    return {{NameRole, "entryName"}, {PathRole, "entryPath"}, {SizeRole, "entrySize"},
            {ModifiedRole, "entryModified"}, {CurrentRole, "entryCurrent"}, {StateRole, "entryState"}};
}
int PlaylistModel::currentIndex() const {
    if (!currentKey_.isEmpty())
        for (int i = 0; i < count(); ++i) if (entries_[i].key == currentKey_) return i;
    return -1;
}
void PlaylistModel::setPlayback(const QString& path, const QString& state) {
    const QString key = path.isEmpty() ? QString() : keyOf(path);
    if (key == currentKey_ && state == state_) return;
    const int before = currentIndex();
    currentKey_ = key; state_ = state;
    const int after = currentIndex();
    if (before >= 0) emit dataChanged(index(before), index(before), {CurrentRole, StateRole});
    if (after >= 0 && after != before) emit dataChanged(index(after), index(after), {CurrentRole, StateRole});
    emit currentChanged();
}
void PlaylistModel::setAutoAdvance(bool enabled) {
    if (autoAdvance_ == enabled) return;
    autoAdvance_ = enabled; emit autoAdvanceChanged(); saveTimer_.start();
}
void PlaylistModel::setStatus(QString text) {
    if (status_ == text) return;
    status_ = std::move(text); emit statusChanged();
}
void PlaylistModel::addUrls(const QList<QUrl>& urls) {
    QStringList paths;
    for (const auto& url : urls) {
        if (url.isLocalFile()) paths << url.toLocalFile();
    }
    addPaths(paths);
}
void PlaylistModel::addPaths(const QStringList& paths) {
    addPathsWithNames(paths, {});
}
void PlaylistModel::addPathsWithNames(const QStringList& paths, const QHash<QString, QString>& names) {
    if (paths.isEmpty()) return;
    if (pending_ >= maxJobs || count() >= maxEntries) {
        emit error(tr("列表或导入任务已达上限，请稍后重试或移除部分条目")); return;
    }
    // No directory/file existence checks here: a network folder can be slow.
    Job job; job.generation = generation_.load();
    for (const auto& path : paths) {
        if (!path.trimmed().isEmpty() && job.paths.size() < maxEntries) {
            const auto normalized = normalizedPath(path);
            job.paths << normalized;
            // The caller's name wins over the one derived from the path.
            const auto it = names.constFind(path);
            if (it != names.constEnd() && !it->trimmed().isEmpty()) job.names.insert(normalized, *it);
        }
    }
    if (job.paths.isEmpty()) return;
    const bool wasBusy = busy(); ++pending_;
    { std::lock_guard lock(mutex_); jobs_.push_back(std::move(job)); }
    if (!wasBusy) emit busyChanged();
    setStatus(tr("正在扫描文件夹…"));
    wake_.notify_one();
}
void PlaylistModel::order(std::vector<Entry>& entries, int mode) {
    QCollator collator; collator.setNumericMode(true); collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        if ((mode == NewestFirst || mode == OldestFirst) && a.modified != b.modified)
            return mode == NewestFirst ? a.modified > b.modified : a.modified < b.modified;
        if ((mode == LargestFirst || mode == SmallestFirst) && a.size != b.size)
            return mode == LargestFirst ? a.size > b.size : a.size < b.size;
        const int compared = collator.compare(a.name, b.name);
        if (compared) return mode == NameDescending ? compared > 0 : compared < 0;
        return a.key < b.key;
    });
}
PlaylistModel::Result PlaylistModel::scan(const Job& job, std::stop_token stop) const {
    Result result;
    QSet<QString> visited, found;
    QStringList directories;
    int examined = 0;
    auto cancelled = [&] { return stop.stop_requested() || job.generation != generation_.load(); };
    auto file = [&](const QFileInfo& info) {
        if (!info.isFile() || !supported(info.filePath()) || reparse(info)) { ++result.skipped; return; }
        const auto path = normalizedPath(info.absoluteFilePath()), key = keyOf(path);
        if (found.contains(key)) return;
        found.insert(key);
        result.entries.push_back({path, info.fileName(), key, info.size(), info.lastModified().toMSecsSinceEpoch()});
    };
    for (const auto& path : job.paths) {
        if (cancelled()) return {};
        // A network address is an entry in its own right: no file to stat, no suffix
        // to check. Multi-part Bilibili videos contribute one address per part, and
        // the ?p= suffix is what tells two entries of the same video apart.
        if (isNetworkPath(path)) {
            const auto key = keyOf(path);
            if (found.contains(key)) continue;
            found.insert(key);
            // A name supplied by the caller wins: for a site page that is the title
            // the site shows, which the URL alone cannot reproduce.
            QString name = job.names.value(path);
            if (name.isEmpty()) {
                name = QUrl(path).fileName();
                if (name.isEmpty()) name = QUrl(path).host();
                const QString part = QUrlQuery(QUrl(path)).queryItemValue(QStringLiteral("p"));
                if (!part.isEmpty()) name += QStringLiteral(" (P%1)").arg(part);
            }
            result.entries.push_back({path, name, key, 0, 0});
            if (int(result.entries.size()) >= maxEntries) { result.limited = true; break; }
            continue;
        }
        const QFileInfo info(path);
        if (info.isDir() && !reparse(info)) directories << path;
        else file(info);
        if (int(result.entries.size()) >= maxEntries) { result.limited = true; break; }
    }
    while (!directories.isEmpty() && !result.limited && !cancelled()) {
        const auto path = directories.takeLast(), key = keyOf(path);
        if (visited.contains(key)) continue;
        visited.insert(key);
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::NoIteratorFlags);
        while (it.hasNext() && !cancelled()) {
            it.next(); const QFileInfo info = it.fileInfo();
            if (++examined > maxVisited) { result.limited = true; break; }
            if (info.isDir()) {
                if (!reparse(info) && directories.size() < maxEntries) directories << info.absoluteFilePath();
            } else file(info);
            if (int(result.entries.size()) >= maxEntries) { result.limited = true; break; }
        }
    }
    // A site page's list has its own meaningful order (P1, P2, ... / episode order) and
    // the caller sends it in that order; re-sorting by name scrambles it ("二" before
    // "一"). Files keep the existing name sort, which is what makes a folder readable.
    const bool hasSitePages = std::any_of(result.entries.begin(), result.entries.end(),
        [this](const Entry& e) { return isNetworkPath(e.path); });
    if (!cancelled() && !hasSitePages) order(result.entries, NameAscending);
    return result;
}
void PlaylistModel::workerLoop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !jobs_.empty(); })) return;
            job = std::move(jobs_.front()); jobs_.pop_front();
        }
        auto result = scan(job, stop);
        if (stop.stop_requested()) return;
        QMetaObject::invokeMethod(this, [this, result = std::move(result), generation = job.generation]() mutable {
            accept(std::move(result), generation);
        }, Qt::QueuedConnection);
    }
}
void PlaylistModel::accept(Result result, uint64_t generation) {
    if (generation != generation_.load()) return;
    --pending_;
    std::vector<Entry> added;
    for (auto& entry : result.entries) {
        if (keys_.contains(entry.key)) continue;
        if (count() + int(added.size()) >= maxEntries) { result.limited = true; break; }
        keys_.insert(entry.key); added.push_back(std::move(entry));
    }
    if (!added.empty()) {
        beginInsertRows({}, count(), count() + int(added.size()) - 1);
        for (auto& entry : added) entries_.push_back(std::move(entry));
        endInsertRows(); emit countChanged(); emit currentChanged();
        if (sort_ != Manual) sortBy(sort_);
        saveTimer_.start();
    }
    if (!busy()) emit busyChanged();
    setStatus(busy() ? tr("正在扫描文件夹…")
                    : tr("已添加 %1 项 · 列表共 %2 项").arg(added.size()).arg(count()));
    if (result.limited) emit error(tr("已达到导入上限（最多10000项），其余文件未添加"));
    else if (added.empty() && !busy()) setStatus(tr("未添加新条目：文件已在列表中，或没有支持的媒体文件"));
}
QString PlaylistModel::pathAt(int row) const { return row >= 0 && row < count() ? entries_[row].path : QString(); }
int PlaylistModel::indexOfPath(const QString& path) const {
    const auto key = keyOf(path);
    for (int i = 0; i < count(); ++i) if (entries_[i].key == key) return i;
    return -1;
}
int PlaylistModel::indexOfSitePage(const QString& path) const {
    // A site page can be written several ways: the browser may append tracking
    // parameters (spm_id_from, vd_source, trackid), the order of query items is not
    // stable, and Bilibili itself uses both "/video/BVxxxx" and "/video/BVxxxx/" for the
    // same page. Compare what identifies the video instead: host + path (trailing slash
    // ignored) + the part. A page with no p at all is part 1, because the URL for the
    // first part usually omits it while the queued entry says p=1.
    const QUrl wanted(path, QUrl::StrictMode);
    if (!wanted.isValid() || wanted.host().isEmpty()) return -1;
    const auto pathOf = [](const QUrl& url) {
        QString p = url.path();
        while (p.endsWith(QLatin1Char('/'))) p.chop(1);
        return p;
    };
    const auto partOf = [](const QUrl& url) {
        const auto value = QUrlQuery(url).queryItemValue(QStringLiteral("p")).trimmed();
        return value.isEmpty() ? QStringLiteral("1") : value;
    };
    const auto wantedPart = partOf(wanted);
    const auto wantedPath = pathOf(wanted);
    for (int i = 0; i < count(); ++i) {
        const QUrl candidate(entries_[i].path, QUrl::StrictMode);
        if (!candidate.isValid()) continue;
        if (candidate.host().compare(wanted.host(), Qt::CaseInsensitive) != 0) continue;
        if (pathOf(candidate) != wantedPath) continue;
        if (partOf(candidate) == wantedPart) return i;
    }
    return -1;
}
QString PlaylistModel::pathAtRow(int row) const {
    if (row < 0 || row >= count()) return {};
    return entries_[row].path;
}
void PlaylistModel::retainSiteVideos(const QStringList& keepPaths) {
    // 按 host + path 识别「同一个视频」，保留集合由调用方给全（合集是多个 BV）。
    // 本地文件不在处理范围内。尾斜杠不表意：/video/BVxxx 与 /video/BVxxx/ 是同一页。
    // Identify a video by host + path; the caller supplies the whole keep set (a
    // collection is many BVs). Local files are left alone. Trailing slash is not
    // significant - Bilibili uses both spellings for one page.
    const auto pathOf = [](const QUrl& url) {
        QString p = url.path();
        while (p.endsWith(QLatin1Char('/'))) p.chop(1);
        return p;
    };
    QSet<QString> keepKeys;
    for (const auto& path : keepPaths) {
        const QUrl url(path, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty()) continue;
        keepKeys.insert(url.host().toLower() + pathOf(url));
    }
    if (keepKeys.isEmpty()) return;
    std::vector<int> doomed;
    for (int i = 0; i < count(); ++i) {
        const auto& entry = entries_[i];
        if (!isNetworkPath(entry.path)) continue;          // local file: keep
        const QUrl candidate(entry.path, QUrl::StrictMode);
        if (!candidate.isValid()) continue;
        const auto key = candidate.host().toLower() + pathOf(candidate);
        if (!keepKeys.contains(key)) doomed.push_back(i);
    }
    if (doomed.empty()) return;
    // Remove back to front so the earlier indices stay valid.
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
        const int row = *it;
        keys_.remove(entries_[row].key);
        beginRemoveRows({}, row, row);
        entries_.erase(entries_.begin() + row);
        endRemoveRows();
    }
    emit countChanged(); emit currentChanged(); saveTimer_.start();
}
bool PlaylistModel::play(int row) {
    if (row < 0 || row >= count()) return false;
    const QString path = entries_[row].path;
    // A network address has no file to stat; the engine reports a bad one when it
    // tries to open it. Local entries still fail here, which keeps "file moved or
    // deleted" off the engine.
    const bool network = isNetworkPath(path);
    if (!network && !QFileInfo(path).isFile()) {
        emit error(tr("文件不存在或已移动：%1").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    emit playRequested(network ? path : QDir::toNativeSeparators(path));
    return true;
}
bool PlaylistModel::playFrom(int row, int direction) {
    for (; row >= 0 && row < count(); row += direction) {
        // Skip missing entries without opening a failed engine session for each.
        // 网络条目（播放列表里的 B 站分集/直链）没有文件可 stat，交给引擎去判断；
        // 只对本地路径做存在性检查。
        const auto& path = entries_[row].path;
        if (isNetworkPath(path) || QFileInfo(path).isFile()) return play(row);
    }
    return false;
}
bool PlaylistModel::next() { const int at = currentIndex(); return playFrom(at < 0 ? 0 : at + 1, 1); }
bool PlaylistModel::previous() { const int at = currentIndex(); return playFrom(at < 0 ? count() - 1 : at - 1, -1); }
bool PlaylistModel::advanceAfterEnd() { return autoAdvance_ && currentIndex() >= 0 && next(); }
bool PlaylistModel::move(int from, int to) {
    if (from < 0 || to < 0 || from >= count() || to >= count() || from == to) return false;
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    auto entry = std::move(entries_[from]); entries_.erase(entries_.begin() + from);
    entries_.insert(entries_.begin() + to, std::move(entry)); endMoveRows();
    if (sort_ != Manual) { sort_ = Manual; emit sortModeChanged(); }
    emit currentChanged(); saveTimer_.start(); return true;
}
bool PlaylistModel::remove(int row) {
    if (row < 0 || row >= count()) return false;
    beginRemoveRows({}, row, row); keys_.remove(entries_[row].key); entries_.erase(entries_.begin() + row); endRemoveRows();
    emit countChanged(); emit currentChanged(); setStatus({}); saveTimer_.start(); return true;
}
void PlaylistModel::clear() {
    ++generation_;
    { std::lock_guard lock(mutex_); jobs_.clear(); }
    const bool wasBusy = busy(); pending_ = 0;
    beginResetModel(); entries_.clear(); keys_.clear(); endResetModel();
    if (wasBusy) emit busyChanged();
    emit countChanged(); emit currentChanged();
    setStatus({}); saveTimer_.start();
}
void PlaylistModel::sortBy(int mode) {
    if (mode < Manual || mode > SmallestFirst) return;
    if (mode != Manual && count() > 1) {
        // A layout change keeps the identity of any persistent model indices.
        emit layoutAboutToBeChanged();
        const auto indices = persistentIndexList(); QStringList paths;
        for (const auto& item : indices) paths << entries_[item.row()].key;
        order(entries_, mode);
        QModelIndexList replacements;
        for (const auto& key : paths) {
            const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.key == key; });
            replacements << index(int(it - entries_.begin()));
        }
        changePersistentIndexList(indices, replacements); emit layoutChanged(); emit currentChanged();
    }
    if (sort_ != mode) { sort_ = mode; emit sortModeChanged(); }
    saveTimer_.start();
}
bool PlaylistModel::save() {
    saveTimer_.stop();
    if (storage_.isEmpty()) return true;
    if (unreadable_) return false;   // status_ already tells the user the file was not loaded
    QJsonArray array;
    for (const auto& entry : entries_) {
        QJsonObject o{{"path", entry.path}, {"size", double(entry.size)}, {"modified", double(entry.modified)}};
        // Site pages carry a title that exists only in the page (a Bilibili part name),
        // so it has to be persisted: it cannot be recovered from the URL on reload.
        // Local files keep deriving their name from the path as before.
        if (isNetworkPath(entry.path) && !entry.name.isEmpty()) o.insert("name", entry.name);
        array.append(o);
    }
    QSaveFile file(storage_);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"sort", sort_}, {"autoAdvance", autoAdvance_}, {"entries", array}}).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        emit error(tr("播放列表保存失败：%1").arg(file.errorString())); return false;
    }
    return true;
}
void PlaylistModel::load() {
    if (storage_.isEmpty()) return;
    QFile file(storage_);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
        unreadable_ = true; status_ = tr("播放列表文件无法读取，已使用空列表"); return;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    const auto object = doc.object();
    if (error.error != QJsonParseError::NoError || object.value("version").toInt() != 1 || !object.value("entries").isArray()) {
        unreadable_ = true; status_ = tr("播放列表文件损坏，已使用空列表"); return;
    }
    sort_ = std::clamp(object.value("sort").toInt(), int(Manual), int(SmallestFirst));
    autoAdvance_ = object.value("autoAdvance").toBool(true);
    for (const auto& value : object.value("entries").toArray()) {
        const auto entry = value.toObject(); const auto path = entry.value("path").toString();
        if (path.isEmpty()) continue;
        // A saved network address (a multi-part Bilibili video keeps one per part) is
        // not an absolute filesystem path and has no file suffix, so the file checks
        // below would drop it every time the list is reloaded.
        const bool network = isNetworkPath(path);
        if (!network && (!QDir::isAbsolutePath(path) || !supported(path))) continue;
        const auto normalized = normalizedPath(path), key = keyOf(normalized);
        if (keys_.contains(key)) continue;
        keys_.insert(key);
        QString name;
        if (network) {
            // A saved name wins: it is the site's own title, which the URL cannot give.
            name = entry.value("name").toString().trimmed();
            if (name.isEmpty()) {
                name = QUrl(normalized).fileName();
                if (name.isEmpty()) name = QUrl(normalized).host();
                const QString part = QUrlQuery(QUrl(normalized)).queryItemValue(QStringLiteral("p"));
                if (!part.isEmpty()) name += QStringLiteral(" (P%1)").arg(part);
            }
        } else {
            name = QFileInfo(normalized).fileName();
        }
        entries_.push_back({normalized, name, key,
                           std::max(qint64(0), entry.value("size").toInteger()),
                           entry.value("modified").toInteger()});
        if (count() >= maxEntries) break;
    }
}
}
