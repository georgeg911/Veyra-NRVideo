#include "veyra/media/FFmpegDemuxer.h"

#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/dict.h>
#include <cmath>
#include <cstdlib>
}

#include <format>

#include "veyra/Log.h"

namespace veyra::media {

namespace {
// 清掉 AVIOContext 上的粘性错误状态。
//
// FFmpeg 的 AVIOContext 记着两个「一旦置位就一直有效」的标志：
//   pb->eof_reached  读到过 EOF
//   pb->error        读过失败
// 它们不会因为一次成功的 seek 而自动复位，于是出现「跳到完好区域也读不出来」：
// av_read_frame 会立刻把那个陈旧的错误再抛一次。没下完的文件读过一个空洞之后，
// 整条流就再也读不动了——正是因为这两个标志粘住了。
// mpv 在它的 libavformat 解复用层里做的同一件事（demux_lavf.c 的
// "Clear sticky EOF/error to reuse this demuxer"）。
//
// Clear the sticky error state on the AVIOContext.
//
// FFmpeg's AVIOContext remembers two flags that stay set once raised:
//   pb->eof_reached  an EOF was seen
//   pb->error        a read failed
// A successful seek does not reset them, so "seek into an intact region still reads
// nothing": av_read_frame immediately re-raises the stale error. After reading across one
// hole in a partially downloaded file the whole stream becomes unreadable - exactly
// because these two flags stick. mpv does the same thing in its libavformat demuxer
// (demux_lavf.c, "Clear sticky EOF/error to reuse this demuxer").
void clearAvioError(AVFormatContext* context) {
    if (context == nullptr || context->pb == nullptr) return;
    context->pb->eof_reached = 0;
    context->pb->error = 0;
}
// Custom (P5): count the HEVC layers inside one access unit. Dolby Vision
// Profile 7 carries the enhancement layer as a second NAL layer inside the same
// access unit; the hardware decoder drops it, so this is the only proof that the
// data reaches us at all. Gated by VEYRA_DOVI_EL_TRACE, never on in production.
struct HevcLayerScan { unsigned nal=0, enhancement=0, maxLayer=0; };

// Matroska stores HEVC length-prefixed (hvcC), not as Annex B start codes: the
// prefix width lives in byte 21 of the configuration record.
unsigned hevcLengthSize(const AVCodecParameters* par) {
    const uint8_t* ext = par ? par->extradata : nullptr;
    const int size = par ? par->extradata_size : 0;
    if (ext && size > 21 && ext[0] == 1) return unsigned(ext[21] & 3) + 1u;
    return 0;
}

void countOneNal(const uint8_t* nal, unsigned available, HevcLayerScan& out) {
    if (nal == nullptr || available < 2) return;
    // HEVC NAL header: forbidden(1) type(6) layer(6) tid+1(3)
    const unsigned type = (unsigned(nal[0]) >> 1) & 0x3F;
    const unsigned layer = ((unsigned(nal[0]) & 1u) << 5) | (unsigned(nal[1]) >> 3);
    if (type < 1 || type > 63) return;
    ++out.nal;
    if (layer > 0) ++out.enhancement;
    if (layer > out.maxLayer) out.maxLayer = layer;
}

HevcLayerScan scanHevcLayers(const uint8_t* data, int size, unsigned lengthSize) {
    HevcLayerScan out;
    if (data == nullptr || size < 6) return out;
    if (lengthSize >= 1 && lengthSize <= 4) {
        int pos = 0;
        while (pos + int(lengthSize) + 2 <= size) {
            uint32_t length = 0;
            for (unsigned b = 0; b < lengthSize; ++b) length = (length << 8) | data[pos + b];
            pos += int(lengthSize);
            if (length == 0 || length > uint32_t(size - pos)) break;
            countOneNal(data + pos, length, out);
            pos += int(length);
        }
        return out;
    }
    // Annex B fallback (raw .hevc input): start-code delimited.
    for (int i = 0; i + 2 < size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
        countOneNal(data + i + 3, unsigned(size - i - 3), out);
        i += 2;
    }
    return out;
}

// Rotation from the "rotate" tag or the display matrix in the stream's side data.
int rotationFrom(const AVCodecParameters* parameters, const char* rotateTag)
{
    // av_display_rotation_get reads the container's display matrix (the same
    // metadata players use); it returns degrees counter-clockwise.
    if (rotateTag != nullptr && *rotateTag != '\0') {
        const int value = std::atoi(rotateTag);
        if (value != 0) return ((value % 360) + 360) % 360;
    }
    if (parameters == nullptr) return 0;
    // FFmpeg 6+ exposes stream side data through the packet-side-data API.
    const AVPacketSideData* side = parameters->coded_side_data;
    const int sideCount = parameters->nb_coded_side_data;
    const uint8_t* matrix = nullptr;
    for (int i = 0; i < sideCount; ++i) {
        if (side[i].type == AV_PKT_DATA_DISPLAYMATRIX && side[i].size >= 9 * int(sizeof(int32_t))) { matrix = side[i].data; break; }
    }
    if (matrix == nullptr) return 0;
    const double degrees = av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (!std::isfinite(degrees)) return 0;
    int normalized = int(std::lround(-degrees)) % 360;
    if (normalized < 0) normalized += 360;
    return normalized;
}
} // namespace

FFmpegDemuxer::~FFmpegDemuxer()
{
    close();
}

bool FFmpegDemuxer::openShared(std::shared_ptr<DemuxSession> session)
{
    if (opened()) {
        close();
    }
    if (!session || session->videoStream() < 0) {
        return false;
    }
    packet_ = av_packet_alloc();
    if (packet_ == nullptr) {
        return false;
    }
    session_ = std::move(session);
    videoStreamIndex_ = session_->videoStream();
    stats_ = DemuxerStats{};
    const auto* stream = session_->stream(videoStreamIndex_);
    log::info("media", std::format("demuxer: opened shared session codecId={} streams={} videoStream={} durationUs={} timeBase={}/{} format={}",
        stream && stream->parameters ? static_cast<int>(stream->parameters->codec_id) : -1, session_->streams().size(), videoStreamIndex_,
        session_->durationUs(), stream ? stream->timeBaseNum : 0, stream ? stream->timeBaseDen : 0, session_->formatName()));
    return true;
}

bool FFmpegDemuxer::open(const std::wstring& path,const std::atomic<bool>* cancelled,bool tolerate)
{
    if (opened()) {
        close();
    }

    // avformat_open_input takes a UTF-8 path; convert from wide first.
    std::string utf8Path;
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
        utf8Path.resize(static_cast<size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), utf8Path.data(), size, nullptr, nullptr);
    }
    AVFormatContext* raw = avformat_alloc_context();
    if(!raw)return false;
    const bool network=networkMediaInput(path);
    interrupt_.configure(network,cancelled);
    raw->interrupt_callback={InputInterrupt::callback,&interrupt_};
    AVDictionary* options=nullptr;
    if(network)networkMediaOptions(&options);
    // A 档容错：让 FFmpeg 把「坏数据」当成可跳过的噪声，而不是整个输入无效。
    // 默认的 err_detect 会在第一次遇到坏包/坏索引时让 avformat_open_input 或
    // av_read_frame 直接失败，表现就是「文件损坏，无法播放」——哪怕 99% 的数据是好的。
    // ignore_err 让解复用层继续往下走；discardcorrupt 让解出的坏帧被丢弃而不是送进解码器。
    // 两者都是 FFmpeg 官方文档里的选项，mpv 用的是同一组（--demuxer-lavf-o=err_detect=ignore_err）。
    // 只在 tolerateCorruption 时开启：正常文件不需要，开着反而可能吞掉真实的格式错误。
    // Tier A resilience: make FFmpeg treat bad bytes as skippable noise instead of an
    // invalid input. The default err_detect fails the whole open/read on the first bad
    // packet, which reports "corrupted file, cannot play" even when 99% of it is fine.
    // ignore_err keeps the demuxer walking; discardcorrupt drops the bad frames instead of
    // feeding them to the decoder. Both are documented FFmpeg options, and mpv uses the
    // same pair (--demuxer-lavf-o=err_detect=ignore_err). Enabled only when tolerance was
    // requested: a healthy file does not need it and it could swallow real format errors.
    if (tolerate) {
        av_dict_set(&options,"err_detect","ignore_err",0);
        av_dict_set(&options,"discardcorrupt","1",0);
        // 读失败时跳过一段继续读，而不是结束整条流（见 readVideoPacket 的错误分支）。
        // Skip a stretch on a read failure instead of ending the stream (see the error
        // branch in readVideoPacket).
        tolerant_ = true;
        damageSkips_ = 0;
    }
    interrupt_.armConnect();
    const int openResult = avformat_open_input(&raw, utf8Path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (openResult < 0) {
        char errorText[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(openResult, errorText, sizeof(errorText));
        log::error("media", std::format("demuxer: avformat_open_input failed code={} text={}", openResult, errorText));
        return false;
    }

    interrupt_.armProbe();
    const int infoResult=avformat_find_stream_info(raw, nullptr);
    interrupt_.disarm();
    if (infoResult < 0) {
        log::error("media", "demuxer: avformat_find_stream_info failed");
        avformat_close_input(&raw);
        return false;
    }

    const int streamIndex = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        log::error("media", "demuxer: no video stream");
        avformat_close_input(&raw);
        return false;
    }

    packet_ = av_packet_alloc();
    if (packet_ == nullptr) {
        avformat_close_input(&raw);
        return false;
    }

    context_ = raw;
    videoStreamIndex_ = streamIndex;
    stats_ = DemuxerStats{};
    // 打开过程本身可能已经在 AVIOContext 上留下错误/EOF 状态（探流信息时读到坏数据）；
    // 容错模式从干净状态开始。
    // Opening may already have left an error/EOF state on the AVIOContext while probing;
    // tolerant playback starts from a clean one.
    if (tolerant_) {
        clearAvioError(raw);
        // 测试钩子：跳到指定秒数再开始，用来验证「空洞区能否跳过」。
        // Test hook: start at a given second so "can a hole be skipped" is testable
        // without waiting for playback to walk there naturally.
        if (const char* at = std::getenv("VEYRA_TEST_SEEK_TO_SEC")) {
            const int64_t targetUs = int64_t(std::atof(at) * 1e6);
            if (targetUs > 0) {
                const int64_t ts = av_rescale_q(targetUs, { 1, 1000000 }, AVRational{1, AV_TIME_BASE});
                const int rc = av_seek_frame(raw, -1, ts, AVSEEK_FLAG_BACKWARD);
                clearAvioError(raw);
                log::warn("media", std::format("test: start seek to {} s rc={}", targetUs / 1e6, rc));
            }
        }
    }
    const AVStream* stream = raw->streams[streamIndex];
    log::info("media", std::format("demuxer: opened codecId={} streams={} videoStream={} durationUs={} timeBase={}/{}",
        static_cast<int>(stream->codecpar->codec_id), raw->nb_streams, streamIndex,
        stream->duration >= 0 ? stream->duration : raw->duration,
        stream->time_base.num, stream->time_base.den));
    return true;
}

