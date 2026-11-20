#pragma once
#include <string>
#include <vector>

namespace veyra::media {

// One danmaku comment, as the comment CDN's XML track describes it.
struct BiliDanmaku {
    double time = 0.0;      // seconds into the video
    int mode = 1;           // 1..3 scroll, 4 bottom, 5 top
    int color = 0xFFFFFF;
    std::wstring text;
};

// One CC subtitle track the site offers for a part.
struct BiliSubtitleTrack {
    std::wstring lan;       // zh / ai-zh / en ...
    std::wstring url;       // absolute https URL; empty when the request was unsigned
};

// One subtitle line. Seconds, same unit as the site's JSON.
struct BiliSubtitleCue {
    double from = 0.0;
    double to = 0.0;
    std::wstring content;
};

// 站方字幕自带的显示参数（BCC 头）。网页版的「白字 + 半透明紫底」就来自这几个字段，
// 和播放器自己的全局字幕偏好是两套东西。
// The site's own display parameters from the BCC header: what gives the web player
// its white-on-purple look, independent of the player's global subtitle preferences.
struct BiliSubtitleStyle {
    std::wstring fontColor;         // font_color, e.g. "#FFFFFF"
    std::wstring backgroundColor;   // background_color, e.g. "#9C27B0"
    double backgroundAlpha = 0.0;   // background_alpha, 0..1
    std::wstring stroke;            // "none" or a width; AI tracks spell it "Stroke"
    int location = 2;               // 2 = bottom centre (numpad alignment)
    bool present = false;           // the header actually carried these fields
};

// Downloads the part's danmaku. No login and no signature: the track is public XML
// keyed on cid. (api.bilibili.com answers protobuf now; the comment CDN still speaks
// XML, which is why this reads the CDN.)
bool fetchBiliDanmaku(long long cid, std::vector<BiliDanmaku>& out, std::wstring& error);

// Lists and downloads CC subtitles for a part. Signed with wbi on purpose: the
// unsigned /x/player/v2 answers the list with empty subtitle_url values, which reads
// as "this video has no subtitles". `cookie` must be the signed-in session.
//
// A video with no CC track is NOT an error: it returns true with `cues` empty, so the
// caller can stay silent about it.
bool fetchBiliSubtitle(long long aid, long long cid, const std::wstring& cookie,
                       std::vector<BiliSubtitleCue>& cues, BiliSubtitleStyle& style,
                       std::wstring& language, std::wstring& error);

} // namespace veyra::media
