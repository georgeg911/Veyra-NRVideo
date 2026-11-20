// Bilibili danmaku (comment track) and CC subtitles.
//
// Both hang off a resolved video part (aid, cid) that BilibiliResolver already
// fetched, so this module only does the two extra requests and the parsing. It
// shares the HTTPS plumbing with the resolver (BilibiliHttp.h) so headers,
// timeouts and decompression cannot drift apart.
#include "veyra/media/BilibiliExtras.h"
#include "veyra/media/BiliJson.h"
#include "veyra/Log.h"
#include "BilibiliHttp.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace veyra::media {
namespace {

using bili::fromUtf8;
using bili::httpGet;
using bili::toUtf8;

constexpr wchar_t kReferer[] = L"https://www.bilibili.com";

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(char(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
    }
    return out;
}

// --- wbi signing -----------------------------------------------------------
// The scheme newBV uses (ApiSign.kt): the img_key and sub_key from
// /x/web-interface/nav are concatenated, permuted through a fixed table, and the
// first 32 characters form the mixin key. The query is sorted, the mixin key
// appended, and the MD5 of that string becomes w_rid.

const int kMixinTab[64] = {
    46,47,18,2,53,8,23,32,15,50,10,31,58,3,45,35,
    27,43,5,49,33,9,42,19,29,28,14,39,12,38,41,13,
    37,48,7,16,24,55,40,61,26,17,0,1,60,51,30,4,
    22,25,54,21,56,59,6,63,57,62,11,36,20,34,44,52
};

std::string md5Hex(const std::string& input) {
    // Windows CNG, so no crypto library is added for one hash.
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0) {
        DWORD hashLen = 0, got = 0;
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &got, 0);
        std::vector<UCHAR> hash(hashLen);
        BCRYPT_HASH_HANDLE h = nullptr;
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), DWORD(input.size()), 0);
            BCryptFinishHash(h, hash.data(), hashLen, 0);
            BCryptDestroyHash(h);
            static const char* hex = "0123456789abcdef";
            result.reserve(hashLen * 2);
            for (UCHAR b : hash) { result.push_back(hex[b >> 4]); result.push_back(hex[b & 0xF]); }
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return result;
}

struct WbiKeys {
    std::string imgKey;
    std::string subKey;
    int64_t fetchedAt = 0;
};

WbiKeys& wbiKeys() {
    static WbiKeys keys;
    return keys;
}

// The nav response carries the keys as full URLs; only the file stem is signed.
std::string keyFromUrl(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    std::string stem = slash == std::string::npos ? url : url.substr(slash + 1);
    const size_t dot = stem.find('.');
    return dot == std::string::npos ? stem : stem.substr(0, dot);
}

bool refreshWbiKeys(const std::wstring& cookie) {
    std::string body;
    std::wstring error;
    if (!httpGet(L"api.bilibili.com", L"/x/web-interface/nav", kReferer, cookie, body, error)) return false;
    const auto root = json::parse(body);
    const std::string imgUrl = root["data"]["wbi_img"]["img_url"].asString();
    const std::string subUrl = root["data"]["wbi_img"]["sub_url"].asString();
    if (imgUrl.empty() || subUrl.empty()) return false;
    auto& k = wbiKeys();
    k.imgKey = keyFromUrl(imgUrl);
    k.subKey = keyFromUrl(subUrl);
    k.fetchedAt = GetTickCount64();
    log::info("bilibili", std::format("wbi keys refreshed img={} sub={}", k.imgKey.size(), k.subKey.size()));
    return !k.imgKey.empty() && !k.subKey.empty();
}

std::string mixinKey() {
    auto& k = wbiKeys();
    const std::string orig = k.imgKey + k.subKey;
    if (orig.size() < 64) return {};
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out.push_back(orig[size_t(kMixinTab[i])]);
    return out;
}

