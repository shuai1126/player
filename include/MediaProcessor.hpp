#include "ffmpegUtil.h"

#include <iostream>
#include <string>
#include <list>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>

using std::condition_variable;
using std::cout;
using std::endl;
using std::list;
using std::mutex;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

class MediaProcessor {
    list<unique_ptr<AVPacket>> packetList{};
    mutex pktListMutex{};
    int PKT_WAITING_SIZE = 3;
    bool started = false;
    bool closed = false;
    bool streamFinished = false;

    AVFrame* nextFrame = av_frame_alloc();
    AVPacket* targetPkt = nullptr;

    void nextFrameKeeper() {
        auto lastPrepareTime = std::chrono::system_clock::now();
        while (!streamFinished && started) {
            std::unique_lock<std::mutex> lk{nextDataMutex};
            cv.wait(lk, [this] { return !started || !isNextDataReady.load(); });
            if (!started) {
                break;
            }
            auto prepareTime = std::chrono::system_clock::now();
            std::chrono::duration<double> diff = prepareTime - lastPrepareTime;
            lastPrepareTime = prepareTime;
            prepareNextData();
        }
        cout << "[THREAD] next frame keeper finished, index=" << streamIndex << endl;
        started = false;
        closed = true;
    }

protected:
    std::atomic<uint64_t> currentTimestamp{0};
    std::atomic<uint64_t> nextFrameTimestamp{0};
    AVRational streamTimeBase{1, 0};
    bool noMorePkt = false;

    int streamIndex = -1;
    AVCodecContext* codecCtx = nullptr;

    condition_variable cv{};
    mutex nextDataMutex{};

    std::atomic<bool> isNextDataReady{false};

    virtual void generateNextData(AVFrame* f) = 0;

    unique_ptr<AVPacket> getNextPkt() {
        if (noMorePkt) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lg(pktListMutex);
        if (packetList.empty()) {
            return nullptr;
        } else {
            auto pkt = std::move(packetList.front());
            if (pkt == nullptr) {
                noMorePkt = true;
                return nullptr;
            } else {
                packetList.pop_front();
                return pkt;
            }
        }
    }

    void prepareNextData() {
        while (!isNextDataReady.load() && !streamFinished) {
            if (targetPkt == nullptr) {
                if (!noMorePkt) {
                    auto pkt = getNextPkt();
                    if (pkt != nullptr) {
                        targetPkt = pkt.release();
                    } else if (noMorePkt) {
                        targetPkt = nullptr;
                    } else {
                        return;
                    }
                } else {
                    cout << "++++++++ no more pkt index=" << streamIndex
                         << " finished=" << streamFinished << endl;
                }
            }

            int ret = -1;
            ret = avcodec_send_packet(codecCtx, targetPkt);
            if (ret == 0) {
                av_packet_free(&targetPkt);
                targetPkt = nullptr;
            } else if (ret == AVERROR(EAGAIN)) {

            } else if (ret == AVERROR_EOF) {

                cout << "[WARN]  no new packets can be sent to it. index=" << streamIndex << endl;
            } else {
                string errorMsg = "+++++++++ ERROR avcodec_send_packet error: ";
                errorMsg += ret;
                cout << errorMsg << endl;
                throw std::runtime_error(errorMsg);
            }

            ret = avcodec_receive_frame(codecCtx, nextFrame);
            if (ret == 0) {

                generateNextData(nextFrame);
                isNextDataReady.store(true);
            } else if (ret == AVERROR_EOF) {
                cout << "+++++++++++++++++++++++++++++ MediaProcessor no more output frames. index="
                     << streamIndex << endl;
                streamFinished = true;
            } else if (ret == AVERROR(EAGAIN)) {

            } else {
                string errorMsg = "avcodec_receive_frame error: ";
                errorMsg += ret;
                cout << errorMsg << endl;
                throw std::runtime_error(errorMsg);
            }
        }
    }

public:
    ~MediaProcessor() {

        if (nextFrame != nullptr) {
            av_frame_free(&nextFrame);
        }

        if (targetPkt != nullptr) {
            av_packet_free(&targetPkt);
        }

        if (codecCtx != nullptr) {
            avcodec_free_context(&codecCtx);
        }

        // important
        for (auto& p : packetList) {
            auto pkt = p.release();
            av_packet_free(&pkt);
        }

        cout << "~MediaProcessor called. index=" << streamIndex << endl;
    }
    void start() {
        started = true;
        std::thread keeper{&MediaProcessor::nextFrameKeeper, this};
        keeper.detach();
    }