void FFmpegDemuxer::close()
{
    for (auto*& packet : elPending_) av_packet_free(&packet);
    elPending_.clear();
    elStreamIndex_ = -1;
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (context_ != nullptr) {
        avformat_close_input(&context_);
    }
    session_.reset();
    videoStreamIndex_ = -1;
    havePendingPacket_ = false;
    packetDiscontinuity_ = false;
}

int64_t FFmpegDemuxer::durationUs() const
{
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        if (stream && stream->duration > 0 && stream->timeBaseDen > 0) {
            return av_rescale_q(stream->duration, { stream->timeBaseNum, stream->timeBaseDen }, { 1, 1000000 });
        }
        return session_->durationUs();
    }
    if (context_ == nullptr) {
        return 0;
    }
    const AVStream* stream = context_->streams[videoStreamIndex_];
    if (stream->duration > 0 && stream->time_base.den > 0) {
        return av_rescale_q(stream->duration, stream->time_base, { 1, 1000000 });
    }
    return context_->duration; // AV_TIME_BASE units == microseconds
}

bool FFmpegDemuxer::seekable() const {
    if (session_) return session_->seekable();
    return context_&&context_->pb&&(context_->pb->seekable&AVIO_SEEKABLE_NORMAL);
}

double FFmpegDemuxer::averageFps() const
{
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        if (!stream || stream->avgRateDen == 0) return 0.0;
        return static_cast<double>(stream->avgRateNum) / stream->avgRateDen;
    }
    if (context_ == nullptr) {
        return 0.0;
    }
    const AVStream* stream = context_->streams[videoStreamIndex_];
    if (stream->avg_frame_rate.den == 0) {
        return 0.0;
    }
    return static_cast<double>(stream->avg_frame_rate.num) / stream->avg_frame_rate.den;
}

