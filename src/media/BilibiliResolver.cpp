// Bilibili page URL -> playable DASH stream URL.
//
// Implemented on WinHTTP so the player does not gain a dependency for this one
// feature. The two API calls are the same ones the site's own player makes.
#include "veyra/media/BilibiliResolver.h"
#include "veyra/media/BiliJson.h"
#include "veyra/Log.h"
#include "BilibiliHttp.h"
#include <windows.h>
#include <wincrypt.h>   // CryptProtectData: the session cookie is stored encrypted
#include <regex>
#include <string>
#include <format>

#pragma comment(lib, "winhttp.lib")

namespace veyra::media {

// The HTTP plumbing is shared with the danmaku/subtitle module.
using bili::fromUtf8;
using bili::httpFollowLocation;
using bili::httpGet;
using bili::toUtf8;

namespace {

// The DASH manifest carries one object per rendition under "video"/"audio".
// The parsed document is handed back rather than raw substrings so callers can
// read nested fields (id, codecs, baseUrl) without re-scanning text.
const json::Value& dashArray(const json::Value& playRoot, const char* name) {
    return playRoot["data"]["dash"][name];
}

// Bilibili answers code 0 on success and a number otherwise. A number alone is
// useless to the person who just pasted a link, so name the ones a paste can hit
// and keep the raw code for the rest.
std::wstring apiFailure(const wchar_t* what, long long code) {
    switch (code) {
    case -404:
    case -400: return std::format(L"{}失败：视频不存在或已被删除", what);
    case -403: return std::format(L"{}失败：无权访问，可能需要先登录", what);
    case -10403: return std::format(L"{}失败：该视频在当前地区不可播放", what);
    case 62002: return std::format(L"{}失败：稿件不可见（已删除或设为私密）", what);
    case 62004: return std::format(L"{}失败：稿件正在审核", what);
    case 87007:
    case 87008: return std::format(L"{}失败：该画质需要大会员", what);
    default: return std::format(L"{}失败（错误码 {}）", what, code);
    }
}

} // namespace

// b23.tv/<code> carries no BV id: the redirect holds the real page URL.
std::wstring expandShortLink(const std::wstring& url, std::wstring& error) {
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos) { error = L"短链格式无效"; return {}; }
    const size_t hostStart = scheme + 3;
    const size_t slash = url.find(L'/', hostStart);
    const std::wstring host = url.substr(hostStart, slash == std::wstring::npos
                                                    ? std::wstring::npos : slash - hostStart);
    const std::wstring path = slash == std::wstring::npos ? L"/" : url.substr(slash);
    std::wstring location;
    if (!httpFollowLocation(host, path, location, error)) return {};
    return location;
}

bool isBilibiliPageUrl(const std::wstring& url) {
    return url.find(L"bilibili.com/video/") != std::wstring::npos ||
           url.find(L"b23.tv/") != std::wstring::npos;
}

// The cookie lives beside the app's other per-user state.
std::wstring bilibiliCookiePath() {
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"bilibili-cookie.txt";
    std::wstring dir(base);
    dir += L"\\Veyra";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\bilibili-cookie.txt";
}

// The cookie is a session credential: SESSDATA alone lets anyone who can read the
// file act as the account. Every other long-lived credential in this repo (Xbox,
// PSN, PS5 pairing, Moonlight) goes through DPAPI, so this one does too.
// A file written by an older build is plain text with no magic: it is still read,
// so an existing sign-in is not lost. Nothing in this build writes the file; the
// session comes from the browser extension or the legacy file.
constexpr char kCookieMagic[4] = {'V', 'B', 'C', 'K'};

bool readAllBytes(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        if (out.size() + read > (1u << 20)) {   // a cookie is a few hundred bytes
            CloseHandle(file);
            return false;
        }
        out.append(buffer, read);
    }
    CloseHandle(file);
    return true;
}

std::wstring loadBilibiliCookie() {
    const std::wstring path = bilibiliCookiePath();
    std::string bytes;
    if (!readAllBytes(path, bytes) || bytes.empty()) return {};

    std::string plain;
    if (bytes.size() > 4 && std::memcmp(bytes.data(), kCookieMagic, 4) == 0) {
        DATA_BLOB input{DWORD(bytes.size() - 4), reinterpret_cast<BYTE*>(bytes.data() + 4)};
        DATA_BLOB output{};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &output)) {
            log::warn("bilibili", "cookie decrypt failed; treating the account as signed out");
            return {};
        }
        plain.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
    } else {
        plain = std::move(bytes);   // legacy plain-text file
    }
    // Trim the trailing newline an editor may have added.
    while (!plain.empty() && (plain.back() == '\r' || plain.back() == '\n' || plain.back() == ' '))
        plain.pop_back();
    return fromUtf8(plain);
}

