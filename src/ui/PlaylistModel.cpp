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
namespace {
// B 站视频页的识别。只有这类地址才做「视频 + 分P」归一化：别的网络地址（直链、rtsp、
// udp）的 query/端口/协议都是有意义的，把它们也压成 host+path 会让两个不同的流共用
// 一个键，去重时静默丢掉后加的那条。
// Identifies a Bilibili video page. Only these get the "video + part" normalisation:
// for any other network address the query, port and scheme are meaningful, and
// collapsing them to host+path would make two different streams share one key and be
// silently dropped as duplicates.
bool isSiteVideoPage(const QUrl& url) {
    if (!url.isValid()) return false;
    const auto host = url.host().toLower();
    if (host != QLatin1String("bilibili.com") && !host.endsWith(QLatin1String(".bilibili.com")) &&
        host != QLatin1String("b23.tv"))
        return false;
    return url.path().contains(QLatin1String("/video/"));
}
// 去掉尾斜杠：B 站同一页有 /video/BVxxx 和 /video/BVxxx/ 两种写法。
// Strip trailing slashes: Bilibili serves one page under both spellings.
QString trimmedPath(const QUrl& url) {
    QString p = url.path();
    while (p.endsWith(QLatin1Char('/'))) p.chop(1);
    return p;
}
// 分P 号；没有 p 参数按第 1 分P（B 站第一集的地址通常不带 p）。
// The part number; an absent p means part 1 (Bilibili omits it for the first episode).
QString partOf(const QUrl& url) {
    const auto value = QUrlQuery(url).queryItemValue(QStringLiteral("p")).trimmed();
    return value.isEmpty() ? QStringLiteral("1") : value;
}
// 站点条目没有名字时的回退：BV 号 → host → 加 (P N)。scan() 和 load() 共用一份，
// 否则两处会慢慢长成不一样的规则（列表里同一集显示两个名字）。
// Fallback name for a site entry: BV id -> host -> add "(P N)". Shared by scan() and
// load() so the two cannot drift into different rules for the same episode.
QString fallbackSiteName(const QString& path) {
    const QUrl url(path);
    QString name = url.fileName();
    if (name.isEmpty()) name = url.host();
    const auto part = QUrlQuery(url).queryItemValue(QStringLiteral("p"));
    if (!part.isEmpty()) name += QStringLiteral(" (P%1)").arg(part);
    return name;
}
} // namespace

QString PlaylistModel::keyOf(const QString& path) {
    auto value = normalizedPath(path);
    if (isNetworkPath(value)) {
        const QUrl url(value, QUrl::StrictMode);
        if (isSiteVideoPage(url)) {
            // host 统一小写；path 与分P 保持原样——BV 号区分大小写，折叠会把两个不同
            // 视频并成一条。尾斜杠与会变的跟踪参数不参与身份。
            // Lower-case the host only. The path and the part keep their case: a BV id is
            // case-sensitive, and folding it merged two different videos into one key.
            // The trailing slash and the volatile tracking parameters are not identity.
            return url.host().toLower() + trimmedPath(url) + QLatin1Char('#') + partOf(url);
        }
        // 其它网络地址按原样（规范化 scheme/host 大小写即可）。
        // Other network addresses keep their full form.
        return value;
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
        if (!info.isFile() || !supported(info.filePath()) || reparse(info)) return;
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
            if (name.isEmpty()) name = fallbackSiteName(path);
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
    // 站点页面有自己唯一正确的顺序（P1、P2… / 合集的分集顺序），调用方就是按那个
    // 顺序发来的；按名字重排会把它打乱（「第二集」排在「第一集」前面）。所以只对本地
    // 文件排序，站点条目原样保留——混在一批里时两部分各自有序。
    // A site page has exactly one correct order (P1, P2 ... / the collection's episode
    // order) and the caller sends it that way; a name sort scrambles it ("第二集" sorts
    // before "第一集"). Sort the local files only and leave site entries in place.
    if (!cancelled()) {
        std::vector<Entry> files, pages;
        for (auto& e : result.entries) (isNetworkPath(e.path) ? pages : files).push_back(std::move(e));
        // 注意：上面把元素都 move 走了，所以必须无条件把两者拼回去，任何一条分支漏掉
        // 都会让 result.entries 变成空壳（曾经只有站点页面时整批条目被丢掉）。
        // The loop above moved every element out, so they must always be written back;
        // a branch that forgets leaves result.entries empty (an all-site batch was lost).
        order(files, NameAscending);   // 站点条目本来就是正确顺序，只排文件
        files.insert(files.end(), std::make_move_iterator(pages.begin()),
                     std::make_move_iterator(pages.end()));
        result.entries = std::move(files);
    }
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
        // 用户在名称/大小排序下添加文件时保持排序，但站点条目必须按发布顺序留着：
        // 整表重排会把 P1/P2… 和合集分集顺序打乱，auto-advance 的「下一集」也就错了。
        // Keep the user's sort for files, but never re-sort site entries: a full re-sort
        // scrambles P1/P2... and the collection order, which also breaks "next episode".
        if (sort_ != Manual) {
            const bool anySite = std::any_of(entries_.begin(), entries_.end(),
                [this](const Entry& e) { return isNetworkPath(e.path); });
            if (!anySite) sortBy(sort_);
        }
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
    // 直接复用 keyOf 的归一化：它已经按 host + 去尾斜杠的 path + 分P 生成键，忽略
    // 跟踪参数、参数顺序和 BV 号大小写之外的差异。三处各写一套比较是这个文件里
    // 反复出错的来源（曾经 keyOf 认为相同、indexOfSitePage 认为不同）。
    // Reuse keyOf's normalisation instead of a second comparison: it already keys on
    // host + slash-trimmed path + part, ignoring tracking parameters and parameter
    // order. Three separate implementations of "same page" is what kept breaking here -
    // keyOf would call two addresses equal while indexOfSitePage called them different.
    const QUrl wanted(path, QUrl::StrictMode);
    if (!isSiteVideoPage(wanted)) return -1;
    const auto wantedKey = keyOf(path);
    for (int i = 0; i < count(); ++i) {
        if (entries_[i].key == wantedKey) return i;
    }
    return -1;
}
void PlaylistModel::retainSiteVideos(const QStringList& keepPaths) {
    // 按「视频」而不是「分P」保留：合集里同一视频的多集要一起留下，所以键里不带分P。
    // 本地文件不在处理范围内。
    // Keep by *video*, not by part: every episode of one video in the keep set has to
    // survive, so the part is deliberately not part of this key. Local files untouched.
    QSet<QString> keepVideos;
    for (const auto& path : keepPaths) {
        const QUrl url(path, QUrl::StrictMode);
        if (isSiteVideoPage(url)) keepVideos.insert(url.host().toLower() + trimmedPath(url));
    }
    if (keepVideos.isEmpty()) return;
    std::vector<int> doomed;
    for (int i = 0; i < count(); ++i) {
        const auto& entry = entries_[i];
        if (!isNetworkPath(entry.path)) continue;          // local file: keep
        const QUrl candidate(entry.path, QUrl::StrictMode);
        if (!isSiteVideoPage(candidate)) continue;         // not a site page: keep
        const auto video = candidate.host().toLower() + trimmedPath(candidate);
        if (!keepVideos.contains(video)) doomed.push_back(i);
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
            if (name.isEmpty()) name = fallbackSiteName(normalized);
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