int FFmpegDemuxer::nominalRateNum() const {
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        return stream ? stream->nominalRateNum : 0;
    }
    return context_ && videoStreamIndex_ >= 0 ? context_->streams[videoStreamIndex_]->r_frame_rate.num : 0;
}
int FFmpegDemuxer::nominalRateDen() const {
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        return stream ? stream->nominalRateDen : 0;
    }
    return context_ && videoStreamIndex_ >= 0 ? context_->streams[videoStreamIndex_]->r_frame_rate.den : 0;
}

void FFmpegDemuxer::sampleAspect(int& num, int& den) const
{
    num = 0; den = 0;
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        if (!stream) return;
        if (stream->sarNum > 0 && stream->sarDen > 0) { num = stream->sarNum; den = stream->sarDen; return; }
        const auto* parameters = stream->parameters.get();
        if (parameters && parameters->sample_aspect_ratio.num > 0 && parameters->sample_aspect_ratio.den > 0) {
            num = parameters->sample_aspect_ratio.num; den = parameters->sample_aspect_ratio.den;
        }
        return;
    }
    if (context_ == nullptr || videoStreamIndex_ < 0) return;
    const AVRational sar = context_->streams[videoStreamIndex_]->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0) { num = sar.num; den = sar.den; return; }
    // Fall back to the codec parameters when the stream field is unset.
    const AVRational coded = context_->streams[videoStreamIndex_]->codecpar->sample_aspect_ratio;
    if (coded.num > 0 && coded.den > 0) { num = coded.num; den = coded.den; }
}

