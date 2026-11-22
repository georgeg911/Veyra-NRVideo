#pragma once
#include <atomic>
#include <chrono>
#include <cwctype>
#include <string>
#include <string_view>
#include <mutex>
#include <windows.h>
#include "veyra/media/NetworkSettings.h"
extern "C" {
#include <libavformat/avformat.h>
}
namespace veyra::media {
// Network deadlines (#48). The old single 5 s limit applied to open, probe and every
// read alike, so a slow server or a Wi-Fi hiccup was reported as a failed source.
// Opening a connection and reading media now have separate limits; the owner's
// cancel flag (stop / source switch) still aborts every blocking call at once.
constexpr int kConnectTimeoutMs = 40000;     // avformat_open_input as a whole (a dead connection is cut at 15 s by rw_timeout)
// 重连用的短预算。断网时重连是在引擎的主循环线程上同步做的，40 秒的预算会把画面冻住
// 几十秒（现场 2026-11-22 实测 stall 38 秒、连续写 minidump）。重连本来就只是「再试一次
// 这个地址」，试不通就换下一个，不该按首次打开的标准等。
// Short budget for reconnects. A reopen runs synchronously on the engine's main loop, so
// the 40 s open budget freezes the picture for tens of seconds (measured 2026-11-22: a
// 38 s stall and repeated minidumps). A reopen is just "try this address again" - when it
// does not answer quickly, move to the next one.
constexpr int kReconnectTimeoutMs = 6000;
constexpr int kProbeTimeoutMs = 30000;       // avformat_find_stream_info
constexpr int kReadTimeoutVodMs = 20000;     // one av_read_frame / seek on a seekable source
constexpr int kReadTimeoutLiveMs = 10000;    // one av_read_frame on a live source

inline bool datagramMediaInput(std::wstring_view path) {
    return path.size() >= 4 && (_wcsnicmp(path.data(), L"rtp:", 4) == 0 ||
                               _wcsnicmp(path.data(), L"udp:", 4) == 0);
}

inline bool networkMediaInput(std::wstring_view path) {
    const auto colon=path.find(L':');
    if(colon==std::wstring_view::npos||colon>5)return false;
    std::wstring scheme(path.substr(0,colon));
    for(auto& ch:scheme)ch=std::towlower(ch);
    return scheme==L"http"||scheme==L"https"||scheme==L"rtsp"||datagramMediaInput(path);
}
// A file on a server: a UNC path (\\host\share\...) or a drive letter mapped to a
// network share (GetDriveType == DRIVE_REMOTE). Local extended paths (\\?\C:\...)
// are local. Implemented in DemuxSession.cpp.
bool remoteMediaPath(std::wstring_view path);
// VEYRA_TEST_FORCE_DEMUX_SESSION=1: send local files through the shared demux
// session too, to compare it against the direct path frame by frame.
bool forceDemuxSessionForTest();
// Playback sources that read through the shared demux session. Offline users
// (export, tools) pass playback=false and keep the direct file path for NAS files.
inline bool sharedDemuxInput(std::wstring_view path, bool playback) {
    return networkMediaInput(path) || (playback && remoteMediaPath(path)) || forceDemuxSessionForTest();
}

// Per-operation deadlines only on networks; immediate owner cancellation on all inputs.
class InputInterrupt {
public:
    void configure(bool network,const std::atomic<bool>* owner,const std::atomic<bool>* local=nullptr,bool live=false){network_=network;owner_=owner;local_=local;live_=live;deadline_=0;}
    // Read / seek deadline.
    // 本地文件默认不给 deadline（本地磁盘读本该是即时的）。但损坏/稀疏文件是个例外：
    // seek 要扫过成片的空洞，没有上限就会把读取线程（以及等它的主循环）卡住几十秒。
    // 用 armBounded() 给这条路径一个有限预算。
    // A local file normally gets no deadline (local reads are meant to be immediate). Damaged
    // or sparse files are the exception: a seek has to scan across long holes, and without a
    // limit it blocks the read thread - and the main loop waiting on it - for tens of seconds.
    // armBounded() gives that path a finite budget.
    void arm(){deadline_=network_?now()+int64_t(live_?kReadTimeoutLiveMs:kReadTimeoutVodMs)*1000000ll:0;}
    void armBounded(){deadline_=now()+int64_t(kBoundedSeekMs)*1000000ll;}
    static constexpr int kBoundedSeekMs = 1500;
    void armConnect(){deadline_=network_?now()+int64_t(kConnectTimeoutMs)*1000000ll:0;}
    // 重连路径：用短预算，别把主循环冻住几十秒。
    // Reconnect path: a short budget so the main loop is not frozen for tens of seconds.
    void armConnectShort(){deadline_=network_?now()+int64_t(kReconnectTimeoutMs)*1000000ll:0;}
    void armProbe(){deadline_=network_?now()+int64_t(kProbeTimeoutMs)*1000000ll:0;}
    void disarm(){deadline_=0;}
    static int callback(void* opaque){
        const auto& self=*static_cast<InputInterrupt*>(opaque);const auto deadline=self.deadline_.load();
        return (self.owner_&&self.owner_->load())||(self.local_&&self.local_->load())||(deadline&&now()>=deadline);
    }
private:
    static int64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    const std::atomic<bool>* owner_=nullptr;const std::atomic<bool>* local_=nullptr;
    bool network_=false,live_=false;std::atomic<int64_t> deadline_{0};
};
inline void networkMediaOptions(AVDictionary** options){
    static std::once_flag initialized;std::call_once(initialized,[]{avformat_network_init();});
    av_dict_set(options,"rw_timeout","15000000",0);av_dict_set(options,"timeout","15000000",0);
    av_dict_set(options,"rtsp_transport","tcp",0);
}
// Extra HTTP request headers, one "Name: value" per line.
//
// Some hosts (Bilibili's CDN among them) reject a media request that does not carry
// the site's Referer and User-Agent, so a resolved stream URL alone is not enough.
// FFmpeg takes them as one CRLF-separated "headers" option, and the UA has its own
// option: putting User-Agent inside `headers` sends it twice and the CDN refuses.
inline void networkMediaHeaders(AVDictionary** options,const std::wstring& headers,
                                const std::wstring& userAgent={}){
    auto setOption=[options](const char* name,const std::wstring& value){
        if(value.empty())return;
        const int size=WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
        if(size<=0)return;
        std::string utf8(static_cast<size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,0,value.c_str(),static_cast<int>(value.size()),utf8.data(),size,nullptr,nullptr);
        av_dict_set(options,name,utf8.c_str(),0);
    };
    setOption("headers",headers);
    setOption("user_agent",userAgent);
}
}
