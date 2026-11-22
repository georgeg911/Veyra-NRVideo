#pragma once

// FFmpeg demuxer wrapper (Playbook section 13.1). Video-only for Phase 3;
// PTS-driven, never assumes a fixed frame rate.
//
// Two modes. Direct (default): this class owns an AVFormatContext and reads it on
// the caller's thread - local files, exactly as before. Shared (openShared): the
// stream description and the packets come from a DemuxSession that also feeds the
// audio pipeline (network sources and files on a server, #48).

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include "veyra/media/DemuxSession.h"
#include "veyra/media/NetworkInput.h"

struct AVFormatContext;
struct AVCodecParameters;
struct AVPacket;

namespace veyra::media {

struct DemuxerStats {
    uint64_t packetsRead = 0;
    int64_t firstPts = 0;
    int64_t lastPts = 0;
    uint64_t ptsNonMonotonicCount = 0;
    // 容错模式下因为读不出来而被跳过的区段数（正常模式下恒为 0）。
    // Regions skipped in tolerant mode because they could not be read (always 0 otherwise).
    uint64_t damagedRegionsSkipped = 0;
};

class FFmpegDemuxer {
public:
    FFmpegDemuxer() = default;
    ~FFmpegDemuxer();

    FFmpegDemuxer(const FFmpegDemuxer&) = delete;
    FFmpegDemuxer& operator=(const FFmpegDemuxer&) = delete;

    // tolerate=true 时用宽松的 err_detect（坏包跳过而不是整个输入失败），
    // 用于「文件损坏也要能播未损坏部分」的场景。默认 false 保持严格，免得
    // 正常文件里的真实格式错误被静默吞掉。
    // tolerate=true switches to a lenient err_detect (skip bad packets instead of failing
    // the whole input), for "play the intact part of a damaged file". Defaults to false to
    // keep strict behaviour so genuine format errors on healthy files stay visible.
    bool open(const std::wstring& path,const std::atomic<bool>* cancelled=nullptr,bool tolerate=false);
    // Shared mode: packets and stream parameters come from the session.
    bool openShared(std::shared_ptr<DemuxSession> session);
    void close();

    bool opened() const { return context_ != nullptr || session_ != nullptr; }
    bool shared() const { return session_ != nullptr; }
    const std::shared_ptr<DemuxSession>& session() const { return session_; }
    int videoStreamIndex() const { return videoStreamIndex_; }
    int64_t durationUs() const;
    bool seekable() const;
    double averageFps() const; // informational only; the pipeline is PTS-driven
    int nominalRateNum() const;
    int nominalRateDen() const;
    const AVCodecParameters* videoCodecParameters() const;
    // All streams, for probing. Dolby Vision profile-7 detection needs to know
    // whether a second HEVC video stream (the enhancement layer) exists, which
    // only shows up at container level.
    int streamCount() const;
    const AVCodecParameters* streamParameters(int index) const;
    // Container start time in microseconds (0 when unset); packet PTS values are
    // absolute, so seeking adds this and frame timestamps subtract it.
    int64_t startTimeUs() const;
    int videoTimeBaseNum() const; // stream time_base for frame PTS conversion
    int videoTimeBaseDen() const;
    std::string formatName() const;
    // Stream sample aspect ratio (numerator, denominator) as stored in the
    // container; 0/0 = unknown. The display aspect is this ratio applied to the
    // coded size, then swapped for 90/270 degree rotations.
    void sampleAspect(int& num, int& den) const;
    // Container display-matrix rotation in degrees (0/90/180/270); 0 = none.
    int rotationDegrees() const;

    // Returns false at end of file. The packet stays owned by this demuxer
    // and remains valid until the next call. In shared mode this waits for data
    // (and never pauses for buffering); false with endOfFile=false is a failure.
    bool readVideoPacket(bool& endOfFile);
    // Enhancement-layer stream packet (second HEVC video stream), when the
    // container carries one. Returns false when this source has none.
    bool takeElPacket(AVPacket** out);
    // Tell the direct reader which stream is the enhancement layer so its packets
    // are queued instead of dropped (the shared session detects it on its own).
    void setElStreamIndex(int index) { elStreamIndex_ = index; }

    // Playback read. Direct mode behaves like readVideoPacket. Shared mode waits at
    // most waitMs and, with honourHold, reports Waiting while playback is paused
    // for buffering.
    enum class ReadResult { Packet, Eof, Error, Waiting, Cancelled };
    ReadResult readVideoPacketEx(int waitMs, bool honourHold);
    // The last packet came first after a live reconnect: reset the decoder.
    bool packetDiscontinuity() const { return packetDiscontinuity_; }

    // Seek to a time in microseconds; the next read yields a packet at or
    // after that time. Flushing the decoder afterwards is the caller's job.
    // byUser=true 重置「跳过损坏区段」的预算：用户主动 seek 应该获得完整的重试机会，
    // 而内部的自动跳过必须受预算约束（否则一个全坏文件会无限往后跳）。
    // byUser=true resets the damaged-region skip budget: a user-initiated seek deserves the
    // full retry allowance, while internal auto-skips must stay bounded (or an entirely
    // broken file would walk forward for ever).
    bool seekToUs(int64_t targetUs,bool byUser=false);
    // 解复用器当前所在位置（微秒）。容错跳段用它当基准，而不是上层记录的帧 PTS
    // （后者在 seek 之后是无效值）。
    // Where the demuxer currently is, in microseconds. Damaged-file jumps base themselves on
    // this rather than on the caller's last frame PTS, which is invalid right after a seek.
    int64_t positionUs() const { return packetAtUs_; }

    const AVPacket* currentPacket() const { return packet_; }
    const DemuxerStats& stats() const { return stats_; }

private:
    void notePacket();

    AVFormatContext* context_ = nullptr;
    std::shared_ptr<DemuxSession> session_;
    AVPacket* packet_ = nullptr;
    int elStreamIndex_ = -1;
    std::deque<AVPacket*> elPending_;   // direct mode: queued enhancement-layer packets
    int videoStreamIndex_ = -1;
    DemuxerStats stats_{};
    bool havePendingPacket_ = false;
    bool packetDiscontinuity_ = false;
    InputInterrupt interrupt_;
    // 损坏文件容错：读失败时跳过一段再试，而不是结束整条流。步长和次数都有界，
    // 免得一个彻底坏掉的文件在读取循环里长时间空转。
    // Damaged-file tolerance: skip a stretch and retry instead of ending the stream. Both
    // the step and the attempt count are bounded so a hopeless file cannot spin here.
    bool tolerant_ = false;
    int damageSkips_ = 0;
    int64_t packetAtUs_ = 0;   // 最近一个成功读到的包的时间戳，作为跳过的起点
    static constexpr int64_t kDamageSkipStepUs = 3'000'000;   // 每次跳过 3 秒
    static constexpr int kMaxDamageSkips = 40;                // 最多跳 40 次（约 2 分钟素材）
};

} // namespace veyra::media