int FFmpegDemuxer::rotationDegrees() const
{
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        return stream ? rotationFrom(stream->parameters.get(), stream->rotate.c_str()) : 0;
    }
    if (context_ == nullptr || videoStreamIndex_ < 0) return 0;
    const AVStream* stream = context_->streams[videoStreamIndex_];
    const AVDictionaryEntry* rotate = av_dict_get(stream->metadata, "rotate", nullptr, 0);
    return rotationFrom(stream->codecpar, rotate != nullptr ? rotate->value : nullptr);
}

int64_t FFmpegDemuxer::startTimeUs() const {
    if (session_) return session_->startTimeUs();
    if (!context_ || context_->start_time == AV_NOPTS_VALUE) return 0;
    return av_rescale_q(context_->start_time, AV_TIME_BASE_Q, AVRational{1, 1000000});
}

int FFmpegDemuxer::streamCount() const {
    if (session_) return int(session_->streams().size());
    return context_ ? int(context_->nb_streams) : 0;
}

const AVCodecParameters* FFmpegDemuxer::streamParameters(int index) const {
    if (session_) {
        const auto* info = session_->stream(index);
        return info ? info->parameters.get() : nullptr;
    }
    if (!context_ || index < 0 || index >= int(context_->nb_streams)) return nullptr;
    return context_->streams[index]->codecpar;
}

const AVCodecParameters* FFmpegDemuxer::videoCodecParameters() const
{
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        return stream ? stream->parameters.get() : nullptr;
    }
    if (context_ == nullptr || videoStreamIndex_ < 0) {
        return nullptr;
    }
    return context_->streams[videoStreamIndex_]->codecpar;
}

int FFmpegDemuxer::videoTimeBaseNum() const
{
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        return stream ? stream->timeBaseNum : 0;
    }
    if (context_ == nullptr || videoStreamIndex_ < 0) {
        return 0;
    }
    return context_->streams[videoStreamIndex_]->time_base.num;
}

int FFmpegDemuxer::videoTimeBaseDen() const
{
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        return stream ? stream->timeBaseDen : 0;
    }
    if (context_ == nullptr || videoStreamIndex_ < 0) {
        return 0;
    }
    return context_->streams[videoStreamIndex_]->time_base.den;
}

std::string FFmpegDemuxer::formatName() const
{
    if (session_) {
        return session_->formatName();
    }
    if (context_ == nullptr || context_->iformat == nullptr || context_->iformat->name == nullptr) {
        return {};
    }
    return context_->iformat->name;
}