    bool close() {
        started = false;
        int c = 5;
        while (!closed && c > 0) {
            c--;
            cv.notify_one();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return closed;
    }

    bool isClosed() { return closed; }

    void pushPkt(unique_ptr<AVPacket> pkt) {
        std::lock_guard<std::mutex> lg(pktListMutex);
        packetList.push_back(std::move(pkt));
    }
    bool isStreamFinished() { return streamFinished; }

    bool needPacket() {
        bool need;
        std::lock_guard<std::mutex> lg(pktListMutex);
        need = packetList.size() < PKT_WAITING_SIZE;
        return need;
    }

    uint64_t getPts() { return currentTimestamp.load(); }
};

class AudioProcessor : public MediaProcessor {
    std::unique_ptr<ffmpegUtil::ReSampler> reSampler{};

    uint8_t* outBuffer = nullptr;
    int outBufferSize = -1;
    int outDataSize = -1;
    int outSamples = -1;

    ffmpegUtil::AudioInfo inAudio;
    ffmpegUtil::AudioInfo outAudio;

protected:
    void generateNextData(AVFrame* frame) final override {
        if (outBuffer == nullptr) {
            outBufferSize = reSampler->allocDataBuf(&outBuffer, frame->nb_samples);
        } else {
            memset(outBuffer, 0, outBufferSize);
        }
        std::tie(outSamples, outDataSize) = reSampler->reSample(outBuffer, outBufferSize, frame);
        auto t = frame->pts * av_q2d(streamTimeBase) * 1000;
        nextFrameTimestamp.store((uint64_t)t);
    }


public:
    AudioProcessor(const AudioProcessor&) = delete;
    AudioProcessor(AudioProcessor&&) noexcept = delete;
    AudioProcessor operator=(const AudioProcessor&) = delete;
    ~AudioProcessor() {
        if (outBuffer != nullptr) {
            av_freep(&outBuffer);
        }
        cout << "AudioProcessor() called." << endl;
    }

    AudioProcessor(AVFormatContext* formatCtx) {
        for (int i = 0; i < formatCtx->nb_streams; i++) {
            if (formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                streamTimeBase = formatCtx->streams[i]->time_base;
                streamIndex = i;
                cout << "audio stream index = : " << i << " tb.n" << streamTimeBase.num << endl;
                break;
            }
        }
        if (streamIndex < 0) {
            cout << " can not find audio stream." << endl;
        }

        ffmpegUtil::ffUtils::initCodecContext(formatCtx, streamIndex, &codecCtx);

        int64_t inLayout = codecCtx->channel_layout;
        int inSampleRate = codecCtx->sample_rate;
        int inChannels = codecCtx->channels;
        AVSampleFormat inFormat = codecCtx->sample_fmt;

        inAudio = ffmpegUtil::AudioInfo(inLayout, inSampleRate, inChannels, inFormat);
        outAudio = ffmpegUtil::ReSampler::getDefaultAudioInfo(inSampleRate);

        reSampler.reset(new ffmpegUtil::ReSampler(inAudio, outAudio));
    }

    int getAudioIndex() const { return streamIndex; }

    int getSamples() { return outSamples; }

