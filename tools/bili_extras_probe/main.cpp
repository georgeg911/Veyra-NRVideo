// Throwaway probe: resolve a page URL, then pull its danmaku and CC subtitles.
// Used to verify the two extra endpoints without going through the player.
#include "veyra/media/BilibiliExtras.h"
#include "veyra/media/BilibiliResolver.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {
std::wstring widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n > 1 ? size_t(n - 1) : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: bili_extras_probe <page-url>\n"); return 2; }
    const std::wstring page = widen(argv[1]);
    const std::wstring cookie = veyra::media::loadBilibiliCookie();
    std::printf("cookie: %zu chars, signed-in=%d\n", cookie.size(),
                int(veyra::media::bilibiliCookieLooksSignedIn(cookie)));

    const auto stream = veyra::media::resolveBilibiliStream(page, cookie);
    if (!stream.ok) {
        std::printf("resolve FAILED: %ls\n", stream.error.c_str());
        return 1;
    }
    std::printf("resolved: %ux%u aid=%lld cid=%lld qn=%d\n",
                stream.width, stream.height, stream.aid, stream.cid, stream.quality);

    std::vector<veyra::media::BiliDanmaku> danmaku;
    std::wstring error;
    const bool dmOk = veyra::media::fetchBiliDanmaku(stream.cid, danmaku, error);
    std::printf("danmaku: ok=%d count=%zu error=%ls\n", int(dmOk), danmaku.size(), error.c_str());
    if (dmOk && !danmaku.empty()) {
        std::printf("  first: t=%.2f mode=%d color=%06X text=%ls\n",
                    danmaku.front().time, danmaku.front().mode, danmaku.front().color,
                    danmaku.front().text.c_str());
    }

    for (const wchar_t* mode : {L"zh", L"en", L"both"}) {
        std::vector<veyra::media::BiliSubtitleCue> cues;
        veyra::media::BiliSubtitleStyle style;
        std::wstring language;
        error.clear();
        const bool subOk = veyra::media::fetchBiliSubtitle(stream.aid, stream.cid, cookie, mode,
                                                           cues, style, language, error);
        std::printf("subtitle[%ls]: ok=%d count=%zu lang=%ls style=%d error=%ls\n",
                    mode, int(subOk), cues.size(), language.c_str(), int(style.present), error.c_str());
        if (subOk && !cues.empty()) {
            std::wstring first = cues.front().content;
            for (auto& c : first) if (c == L'\n') c = L'|';
            std::printf("  first: %.2f-%.2f %ls\n", cues.front().from, cues.front().to, first.c_str());
        }
    }
    return 0;
}