void FFmpegDemuxer::notePacket()
{
    ++stats_.packetsRead;
    AVRational base{};
    if (session_) {
        const auto* stream = session_->stream(videoStreamIndex_);
        base = stream ? AVRational{stream->timeBaseNum, stream->timeBaseDen} : AVRational{1, 1000000};
    } else {
        base = context_->streams[videoStreamIndex_]->time_base;
    }
    const int64_t ptsUs = av_rescale_q(packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts, base, { 1, 1000000 });
    if (stats_.packetsRead == 1) {
        stats_.firstPts = ptsUs;
    }
    else if (ptsUs < stats_.lastPts) {
        ++stats_.ptsNonMonotonicCount;
    }
    stats_.lastPts = ptsUs;
}

bool FFmpegDemuxer::readVideoPacket(bool& endOfFile)
{
    endOfFile = false;
    if (session_) {
        // Offline / blocking consumers: wait for the session, ignore the buffering hold.
        for (;;) {
            const auto result = readVideoPacketEx(50, false);
            switch (result) {
            case ReadResult::Packet: return true;
            case ReadResult::Eof: endOfFile = true; return false;
            case ReadResult::Waiting: continue;
            default: return false;
            }
        }
    }
    if (context_ == nullptr) {
        return false;
    }
    for (;;) {
        // Explicit ownership: release the previous packet's buffers before
        // reading the next one. av_read_frame would do this implicitly, but
        // the demuxer owns packet_ and makes that release unconditional and
        // observable here (Playbook: every path frees exactly once - the
        // EOF/error paths below receive a blank packet either way).
        av_packet_unref(packet_);
        interrupt_.arm();
        const int result = av_read_frame(context_, packet_);
        interrupt_.disarm();
        if (result == AVERROR_EOF) {
            // 容错模式下 EOF 未必是真的结束。没下完的文件里，空洞区读出来全零，
            // 解复用器找不到下一个合法包，就直接报 EOF —— 但对用户来说后面还有数据。
            // 这里往后跳一段再看：能读出包就继续，跳满预算还读不到才认作真的结束。
            // In tolerant mode EOF is not necessarily the end. In a partially downloaded
            // file a hole reads as all-zero bytes, the demuxer finds no next packet and
            // reports EOF - while real data exists further on. Walk forward: if a packet
            // turns up, continue; give up only when the skip budget is spent.
            if (tolerant_ && damageSkips_ < kMaxDamageSkips) {
                clearAvioError(context_);
                const int64_t fromUs = packetAtUs_ >= 0 ? packetAtUs_ : 0;
                const int64_t nextUs = fromUs + kDamageSkipStepUs;
                ++damageSkips_;
                if (damageSkips_ <= 5 || damageSkips_ % 20 == 0) {
                    log::warn("media", std::format(
                        "demuxer: EOF at {:.1f} s but tolerance is on; probing {:.1f} s (skip #{}/{})",
                        fromUs / 1e6, nextUs / 1e6, damageSkips_, kMaxDamageSkips));
                }
                if (seekToUs(nextUs)) continue;
            }
            endOfFile = true;
            return false;
        }
        if (result < 0) {
            // 损坏文件容错：解复用错误的正确处理不是放弃整条流，而是跳过这一段继续
            // 往后读。没下完/中间有空洞的文件会连续返回错误（空洞区读出来是零字节，
            // 形不成合法包），一次错误就停等于整段播不了。
            // 这里按 seek 推进：跳过一个可配置的步长再读，最多重试 N 次；步长和次数
            // 都要有界，否则一个彻底坏掉的文件会让我们在这里空转很久。
            // Damaged-file tolerance: the right reaction to a demux error is to skip that
            // stretch and keep reading, not to abandon the stream. A partially downloaded
            // file returns errors in a row over its holes (all-zero bytes never form a valid
            // packet), so stopping on the first one means nothing plays.
            // Advance by seek instead: skip a step and retry, bounded in both step size and
            // attempt count, or a hopelessly broken file would spin here for a long time.
            if (tolerant_) {
                // 错误已经发生，先把它从 AVIOContext 上清掉，否则接下来的 seek 会把
                // 这个旧错误继续抛出来。
                // The error already happened: clear it off the AVIOContext first, or the
                // upcoming seek keeps re-raising the stale one.
                clearAvioError(context_);
                if (++damageSkips_ <= kMaxDamageSkips) {
                    ++stats_.damagedRegionsSkipped;
                    const int64_t stepUs = kDamageSkipStepUs;
                    const int64_t nowUs = packetAtUs_ >= 0 ? packetAtUs_ : 0;
                    const int64_t nextUs = nowUs + stepUs;
                    if (damageSkips_ <= 5 || damageSkips_ % 20 == 0) {
                        char errorText[AV_ERROR_MAX_STRING_SIZE]{};
                        av_strerror(result, errorText, sizeof(errorText));
                        log::warn("media", std::format(
                            "demuxer: damaged region (code={} text={}); skipping {:.1f} s ahead to {:.1f} s (skip #{}/{})",
                            result, errorText, stepUs / 1e6, nextUs / 1e6,
                            damageSkips_, kMaxDamageSkips));
                    }
                    if (seekToUs(nextUs)) continue;   // 从跳过后的位置重新读
                    // seek 本身失败（比如超出时长）：不再重试，按原样报错。
                    log::error("media", "demuxer: could not skip past the damaged region");
                } else {
                    log::error("media", std::format(
                        "demuxer: giving up after {} damaged regions", kMaxDamageSkips));
                }
            }
            char errorText[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(result, errorText, sizeof(errorText));
            log::error("media", std::format("demuxer: av_read_frame failed code={} text={}", result, errorText));
            return false;
        }
        // 记下当前位置，供「跳过损坏区段」决定从哪里往后跳。
        // Remember where we are so "skip the damaged stretch" knows where to jump from.
        if (tolerant_ && packet_->stream_index == videoStreamIndex_ && packet_->pts != AV_NOPTS_VALUE)
            packetAtUs_ = av_rescale_q(packet_->pts, context_->streams[videoStreamIndex_]->time_base,
                                       AVRational{1, 1000000}) - startTimeUs();
        if (packet_->stream_index != videoStreamIndex_) {
            if (elStreamIndex_ >= 0 && packet_->stream_index == elStreamIndex_) {
                // Bounded: when the layer runs ahead of the base stream the queue
                // must not grow for the whole film.
                if (elPending_.size() >= 8) {
                    av_packet_free(&elPending_.front());
                    elPending_.pop_front();
                }
                elPending_.push_back(av_packet_clone(packet_));
                static unsigned elQ = 0;
                if (++elQ <= 3) log::info("source-dovi-el", std::format(
                    "EL packet queued size={} pending={}", packet_->size, elPending_.size()));
            }
            av_packet_unref(packet_);
            continue;
        }
        notePacket();
        // Custom (P5): does the enhancement layer reach us inside the packet?
        if (std::getenv("VEYRA_DOVI_EL_TRACE") != nullptr) {
            static const unsigned lengthSize = hevcLengthSize(context_->streams[videoStreamIndex_]->codecpar);
            const auto scan = scanHevcLayers(packet_->data, packet_->size, lengthSize);
            static unsigned withEl = 0, withoutEl = 0, logged = 0;
            if (scan.enhancement) ++withEl; else ++withoutEl;
            if (logged < 4 || stats_.packetsRead % 200 == 0) {
                ++logged;
                log::info("dovi-el", std::format(
                    "packet={} bytes={} lengthSize={} nal={} enhancementNal={} maxLayer={} withEl={} withoutEl={}",
                    stats_.packetsRead, packet_->size, lengthSize, scan.nal, scan.enhancement, scan.maxLayer, withEl, withoutEl));
            }
            // Matroska can carry the dependent layer as a BlockAdditional; FFmpeg
            // surfaces those as packet side data. Report them so the next step is
            // decided by evidence instead of guesswork.
            static unsigned sideReported = 0;
            if (sideReported < 3) {
                ++sideReported;
                log::info("dovi-el", std::format("  packet={} sideDataElems={}", stats_.packetsRead, packet_->side_data_elems));
                for (int i = 0; i < packet_->side_data_elems; ++i)
                    log::info("dovi-el", std::format("    side[{}] type={} size={}", i,
                        static_cast<int>(packet_->side_data[i].type), packet_->side_data[i].size));
            }
        }
        return true;
    }
}

bool FFmpegDemuxer::takeElPacket(AVPacket** out) {
    if (out == nullptr) return false;
    *out = nullptr;
    if (!session_) {
        // Direct mode (local files): the reader queues the layer's packets.
        if (elPending_.empty()) return false;
        *out = elPending_.front();
        elPending_.pop_front();
        return true;
    }
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) return false;
    DemuxSession::Meta meta;
    // A short wait, not zero: in a transport stream the base and enhancement
    // packets interleave, so the layer's packet for this frame often lands just
    // after the base one that triggered the call.
    const auto pop = session_->popEl(packet, meta, 10);
    if (pop != DemuxSession::Pop::Packet) {
        static unsigned missDbg = 0;
        if (++missDbg <= 3) log::info("source-dovi-el", std::format("takeElPacket miss pop={} elIndex={}", int(pop), session_->elIndex()));
        av_packet_free(&packet);
        return false;
    }
    *out = packet;
    return true;
}

