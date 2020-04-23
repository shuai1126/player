//
// Created by 陈志帅 on 2020/4/20.
//

#include "ffmpegUtil.h"
#include <iostream>
#include <memory>
#include <chrono>
#include <thread>
#include "MediaProcessor.hpp"

extern "C"{
#include "SDL2/SDL.h"
};

#define REFRESH_EVENT (SDL_USEREVENT + 1)

#define BREAK_EVENT (SDL_USEREVENT + 2)

namespace {

    using namespace std;
    using namespace ffmpegUtil;

    void callback(void* userData, Uint8* stream, int len) {
        AudioProcessor* receiver = (AudioProcessor*)userData;
        receiver->writeAudioData(stream, len);
    }

    void readPkt(PacketGrabber& packetGrabber, AudioProcessor* audioProcessor, VideoProcessor* videoProcessor){
        const int CHECK_PERIOD = 10;

        cout << "read pkt thread started." << endl;
        int audioIndex = audioProcessor->getAudioIndex();
        int videoIndex = videoProcessor->getVideoIndex();

        while (!packetGrabber.isFileEnd() && !audioProcessor->isClosed() && !videoProcessor->isClosed()) {
            while (audioProcessor->needPacket() || videoProcessor->needPacket()) {
                AVPacket* packet = (AVPacket*)av_malloc(sizeof(AVPacket));
                int t = packetGrabber.grabPacket(packet);
                if (t == -1) {
                    cout << "file finish." << endl;
                    audioProcessor->pushPkt(nullptr);
                    videoProcessor->pushPkt(nullptr);
                    break;
                } else if (t == audioIndex && audioProcessor != nullptr) {
                    unique_ptr<AVPacket> uPacket(packet);
                    audioProcessor->pushPkt(std::move(uPacket));
                } else if (t == videoIndex && videoProcessor != nullptr) {
                    unique_ptr<AVPacket> uPacket(packet);
                    videoProcessor->pushPkt(std::move(uPacket));
                } else {
                    av_packet_free(&packet);
                    cout << "unknown streamIndex:" << t << endl;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_PERIOD));
        }
        cout << "read pkt thread finished." << endl;
    }

    void refreshPicture(int time, bool& exit, bool& faster){
        cout << "refreshPic time:" << time << endl;
        while (!exit){
            SDL_Event event;
            event.type = REFRESH_EVENT;
            SDL_PushEvent(&event);
            if(faster){
                std::this_thread::sleep_for(std::chrono::milliseconds(time / 2)) ;
            }else{
                std::this_thread::sleep_for(std::chrono::milliseconds(time));
            }
        }
        cout << "refreshPicture thread finish" << endl;
    }

    void videoPlay (VideoProcessor& videoProcessor, AudioProcessor* audio = nullptr) {

        auto width = videoProcessor.getWidth();
        auto height = videoProcessor.getHeight();

        SDL_Window* window;

        window = SDL_CreateWindow("player", SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED, width, height,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!window) {
            string errMsg = "could not create window";
            errMsg += SDL_GetError();
            cout << errMsg << endl;
            throw std::runtime_error(errMsg);
        }

        SDL_Renderer* sdlRenderer = SDL_CreateRenderer(window, -1, 0);

        Uint32 pixFormat = SDL_PIXELFORMAT_IYUV;

        SDL_Texture* sdlTexture = SDL_CreateTexture(sdlRenderer, pixFormat, SDL_TEXTUREACCESS_STREAMING, width, height);

        SDL_Event event;
        auto frameRate = videoProcessor.getFrameRate();
        cout << "frame rate " << frameRate <<  endl;

        bool exit = false;
        bool faster = false;
        std::thread refreshThread{refreshPicture, (int)(1000 / frameRate), std::ref(exit), std::ref(faster)};

        int failCount = 0;
        int fastCount = 0;
        int slowCount = 0;
        while (!videoProcessor.isStreamFinished()) {
            SDL_WaitEvent(&event);

            if (event.type == REFRESH_EVENT) {
                if (videoProcessor.isStreamFinished()) {
                    exit = true;
                    continue;
                }

                if (audio != nullptr) {
                    auto vTs = videoProcessor.getPts();
                    auto aTs = audio->getPts();
                    if (vTs > aTs && vTs - aTs > 30) {
                        cout << "VIDEO FASTER ================= vTs - aTs [" << (vTs - aTs)
                             << "]ms, SKIP A EVENT" << endl;
                        faster = false;
                        slowCount++;
                        continue;
                    } else if (vTs < aTs && aTs - vTs > 30) {
                        cout << "VIDEO SLOWER ================= aTs - vTs =[" << (aTs - vTs) << "]ms, Faster" << endl;
                        faster = true;
                        fastCount++;
                    } else {
                        faster = false;
                    }
                }

                AVFrame* frame = videoProcessor.getFrame();

                if (frame != nullptr) {
                    SDL_UpdateYUVTexture(sdlTexture,NULL,

                            frame->data[0], frame->linesize[0],

                            frame->data[1], frame->linesize[1],

                            frame->data[2],frame->linesize[2]);

                    SDL_RenderClear(sdlRenderer);
                    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
                    SDL_RenderPresent(sdlRenderer);

                    if (!videoProcessor.refreshFrame()) {
                        cout << "vProcessor.refreshFrame false" << endl;
                    }
                } else {
                    failCount++;
                    cout << "getFrame fail. failCount = " << failCount << endl;
                }
            } else if (event.type == SDL_QUIT) {
                cout << "SDL screen got a SDL_QUIT." << endl;
                exit = true;
                break;
            } else if (event.type == BREAK_EVENT) {
                break;
            }
        }

        refreshThread.join();
        cout << "Sdl video thread finish: failCount = " << failCount << ", fastCount = " << fastCount
             << ", slowCount = " << slowCount << endl;
    }