bool bilibiliCookieLooksSignedIn(const std::wstring& cookie) {
    return cookie.find(L"SESSDATA=") != std::wstring::npos;
}

BilibiliStream resolveBilibiliStream(const std::wstring& pageUrl, const std::wstring& cookie, int quality) {
    BilibiliStream result;
    std::wstring error;

    // 1. Extract the BV id (or av id) from the page URL. A b23.tv link has no id in
    //    it at all, so follow its redirect first; that is the form the phone app
    //    shares, and without this every shared link failed to resolve.
    std::wstring resolvedUrl = pageUrl;
    if (pageUrl.find(L"b23.tv/") != std::wstring::npos) {
        std::wstring linkError;
        const std::wstring expanded = expandShortLink(pageUrl, linkError);
        if (expanded.empty()) {
            result.error = L"短链解析失败：" + linkError;
            return result;
        }
        resolvedUrl = expanded;
    }
    // One UTF-8 copy of the URL serves the id scan and the ?p= scan.
    const std::string resolvedUtf8 = toUtf8(resolvedUrl);
    std::string bvid, aid;
    {
        std::smatch m;
        const std::regex bvRe(R"((BV[0-9A-Za-z]{10}))");
        if (std::regex_search(resolvedUtf8, m, bvRe)) bvid = m[1].str();
        if (bvid.empty()) {
            const std::regex avRe(R"(/av([0-9]+))");
            if (std::regex_search(resolvedUtf8, m, avRe)) aid = m[1].str();
        }
    }
    if (bvid.empty() && aid.empty()) {
        result.error = L"无法从网址中识别视频编号（需要 BV 号）";
        return result;
    }

    const std::wstring referer = L"https://www.bilibili.com";
    std::string viewBody;
    const std::wstring viewPath = bvid.empty()
        ? std::format(L"/x/web-interface/view?aid={}", fromUtf8(aid))
        : std::format(L"/x/web-interface/view?bvid={}", fromUtf8(bvid));
    if (!httpGet(L"api.bilibili.com", viewPath, referer, cookie, viewBody, error)) {
        result.error = L"查询视频信息失败：" + error;
        return result;
    }
    const auto viewRoot = json::parse(viewBody);
    const long long viewCode = viewRoot["code"].asInt(-1);
    if (viewCode != 0) {
        result.error = apiFailure(L"读取视频信息", viewCode);
        return result;
    }
    // cid lives on the first entry of data.pages; a single-part video also mirrors
    // it at data.cid, but pages[] is the only reliable place for 分P videos.
    const auto& pages = viewRoot["data"]["pages"];
    long long cid = viewRoot["data"]["cid"].asInt(0);
    if (pages.size() > 0) cid = pages[0]["cid"].asInt(cid);
    // 多 P：URL 的 ?p=N 指定第几 P（1 起），每 P 的 cid 在 data.pages 里。
    // 浏览器插件会把带 ?p= 的页面地址原样传过来，所以这里也要认。
    // A ?p= past the last part is refused rather than silently playing part 1:
    // a shared link with a stale part number should say so.
    int pageIndex = 1;
    {
        static const std::regex pRe(R"([?&]p=(\d+))");
        std::smatch pm;
        if (std::regex_search(resolvedUtf8, pm, pRe)) {
            try { pageIndex = std::max(1, std::stoi(pm[1].str())); } catch (...) { pageIndex = 1; }
        }
    }
    if (pageIndex > 1) {
        if (pages.size() < size_t(pageIndex)) {
            result.error = std::format(L"视频只有 {} 个分P，没有第 {} 个", pages.size(), pageIndex);
            return result;
        }
        cid = pages[size_t(pageIndex - 1)]["cid"].asInt(cid);
    }
    if (cid <= 0) {
        result.error = L"视频信息里没有 cid";
        return result;
    }
    const std::string cidText = std::to_string(cid);
    result.title = fromUtf8(viewRoot["data"]["title"].asString());
    // The extras module needs both: danmaku is keyed on cid, the CC subtitle list
    // on aid+cid. The view answer is authoritative for aid in either URL form.
    result.aid = viewRoot["data"]["aid"].asInt(0);
    result.cid = cid;

    // 2. Ask for the DASH manifest. fnval 4048 = DASH | HDR | 4K | Dolby audio |
    //    Dolby Vision | 8K | AV1; fourk=1 keeps 4K in the answer.
    // quality = 0 keeps the old behaviour (ask for the best rendition); a positive
    // value is the page's own qn, for "follow the web player's quality".
    std::string playBody;
    const std::wstring qnText = std::to_wstring(quality > 0 ? quality : 127);
    const std::wstring playPath = bvid.empty()
        ? std::format(L"/x/player/playurl?aid={}&cid={}&qn={}&fnval=4048&fourk=1",
                      fromUtf8(aid), fromUtf8(cidText), qnText)
        : std::format(L"/x/player/playurl?bvid={}&cid={}&qn={}&fnval=4048&fourk=1",
                      fromUtf8(bvid), fromUtf8(cidText), qnText);
    if (!httpGet(L"api.bilibili.com", playPath, referer, cookie, playBody, error)) {
        result.error = L"查询播放地址失败：" + error;
        return result;
    }
    const auto playRoot = json::parse(playBody);
    const long long playCode = playRoot["code"].asInt(-1);
    if (playCode != 0) {
        result.error = apiFailure(L"获取播放地址", playCode);
        return result;
    }

    // 3. Pick the highest H.264 rendition: every Windows hardware decoder handles
    //    it, and it avoids the HEVC licensing question entirely.
    const auto& renditions = dashArray(playRoot, "video");
    if (renditions.elements().empty()) {
        result.error = L"没有可用的视频流（该视频可能需要登录）";
        return result;
    }
    auto baseUrlOf = [](const json::Value& rendition) {
        std::string base = rendition["baseUrl"].asString();
        if (base.empty()) base = rendition["base_url"].asString();
        return base;
    };
    // Prefer H.264; fall back to any codec rather than showing nothing. When the
    // caller asked for a specific qn (the browser bridge following the web player),
    // take that rendition, or the closest lower one if the server omits it.
    const auto pickRendition = [&](bool avcOnly) -> const json::Value* {
        const json::Value* chosen = nullptr;
        for (const auto* rendition : renditions.elements()) {
            // A specific qn means "this exact rendition, whatever codec": HDR (125)
            // and Dolby Vision (126) are HEVC-only, so the H.264 preference would
            // silently drop them and fall back to an SDR rendition.
            if (quality <= 0 && avcOnly && (*rendition)["codecs"].asString().rfind("avc1", 0) != 0) continue;
            if (baseUrlOf(*rendition).empty()) continue;
            const long long id = (*rendition)["id"].asInt(0);
            if (quality > 0) {
                if (id == quality) return rendition;
                if (id <= quality && (chosen == nullptr || id > (*chosen)["id"].asInt(0)))
                    chosen = rendition;
            } else if (chosen == nullptr || id > (*chosen)["id"].asInt(0)) {
                chosen = rendition;
            }
        }
        return chosen;
    };
    const json::Value* best = pickRendition(true);
    // A specific qn skips the codec filter entirely, so the second pass could only
    // repeat the first; only "best available" needs the H.264-then-anything fallback.
    if (!best && quality <= 0) best = pickRendition(false);
    if (!best) {
        result.error = L"没有找到可播放的视频流地址";
        return result;
    }
    const std::string bestUrl = baseUrlOf(*best);
    const std::string bestCodec = (*best)["codecs"].asString();
    const long long bestId = (*best)["id"].asInt(0);

    result.url = fromUtf8(bestUrl);
    result.codec = fromUtf8(bestCodec);
    result.width = unsigned((*best)["width"].asInt(0));
    result.height = unsigned((*best)["height"].asInt(0));
    result.quality = int(bestId);
    result.headers = L"Referer: https://www.bilibili.com\r\n";
    result.userAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

    // 4. Audio. DASH keeps it in its own array; the video stream above carries no
    //    audio track at all, so without this the picture would be silent. The
    //    highest bitrate is what the site itself plays.
    {
        std::string bestAudio, bestAudioCodec;
        long long bestBandwidth = -1;
        for (const auto* a : dashArray(playRoot, "audio").elements()) {
            const std::string base = baseUrlOf(*a);
            if (base.empty()) continue;
            const long long bandwidth = (*a)["bandwidth"].asInt(0);
            if (bandwidth > bestBandwidth) {
                bestBandwidth = bandwidth;
                bestAudio = base;
                bestAudioCodec = (*a)["codecs"].asString();
            }
        }
        if (!bestAudio.empty()) {
            result.audioUrl = fromUtf8(bestAudio);
            result.audioCodec = fromUtf8(bestAudioCodec);
        } else {
            log::warn("bilibili", "no DASH audio track in the response; playback would be silent");
        }
    }

    result.ok = true;
    log::info("bilibili", std::format("resolved {} -> video codec={} quality={} audio={}",        toUtf8(pageUrl), bestCodec, bestId, result.audioUrl.empty() ? "none" : toUtf8(result.audioCodec)));
    return result;
}

} // namespace veyra::media