FFmpegDemuxer::ReadResult FFmpegDemuxer::readVideoPacketEx(int waitMs, bool honourHold)
{
    if (!session_) {
        bool eof = false;
        if (readVideoPacket(eof)) return ReadResult::Packet;
        return eof ? ReadResult::Eof : ReadResult::Error;
    }
    av_packet_unref(packet_);
    DemuxSession::Meta meta;
    switch (session_->popVideo(packet_, meta, waitMs, honourHold)) {
    case DemuxSession::Pop::Packet:
        packetDiscontinuity_ = meta.discontinuity;
        notePacket();
        return ReadResult::Packet;
    case DemuxSession::Pop::Empty: return ReadResult::Waiting;
    case DemuxSession::Pop::Eof: return ReadResult::Eof;
    case DemuxSession::Pop::Cancelled: return ReadResult::Cancelled;
    case DemuxSession::Pop::Failed: break;
    }
    return ReadResult::Error;
}

bool FFmpegDemuxer::seekToUs(int64_t targetUs,bool byUser)
{
    if(byUser)damageSkips_=0;   // 用户 seek：重置跳过预算
    if (session_) {
        if (!session_->seek(targetUs)) {
            log::error("media", std::format("demuxer: session seek failed targetUs={}", targetUs));
            return false;
        }
        packetDiscontinuity_ = false;
        return true;
    }
    if (context_ == nullptr) {
        return false;
    }
    // Seek every stream, not just the video one: with a separate enhancement
    // layer (stream 1) seeking only the base stream left the layer reading from
    // its old position, and the PTS pairing then failed for every frame after
    // the seek. -1 means "all streams" and takes an AV_TIME_BASE timestamp.
    // Packets queued before the seek carry the old timeline; keeping them would
    // make the first frames after a seek pair against stale layer pictures.
    for (auto*& stale : elPending_) av_packet_free(&stale);
    elPending_.clear();
    const int64_t target = av_rescale_q(targetUs + startTimeUs(), { 1, 1000000 },
        AVRational{1, AV_TIME_BASE});
    // 容错模式下 seek 要扫过空洞，必须给预算：无上限会把读取线程卡几十秒，
    // 主循环在等它，表现就是画面冻住（实测 engine-hang 8 秒、帧率掉到 50）。
    // Tolerant seeks scan across holes and need a budget: unbounded blocking stalls the read
    // thread for tens of seconds while the main loop waits (measured: 8 s engine-hang,
    // frame rate down to 50).
    if (tolerant_) interrupt_.armBounded(); else interrupt_.arm();
    const int result = av_seek_frame(context_, -1, target, AVSEEK_FLAG_BACKWARD);
    interrupt_.disarm();
    if (result < 0) {
        char errorText[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, errorText, sizeof(errorText));
        log::error("media", std::format("demuxer: seek failed targetUs={} code={} text={}", targetUs, result, errorText));
        return false;
    }
    // 记录 seek 落点，容错模式下如果这里读不出东西，就从这儿继续往后找可用区域。
    // Remember where the seek landed: in tolerant mode a hole here means "walk forward
    // from this point until something readable turns up".
    packetAtUs_ = targetUs;
    // 注意：这里**不**重置 damageSkips_。跳过损坏区段内部也是走 seekToUs 的，
    // 在这里清零会让「跳过预算」永远用不完——一个彻底坏掉的文件就这么死循环下去。
    // 由用户手动 seek 时在上层重置（见 setUserSeek / 打开时初始化）。
    // NOTE: damageSkips_ is deliberately NOT reset here. Our own skip logic goes through
    // seekToUs too, so clearing it would make the skip budget infinite and a hopeless file
    // would loop forever. A user seek resets it at the call site instead.
    // 用户手动 seek、以及自动跳过损坏区段，都要清掉粘性错误，否则跳到完好区域也读不出来。
    // A manual seek - and our own skip past a damaged region - must clear the sticky error,
    // or reading from an intact position still fails.
    clearAvioError(context_);
    return true;
}

} // namespace veyra::media
