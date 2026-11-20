// Shared HTTPS plumbing for the Bilibili modules.
//
// Private to src/media: BilibiliResolver (page URL -> CDN stream) and
// BilibiliExtras (danmaku XML, CC subtitles) both talk to bilibili.com hosts and
// must agree on timeouts, headers and decompression, so the two requests live
// here instead of being copied per module.
#pragma once

#include <string>

namespace veyra::media::bili {

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);

// One HTTPS GET. Answers UTF-8 text.
//
// WINHTTP_OPTION_DECOMPRESSION is set on purpose: comment.bilibili.com answers
// gzip even when the request carries no Accept-Encoding, and WinHTTP does not
// inflate by itself. Without it the danmaku XML arrives as binary noise.
bool httpGet(const std::wstring& host, const std::wstring& path, const std::wstring& referer,
             const std::wstring& cookie, std::string& body, std::wstring& error);

// One HTTPS GET that reports where a redirect points instead of a body. b23.tv
// links (the form the phone app shares) answer 302 with the real video URL.
bool httpFollowLocation(const std::wstring& host, const std::wstring& path,
                        std::wstring& location, std::wstring& error);

} // namespace veyra::media::bili