// Signs `params` in place (adds wts and w_rid) and returns the query string.
std::string signWbi(std::map<std::string, std::string>& params, const std::wstring& cookie) {
    auto& k = wbiKeys();
    if (k.imgKey.empty() || GetTickCount64() - k.fetchedAt > 3600ull * 1000) refreshWbiKeys(cookie);
    const std::string mixin = mixinKey();
    const int64_t wts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    params["wts"] = std::to_string(wts);

    std::string joined;
    for (const auto& [key, value] : params) {
        if (!joined.empty()) joined += '&';
        // The site strips !'()* from values before signing.
        std::string filtered;
        for (char c : value) if (c != '!' && c != '\'' && c != '(' && c != ')' && c != '*') filtered.push_back(c);
        joined += key + "=" + filtered;
    }
    params["w_rid"] = md5Hex(joined + mixin);

    std::string query;
    for (const auto& [key, value] : params) {
        if (!query.empty()) query += '&';
        query += key + "=" + urlEncode(value);
    }
    return query;
}

// "https://host/path?x" -> host, "/path?x".
bool splitUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return false;
    const size_t hostStart = scheme + 3;
    const size_t slash = url.find(L'/', hostStart);
    host = url.substr(hostStart, slash == std::wstring::npos ? std::wstring::npos : slash - hostStart);
    path = slash == std::wstring::npos ? L"/" : url.substr(slash);
    return !host.empty();
}

// The site's display parameters live in the BCC header. Missing fields leave
// present=false, which the caller reads as "use the player's own style".
void readStyle(const json::Value& root, BiliSubtitleStyle& style) {
    style.fontColor = fromUtf8(root["font_color"].asString());
    style.backgroundColor = fromUtf8(root["background_color"].asString());
    style.backgroundAlpha = root["background_alpha"].asDouble(0.0);
    // AI tracks spell it "Stroke"; ordinary ones use lowercase. Accept both.
    style.stroke = fromUtf8(root["stroke"].asString());
    if (style.stroke.empty()) style.stroke = fromUtf8(root["Stroke"].asString());
    const auto body = root["body"].elements();
    if (!body.empty()) {
        const long long loc = (*body.front())["location"].asInt(2);
        if (loc > 0 && loc <= 9) style.location = int(loc);
    }
    style.present = !style.fontColor.empty() || !style.backgroundColor.empty()
                    || !style.stroke.empty() || root["font_size"].asDouble(0.0) > 0.0;
}

// Downloads one track's cues. The display parameters come from whichever track is
// the primary one; a secondary track only contributes its text.
bool downloadTrack(const std::wstring& url, const std::wstring& cookie,
                   std::vector<BiliSubtitleCue>& cues, BiliSubtitleStyle* style,
                   std::wstring& error) {
    std::wstring host, path;
    if (!splitUrl(url, host, path)) { error = L"字幕地址不是 https"; return false; }
    std::string body;
    if (!httpGet(host, path, kReferer, cookie, body, error)) return false;
    const auto root = json::parse(body);
    if (style) readStyle(root, *style);
    for (const auto& entry : root["body"].elements()) {
        BiliSubtitleCue cue;
        cue.from = (*entry)["from"].asDouble(0.0);
        cue.to = (*entry)["to"].asDouble(0.0);
        cue.content = fromUtf8((*entry)["content"].asString());
        if (cue.to <= cue.from || cue.content.empty()) continue;
        cues.push_back(std::move(cue));
    }
    if (cues.empty()) { error = L"字幕内容为空"; return false; }
    return true;
}

// Prefix match on the site's language codes: zh, ai-zh, zh-Hans, en, ai-en...
bool languageIs(const std::wstring& lan, const wchar_t* tag) {
    return lan.rfind(tag, 0) == 0;
}

