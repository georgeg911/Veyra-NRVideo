#pragma once
#include <string>
#include <vector>

namespace veyra::media {

// Resolves a Bilibili page URL to a playable media stream URL.
//
// The player can already open any http(s) media URL; what it cannot do is turn
// "https://www.bilibili.com/video/BV..." into the CDN address of the actual
// stream. That is what this does, through Bilibili's public web API:
//
//   view?bvid=...   -> cid (the page's clip id) and the title
//   playurl?...     -> a DASH manifest of video/audio renditions
//
// The chosen rendition is the highest H.264 one, because H.264 is what every
// hardware decoder on Windows handles without the browser codec question.
//
// The CDN requires the site's Referer and User-Agent; those come back in
// `headers` so the demuxer can send them (see networkMediaHeaders).
struct BilibiliStream {
    bool ok = false;
    std::wstring url;        // playable media URL (video)
    // DASH serves audio separately: the video URL carries no audio track, so the
    // player has to open this second address as well. Empty when the response
    // delivered a muxed stream.
    std::wstring audioUrl;
    std::wstring headers;    // "Referer: https://www.bilibili.com\r\n"
    // Sent through FFmpeg's user_agent option: a User-Agent inside `headers` would be
    // sent twice and the CDN rejects the request.
    std::wstring userAgent;
    std::wstring title;      // page title, for the UI
    // Identifiers of the part that was resolved. The danmaku track is keyed on cid
    // and the CC subtitle list on aid+cid, so carrying them here saves the extras
    // module a second view request.
    long long aid = 0;
    long long cid = 0;
    std::wstring error;      // human-readable reason when ok == false
    unsigned width = 0;
    unsigned height = 0;
    std::wstring codec;      // e.g. "avc1.64001F"
    std::wstring audioCodec; // e.g. "mp4a.40.2"
    int quality = 0;         // Bilibili quality id that was selected
    // Every page of a multi-part video (分P), in page order; one entry for a
    // single-part clip. The player queues one playlist entry per part so a lecture
    // series plays through instead of stopping after part 1.
    struct Part {
        long long cid = 0;
        std::wstring title;      // the site's own "P1 ..." title
    };
    std::vector<Part> parts;
    int currentPart = 1;         // 1-based page the resolved stream belongs to
    // The playurl response lists several mirrors for one audio rendition under
    // backupUrl. They are live CDN edges, not copies, and the API hands the player
    // all of them precisely so it can move on when one edge stops serving. Playback
    // used to keep only the first address, so when an edge dropped the connection
    // mid-film the buffering gate kept reopening the dead address until it gave up
    // and the rest of the video played silent. Empty when the response carried a
    // single address; the caller falls back to `audioUrl`.
    std::vector<std::wstring> audioFallbacks;
};

// True when `url` looks like a Bilibili video page.
bool isBilibiliPageUrl(const std::wstring& url);

// Resolves the page. Network access happens on the calling thread; call it off the
// UI thread or accept a short block.
//
// `cookie` is the site's Cookie header value (the user's own login). Without it the
// API only serves low renditions - a member's 1080P+/4K streams require the SESSDATA
// and bili_jct cookies from a signed-in browser.
BilibiliStream resolveBilibiliStream(const std::wstring& pageUrl,
                                     const std::wstring& cookie = {},
                                     int quality = 0);   // qn; 0 = best available

// Where the user's cookie is kept between runs.
std::wstring bilibiliCookiePath();

// Reads the stored cookie (empty when none). The file holds only the Cookie header,
// DPAPI-encrypted behind a "VBCK" magic; a plain-text file written by an older
// build is still read, so an existing sign-in is not lost.
std::wstring loadBilibiliCookie();

// True when `cookie` looks like a signed-in Bilibili session: it must carry SESSDATA.
bool bilibiliCookieLooksSignedIn(const std::wstring& cookie);

} // namespace veyra::media