    void writeAudioData(uint8_t* stream, int len) {
        static uint8_t* silenceBuff = nullptr;
        if (silenceBuff == nullptr) {
            silenceBuff = (uint8_t*)av_malloc(sizeof(uint8_t) * len);
            std::memset(silenceBuff, 0, len);
        }

        if (isNextDataReady.load()) {
            std::lock_guard<std::mutex> lock(nextDataMutex);
            currentTimestamp.store(nextFrameTimestamp.load());
            if (outDataSize != len) {
                cout << "WARNING: outDataSize[" << outDataSize << "] != len[" << len << "]" << endl;
            }
            std::memcpy(stream, outBuffer, outDataSize);
            isNextDataReady.store(false);
        } else {

            cout << " writeAudioData, audio data not ready." << endl;
            std::memcpy(stream, silenceBuff, len);
        }
        cv.notify_one();
    }

    int getOutChannels() const { return outAudio.channels; }

    int getOutSampleRate() const { return outAudio.sampleRate; }

    int getSampleFormat() const {
        if (codecCtx != nullptr) {
            return (int)codecCtx->sample_fmt;
        } else {
            throw std::runtime_error("can not getSampleRate.");
        }
    }
};

class VideoProcessor : public MediaProcessor {
    struct SwsContext* sws_ctx = nullptr;
    AVFrame* outPic = nullptr;

protected:
    void generateNextData(AVFrame* frame) override {
        auto t = frame->pts * av_q2d(streamTimeBase) * 1000;
        nextFrameTimestamp.store((uint64_t)t);
        sws_scale(sws_ctx, (uint8_t const* const*)frame->data, frame->linesize, 0,
                  codecCtx->height, outPic->data, outPic->linesize);
    }

public:
    VideoProcessor(const VideoProcessor&) = delete;
    VideoProcessor(VideoProcessor&&) noexcept = delete;
    VideoProcessor operator=(const VideoProcessor&) = delete;
    ~VideoProcessor() {
        if (sws_ctx != nullptr) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }

        if (outPic != nullptr) {
            av_frame_free(&outPic);
        }
        cout << "VideoProcessor() called." << endl;
    }

    VideoProcessor(AVFormatContext* formatCtx) {
        for (int i = 0; i < formatCtx->nb_streams; i++) {
            if (formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                streamIndex = i;
                streamTimeBase = formatCtx->streams[i]->time_base;
                cout << "video stream index = :" << i << "tb.n" << streamTimeBase.num << endl;
                break;
            }
        }

        if (streamIndex < 0) {
            cout << " can not find video stream." << endl;
        }

        ffmpegUtil::ffUtils::initCodecContext(formatCtx, streamIndex, &codecCtx);

        int w = codecCtx->width;
        int h = codecCtx->height;

        sws_ctx = sws_getContext(w, h, codecCtx->pix_fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                 NULL, NULL, NULL);

        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 32);
        outPic = av_frame_alloc();
        uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
        av_image_fill_arrays(outPic->data, outPic->linesize, buffer, AV_PIX_FMT_YUV420P, w, h, 32);
    }

    int getVideoIndex() const { return streamIndex; }

    AVFrame* getFrame() {
        if (isNextDataReady.load()) {
            currentTimestamp.store(nextFrameTimestamp.load());
            return outPic;
        } else {
            cout << " getFrame, video data not ready." << endl;
            return nullptr;
        }
    }

    bool refreshFrame() {
        if (isNextDataReady.load()) {
            currentTimestamp.store(nextFrameTimestamp.load());
            isNextDataReady.store(false);
            cv.notify_one();
            return true;
        } else {
            cv.notify_one();
            return false;
        }
    }

    int getWidth() const {
        if (codecCtx != nullptr) {
            return codecCtx->width;
        } else {
            throw std::runtime_error("can not getWidth.");
        }
    }

    int getHeight() const {
        if (codecCtx != nullptr) {
            return codecCtx->height;
        } else {
            throw std::runtime_error("can not getHeight.");
        }
    }

    double getFrameRate() const {
        if (codecCtx != nullptr) {
            auto frameRate = codecCtx->framerate;
            double fr = frameRate.num && frameRate.den ? av_q2d(frameRate) : 0.0;
            return fr;
        } else {
            throw std::runtime_error("can not getFrameRate.");
        }
    }
};