    void audioPlay(SDL_AudioDeviceID& audioDeviceId, AudioProcessor& audioProcessor){
        SDL_AudioSpec spec;
        SDL_AudioSpec wantedSpec;

        cout << "audioProcessor.getSampleFormat()" << audioProcessor.getSampleFormat() << endl;
        cout << "audioProcessor.getOutSampleRate()" << audioProcessor.getOutSampleRate() << endl;
        cout << "audioProcessor.getOutChannels()" << audioProcessor.getOutChannels() << endl;

        int samples = -1;

        while (true) {
            cout << "get audio samples" << endl;
            samples = audioProcessor.getSamples();
            if(samples <= 0){
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else{
                cout << "get audio samples" << samples << endl;
                break;
            }
        }

        wantedSpec.freq = audioProcessor.getOutSampleRate();
        wantedSpec.format = AUDIO_S16SYS;
        wantedSpec.channels = audioProcessor.getOutChannels();
        wantedSpec.samples = samples;
        wantedSpec.callback = callback;
        wantedSpec.userdata = &audioProcessor;

        audioDeviceId = SDL_OpenAudioDevice(nullptr, 0, &wantedSpec, &spec, 0);

        if (audioDeviceId == 0) {
            string errMsg = "Failed to open audio device:";
            errMsg += SDL_GetError();
            cout << errMsg << endl;
            throw std::runtime_error(errMsg);
        }

        cout << "wantedSpec.freq:" << wantedSpec.freq << endl;
        std::printf("wantedSpec.format: Ox%X\n", wantedSpec.format);
        cout << "wantedSpec.channels:" << wantedSpec.channels << endl;
        cout << "wantedSpec.samples:" << wantedSpec.samples << endl;

        cout << "spec.freq:" << spec.freq << endl;
        std::printf("spec.format: Ox%X\n", spec.format);
        cout << "spec.channels:" << spec.channels << endl;
        cout << "spec.silence:" << spec.silence << endl;
        cout << "spec.samples:" << spec.samples << endl;

        SDL_PauseAudioDevice(audioDeviceId, 0);
        cout << "audio start thread finish." << endl;
    }


    int playVideoAndAudio(const string& inputPath){

        PacketGrabber packetGrabber{inputPath};
        auto formatCtx = packetGrabber.getFormatCtx();
        av_dump_format(formatCtx, 0, "", 0);

        VideoProcessor videoProcessor(formatCtx);
        videoProcessor.start();

        AudioProcessor audioProcessor(formatCtx);
        audioProcessor.start();

        std::thread readerThread{readPkt, std::ref(packetGrabber), &audioProcessor,
                                 &videoProcessor};

        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
            string errMsg = "Could not initialize SDL -";
            errMsg += SDL_GetError();
            cout << errMsg << endl;
            throw std::runtime_error(errMsg);
        }

        SDL_AudioDeviceID audioDeviceId;

        std::thread startAudioThread(audioPlay, std::ref(audioDeviceId),std::ref(audioProcessor));
        startAudioThread.join();

        videoPlay(videoProcessor, &audioProcessor);

        cout << "videoThread join." << endl;

        SDL_PauseAudioDevice(audioDeviceId, 1);
        SDL_CloseAudio();

        bool r;
        r = audioProcessor.close();
        cout << "audioProcessor closed: " << r << endl;
        r = videoProcessor.close();
        cout << "videoProcessor closed: " << r << endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        readerThread.join();
        cout << "Pause and Close audio" << endl;

        return 0;
    }
}


void play(const string& inputPath){
    cout << "input path:" << inputPath << endl;
    playVideoAndAudio(inputPath);
}