// The comment track is XML, so a comment containing & or < arrives escaped. Without
// this the player shows "&amp;" and "&lt;" literally. Only the five predefined
// entities plus numeric character references occur in practice.
std::wstring unescapeXml(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out.push_back(text[i]); continue; }
        const size_t semi = text.find(';', i + 1);
        // A '&' with no terminator nearby is literal text, not an entity.
        if (semi == std::string::npos || semi - i > 10) { out.push_back('&'); continue; }
        const std::string name = text.substr(i + 1, semi - i - 1);
        if (name == "amp") out.push_back('&');
        else if (name == "lt") out.push_back('<');
        else if (name == "gt") out.push_back('>');
        else if (name == "quot") out.push_back('"');
        else if (name == "apos") out.push_back('\'');
        else if (!name.empty() && name[0] == '#') {
            // &#123; decimal or &#x1F; hexadecimal.
            const bool hex = name.size() > 2 && (name[1] == 'x' || name[1] == 'X');
            const std::string digits = name.substr(hex ? 2 : 1);
            unsigned code = 0;
            try { code = unsigned(std::stoul(digits, nullptr, hex ? 16 : 10)); }
            catch (...) { out += text.substr(i, semi - i + 1); i = semi; continue; }
            if (code < 0x80) out.push_back(char(code));
            else if (code < 0x800) {
                out.push_back(char(0xC0 | (code >> 6)));
                out.push_back(char(0x80 | (code & 0x3F)));
            } else if (code < 0x10000) {
                out.push_back(char(0xE0 | (code >> 12)));
                out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(char(0x80 | (code & 0x3F)));
            } else {
                out.push_back(char(0xF0 | (code >> 18)));
                out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(char(0x80 | (code & 0x3F)));
            }
        } else {
            out += text.substr(i, semi - i + 1);   // unknown entity, keep as written
        }
        i = semi;
    }
    return fromUtf8(out);
}

} // namespace

bool fetchBiliDanmaku(long long cid, std::vector<BiliDanmaku>& out, std::wstring& error) {
    out.clear();
    if (cid <= 0) { error = L"缺少 cid"; return false; }
    // The XML track is public and keyed on cid; no cookie and no signature.
    const std::wstring path = L"/" + std::to_wstring(cid) + L".xml";
    std::string body;
    if (!httpGet(L"comment.bilibili.com", path, kReferer, L"", body, error)) return false;
    if (body.empty()) { error = L"弹幕为空"; return false; }
    if (body.find("<d ") == std::string::npos) { error = L"弹幕数据格式异常"; return false; }

    size_t pos = 0;
    while (true) {
        const size_t open = body.find("<d p=\"", pos);
        if (open == std::string::npos) break;
        const size_t attrStart = open + 6;
        const size_t attrEnd = body.find('"', attrStart);
        if (attrEnd == std::string::npos) break;
        const size_t textStart = body.find('>', attrEnd);
        if (textStart == std::string::npos) break;
        const size_t textEnd = body.find("</d>", textStart);
        if (textEnd == std::string::npos) break;

        // p is "time,mode,size,color,timestamp,pool,uid,rowid".
        const std::string attrs = body.substr(attrStart, attrEnd - attrStart);
        std::vector<std::string> fields;
        size_t start = 0;
        for (;;) {
            const size_t comma = attrs.find(',', start);
            fields.push_back(attrs.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (fields.size() >= 4) {
            BiliDanmaku one;
            try {
                one.time = std::stod(fields[0]);
                one.mode = std::stoi(fields[1]);
                one.color = std::stoi(fields[3]);
            } catch (...) {
                pos = textEnd + 4;
                continue;
            }
            one.text = unescapeXml(body.substr(textStart + 1, textEnd - textStart - 1));
            out.push_back(std::move(one));
        }
        pos = textEnd + 4;
    }
    if (out.empty()) { error = L"没有弹幕"; return false; }
    return true;
}

bool fetchBiliSubtitle(long long aid, long long cid, const std::wstring& cookie,
                       const std::wstring& mode,
                       std::vector<BiliSubtitleCue>& cues, BiliSubtitleStyle& style,
                       std::wstring& language, std::wstring& error) {
    cues.clear();
    style = {};
    language.clear();
    if (aid <= 0 || cid <= 0) { error = L"缺少 aid/cid"; return false; }
    if (cookie.empty()) { error = L"字幕需要登录"; return false; }

    // 1. The track list, from the wbi-signed endpoint.
    std::map<std::string, std::string> params{
        {"aid", std::to_string(aid)},
        {"cid", std::to_string(cid)},
    };
    const std::string query = signWbi(params, cookie);
    if (query.empty()) { error = L"字幕签名失败"; return false; }
    std::string listBody;
    if (!httpGet(L"api.bilibili.com", L"/x/player/wbi/v2?" + fromUtf8(query), kReferer, cookie, listBody, error))
        return false;
    const auto listRoot = json::parse(listBody);
    const long long code = listRoot["code"].asInt(-1);
    if (code != 0) {
        error = code == -101 ? L"字幕需要登录" : std::format(L"字幕列表错误码 {}", code);
        return false;
    }

    std::vector<BiliSubtitleTrack> tracks;
    for (const auto& entry : listRoot["data"]["subtitle"]["subtitles"].elements()) {
        BiliSubtitleTrack one;
        one.lan = fromUtf8((*entry)["lan"].asString());
        one.url = fromUtf8((*entry)["subtitle_url"].asString());
        if (one.url.empty()) continue;
        if (one.url.rfind(L"//", 0) == 0) one.url = L"https:" + one.url;
        tracks.push_back(std::move(one));
    }
    // No CC track on this video: not an error, and not worth a notice.
    if (tracks.empty()) return true;

    // 2. Pick the Chinese and the English track, if the video has them. Site codes
    //    are zh / ai-zh for Chinese and en / ai-en for English; machine translations
    //    sort after the human ones.
    const auto pick = [&tracks](const std::wstring& want) -> const BiliSubtitleTrack* {
        for (const auto& t : tracks) if (t.lan == want) return &t;
        return nullptr;
    };
    const auto pickTag = [&tracks](const wchar_t* tag) -> const BiliSubtitleTrack* {
        for (const auto& t : tracks) if (languageIs(t.lan, tag)) return &t;
        return nullptr;
    };
    const BiliSubtitleTrack* chinese = pick(L"zh");
    if (!chinese) chinese = pick(L"ai-zh");
    if (!chinese) chinese = pickTag(L"zh");
    const BiliSubtitleTrack* english = pick(L"en");
    if (!english) english = pick(L"ai-en");
    if (!english) english = pickTag(L"en");

    const bool wantEnglish = mode == L"en";
    const bool wantBoth = mode == L"both";
    // A language the video does not carry falls back to the other one: showing the
    // other language beats showing nothing.
    const BiliSubtitleTrack* primary = wantEnglish ? (english ? english : chinese)
                                                   : (chinese ? chinese : english);
    if (!primary) primary = &tracks.front();

    // 3. Download the primary track. The URL lives on a CDN host, not api.bilibili.com.
    if (!downloadTrack(primary->url, cookie, cues, &style, error)) return false;
    language = primary->lan;

    // 4. Bilingual: merge the English line under each Chinese cue. A Chinese track
    //    sometimes already carries "Chinese\nEnglish" in one cue, in which case the
    //    merge is skipped and the second line is simply kept.
    bool merged = false;
    if (wantBoth && english && english != primary) {
        std::vector<BiliSubtitleCue> englishCues;
        BiliSubtitleStyle unusedStyle;
        std::wstring ignored;
        if (downloadTrack(english->url, cookie, englishCues, &unusedStyle, ignored)) {
            size_t j = 0;
            for (auto& cue : cues) {
                while (j < englishCues.size() && englishCues[j].to <= cue.from) ++j;
                if (j >= englishCues.size() || englishCues[j].from >= cue.to) continue;
                const auto nl = cue.content.find(L'\n');
                if (nl != std::wstring::npos) cue.content.resize(nl);
                cue.content += L'\n' + englishCues[j].content;
            }
            merged = true;
            language = L"both";
        }
    }

    // Single-language modes keep one line: a Chinese cue may hold both languages and
    // the second one is noise when only one was asked for.
    if (!wantBoth) {
        for (auto& cue : cues) {
            const auto nl = cue.content.find(L'\n');
            if (nl != std::wstring::npos) cue.content.resize(nl);
        }
    }

    log::info("bilibili", std::format("subtitle track mode={} lang={} cues={} merged={}",
        toUtf8(mode), toUtf8(language), cues.size(), merged));
    return true;
}

} // namespace veyra::media
