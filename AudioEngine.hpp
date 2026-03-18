#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "winmm.lib")

class AudioEngine
{
    using AudioCallback = std::function<void(void *, int)>;

    static constexpr DWORD SAMPLE_RATE = 44100;
    static constexpr WORD CHANNELS = 1;
    static constexpr WORD BITS_PER_SAMPLE = 16;
    static constexpr int FRAME_BYTES = 1280;
    static constexpr int FRAME_SAMPLES =
        FRAME_BYTES / static_cast<int>(sizeof(int16_t));
    static constexpr size_t INPUT_BUFFER_COUNT = 6;
    static constexpr size_t OUTPUT_BUFFER_COUNT = 12;
    static constexpr size_t STARTUP_BUFFER_FRAMES = 6;
    static constexpr size_t TARGET_SOURCE_BUFFER_FRAMES = 6;
    static constexpr size_t MAX_SOURCE_BUFFER_FRAMES = 18;
    static constexpr auto SOURCE_IDLE_TIMEOUT = std::chrono::seconds(3);
    static constexpr size_t ECHO_HISTORY_FRAMES = 24;
    static constexpr int64_t ECHO_REFERENCE_MIN_ENERGY = 8'000'000;
    static constexpr int64_t ECHO_MIC_MIN_ENERGY = 4'000'000;
    static constexpr float DEFAULT_ECHO_CORRELATION_THRESHOLD = 0.55f;
    static constexpr float DEFAULT_ECHO_SUBTRACTION_MAX_GAIN = 1.25f;
    static constexpr float DEFAULT_ECHO_RESIDUAL_ATTENUATION = 0.45f;

    struct SourceBuffer
    {
        std::deque<std::array<char, FRAME_BYTES>> frames;
        std::chrono::steady_clock::time_point lastSeen =
            std::chrono::steady_clock::now();
    };

    struct EchoReferenceFrame
    {
        std::array<int16_t, FRAME_SAMPLES> samples{};
        int64_t energy = 0;
    };

    HWAVEIN hIn = nullptr;
    HWAVEOUT hOut = nullptr;
    WAVEFORMATEX wfx{};

    std::vector<WAVEHDR *> inHeaders;
    std::array<WAVEHDR, OUTPUT_BUFFER_COUNT> outHeaders{};
    std::array<std::array<char, FRAME_BYTES>, OUTPUT_BUFFER_COUNT> outBuffers{};
    std::array<bool, OUTPUT_BUFFER_COUNT> outBufferFree{};

    std::unordered_map<std::string, SourceBuffer> playbackSources;
    std::unordered_map<std::string, float> sourceVolumes;
    std::deque<EchoReferenceFrame> echoReferenceFrames;
    bool echoSuppressionEnabled = true;
    float echoCorrelationThreshold = DEFAULT_ECHO_CORRELATION_THRESHOLD;
    float echoSubtractionMaxGain = DEFAULT_ECHO_SUBTRACTION_MAX_GAIN;
    float echoResidualAttenuation = DEFAULT_ECHO_RESIDUAL_ATTENUATION;
    bool playbackPrimed = false;

    std::atomic<bool> recording{false};
    std::atomic<bool> playbackRunning{false};
    std::atomic<unsigned long long> playbackOverflowDrops{0};
    std::atomic<unsigned long long> playbackUnderruns{0};

    CRITICAL_SECTION callbackCS;
    AudioCallback onCapture;

    std::mutex playbackMutex;
    std::condition_variable playbackCv;
    std::thread playbackWorker;
    std::mutex echoMutex;

    std::atomic<unsigned long long> echoSuppressedFrames{0};

  public:
    AudioEngine()
    {
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = CHANNELS;
        wfx.nSamplesPerSec = SAMPLE_RATE;
        wfx.wBitsPerSample = BITS_PER_SAMPLE;
        wfx.nBlockAlign =
            static_cast<WORD>((wfx.nChannels * wfx.wBitsPerSample) / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize = 0;

        InitializeCriticalSection(&callbackCS);
        outBufferFree.fill(true);
    }

    ~AudioEngine()
    {
        StopRecording();
        StopPlaying();
        DeleteCriticalSection(&callbackCS);
    }

    void StartRecording(AudioCallback cb)
    {
        {
            EnterCriticalSection(&callbackCS);
            onCapture = std::move(cb);
            LeaveCriticalSection(&callbackCS);
        }

        if (hIn)
        {
            recording = true;
            return;
        }

        recording = true;

        MMRESULT res = waveInOpen(
            &hIn, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(waveInProc),
            reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (res != MMSYSERR_NOERROR)
        {
            std::cerr << "waveInOpen failed: " << res << std::endl;
            recording = false;
            hIn = nullptr;
            return;
        }

        inHeaders.reserve(INPUT_BUFFER_COUNT);
        for (size_t i = 0; i < INPUT_BUFFER_COUNT; ++i)
        {
            auto *hdr = new WAVEHDR();
            ZeroMemory(hdr, sizeof(WAVEHDR));
            hdr->lpData = new char[FRAME_BYTES];
            hdr->dwBufferLength = FRAME_BYTES;

            if (waveInPrepareHeader(hIn, hdr, sizeof(WAVEHDR)) !=
                MMSYSERR_NOERROR)
            {
                delete[] hdr->lpData;
                delete hdr;
                continue;
            }

            if (waveInAddBuffer(hIn, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            {
                waveInUnprepareHeader(hIn, hdr, sizeof(WAVEHDR));
                delete[] hdr->lpData;
                delete hdr;
                continue;
            }

            inHeaders.push_back(hdr);
        }

        if (inHeaders.empty())
        {
            std::cerr << "waveInAddBuffer failed for all buffers" << std::endl;
            waveInClose(hIn);
            hIn = nullptr;
            recording = false;
            return;
        }

        MMRESULT startRes = waveInStart(hIn);
        if (startRes != MMSYSERR_NOERROR)
        {
            std::cerr << "[Audio] waveInStart failed: " << startRes
                      << std::endl;
            StopRecording();
            return;
        }
        std::cout << "[Audio] Recording started" << std::endl;
    }

    void StopRecording()
    {
        recording = false;

        {
            EnterCriticalSection(&callbackCS);
            onCapture = nullptr;
            LeaveCriticalSection(&callbackCS);
        }

        if (!hIn)
        {
            return;
        }

        HWAVEIN hTemp = hIn;
        hIn = nullptr;

        waveInStop(hTemp);
        waveInReset(hTemp);

        for (auto *hdr : inHeaders)
        {
            waveInUnprepareHeader(hTemp, hdr, sizeof(WAVEHDR));
            delete[] hdr->lpData;
            delete hdr;
        }
        inHeaders.clear();

        waveInClose(hTemp);
        std::cout << "[Audio] Recording stopped" << std::endl;
    }

    void StartPlaying()
    {
        if (hOut)
        {
            return;
        }

        MMRESULT res = waveOutOpen(
            &hOut, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(waveOutProc),
            reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (res != MMSYSERR_NOERROR)
        {
            std::cerr << "waveOutOpen failed: " << res << std::endl;
            hOut = nullptr;
            return;
        }

        bool playbackInitOk = true;
        for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
        {
            ZeroMemory(&outHeaders[i], sizeof(WAVEHDR));
            outHeaders[i].lpData = outBuffers[i].data();
            outHeaders[i].dwBufferLength = FRAME_BYTES;
            outHeaders[i].dwUser = static_cast<DWORD_PTR>(i);

            res = waveOutPrepareHeader(hOut, &outHeaders[i], sizeof(WAVEHDR));
            if (res != MMSYSERR_NOERROR)
            {
                std::cerr << "[Audio] waveOutPrepareHeader failed during init: "
                          << res << std::endl;
                playbackInitOk = false;
                break;
            }
        }

        if (!playbackInitOk)
        {
            waveOutReset(hOut);
            for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
            {
                if (outHeaders[i].dwFlags & WHDR_PREPARED)
                {
                    waveOutUnprepareHeader(hOut, &outHeaders[i],
                                           sizeof(WAVEHDR));
                }
            }
            waveOutClose(hOut);
            hOut = nullptr;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(playbackMutex);
            playbackSources.clear();
            playbackPrimed = false;
            outBufferFree.fill(true);
        }
        {
            std::lock_guard<std::mutex> lock(echoMutex);
            echoReferenceFrames.clear();
        }

        playbackOverflowDrops = 0;
        playbackUnderruns = 0;
        echoSuppressedFrames = 0;
        playbackRunning = true;
        playbackWorker = std::thread(&AudioEngine::PlaybackLoop, this);
        std::cout << "[Audio] Playback started" << std::endl;
    }

    void StopPlaying()
    {
        playbackRunning = false;
        playbackCv.notify_all();

        if (playbackWorker.joinable())
        {
            playbackWorker.join();
        }

        if (hOut)
        {
            waveOutReset(hOut);

            for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
            {
                waveOutUnprepareHeader(hOut, &outHeaders[i], sizeof(WAVEHDR));
            }

            waveOutClose(hOut);
            hOut = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(playbackMutex);
            playbackSources.clear();
            playbackPrimed = false;
            outBufferFree.fill(true);
        }
        {
            std::lock_guard<std::mutex> lock(echoMutex);
            echoReferenceFrames.clear();
        }
    }

    void PlayFrame(void *data, int len)
    {
        PlayFrameFromSource("default", data, len);
    }

    void PlayFrameFromSource(const std::string &sourceId, const void *data,
                             int len)
    {
        if (!data || len <= 0)
        {
            return;
        }

        if (!hOut)
        {
            StartPlaying();
            if (!hOut)
            {
                return;
            }
        }

        const int alignedLen =
            std::min(FRAME_BYTES, len - (len % wfx.nBlockAlign));
        if (alignedLen <= 0)
        {
            return;
        }

        const std::string key = sourceId.empty() ? "default" : sourceId;
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(playbackMutex);
        auto &source = playbackSources[key];
        source.lastSeen = now;

        if (source.frames.size() >= MAX_SOURCE_BUFFER_FRAMES)
        {
            size_t removed = 0;
            while (source.frames.size() > TARGET_SOURCE_BUFFER_FRAMES)
            {
                source.frames.pop_front();
                ++removed;
            }

            auto totalDrops =
                playbackOverflowDrops.fetch_add(removed) + removed;
            if (totalDrops <= 5 || totalDrops % 50 == 0)
            {
                std::cerr << "[Audio] Source trim for " << key
                          << ", dropped total=" << totalDrops
                          << " buffered=" << source.frames.size() << std::endl;
            }
        }

        std::array<char, FRAME_BYTES> frame{};
        memcpy(frame.data(), data, static_cast<size_t>(alignedLen));
        source.frames.push_back(std::move(frame));

        CleanupIdleSourcesLocked(now);
        playbackCv.notify_one();
    }

    void SetSourceVolume(const std::string &sourceId, float volume)
    {
        const std::string key = sourceId.empty() ? "default" : sourceId;
        const float clamped = std::clamp(volume, 0.0f, 2.0f);

        std::lock_guard<std::mutex> lock(playbackMutex);
        if (clamped > 0.999f && clamped < 1.001f)
        {
            sourceVolumes.erase(key);
        }
        else
        {
            sourceVolumes[key] = clamped;
        }
    }

    float GetSourceVolume(const std::string &sourceId)
    {
        const std::string key = sourceId.empty() ? "default" : sourceId;
        std::lock_guard<std::mutex> lock(playbackMutex);
        return GetSourceVolumeLocked(key);
    }

    void SetEchoSuppressionEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoSuppressionEnabled = enabled;
    }

    bool IsEchoSuppressionEnabled()
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoSuppressionEnabled;
    }

    void SetEchoCorrelationThreshold(float value)
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoCorrelationThreshold = std::clamp(value, 0.30f, 0.95f);
    }

    float GetEchoCorrelationThreshold()
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoCorrelationThreshold;
    }

    void SetEchoSubtractionMaxGain(float value)
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoSubtractionMaxGain = std::clamp(value, 0.50f, 2.00f);
    }

    float GetEchoSubtractionMaxGain()
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoSubtractionMaxGain;
    }

    void SetEchoResidualAttenuation(float value)
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoResidualAttenuation = std::clamp(value, 0.10f, 1.00f);
    }

    float GetEchoResidualAttenuation()
    {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoResidualAttenuation;
    }

  private:
    void PlaybackLoop()
    {
        size_t consecutiveUnderruns = 0;
        while (playbackRunning.load())
        {
            std::array<char, FRAME_BYTES> frame{};
            size_t outIndex = OUTPUT_BUFFER_COUNT;
            size_t contributorCount = 0;
            bool hasAudio = false;

            {
                std::unique_lock<std::mutex> lock(playbackMutex);
                playbackCv.wait(lock,
                                [this]()
                                {
                                    return !playbackRunning.load() ||
                                           (HasFreeOutputBufferLocked() &&
                                            (playbackPrimed ||
                                             GetMaxBufferedFramesLocked() >=
                                                 STARTUP_BUFFER_FRAMES));
                                });

                if (!playbackRunning.load())
                {
                    break;
                }

                CleanupIdleSourcesLocked(std::chrono::steady_clock::now());

                outIndex = AcquireFreeOutputBufferLocked();
                if (outIndex == OUTPUT_BUFFER_COUNT)
                {
                    continue;
                }

                if (!playbackPrimed)
                {
                    playbackPrimed = true;
                    std::cout << "[Audio] Playback primed with "
                              << GetMaxBufferedFramesLocked()
                              << " buffered frames across "
                              << GetActiveSourceCountLocked() << " sources"
                              << std::endl;
                }

                hasAudio = MixNextFrameLocked(frame, contributorCount);
                if (hasAudio)
                {
                    consecutiveUnderruns = 0;
                }
                else
                {
                    auto underrun = playbackUnderruns.fetch_add(1) + 1;
                    if (underrun <= 5 || underrun % 50 == 0)
                    {
                        std::cerr << "[Audio] Playback underrun #" << underrun
                                  << std::endl;
                    }

                    ++consecutiveUnderruns;
                    if (consecutiveUnderruns >= OUTPUT_BUFFER_COUNT)
                    {
                        playbackPrimed = false;
                        outBufferFree[outIndex] = true;
                        if (underrun <= 5 || underrun % 50 == 0)
                        {
                            std::cerr
                                << "[Audio] Playback rebuffering after underrun"
                                << std::endl;
                        }
                        playbackCv.notify_one();
                        continue;
                    }

                    frame.fill(0);
                }
            }

            SubmitOutputBuffer(outIndex, frame);
        }
    }

    bool MixNextFrameLocked(std::array<char, FRAME_BYTES> &outFrame,
                            size_t &contributorCount)
    {
        std::array<int64_t, FRAME_SAMPLES> accumulator{};
        contributorCount = 0;
        auto now = std::chrono::steady_clock::now();
        bool consumedFrame = false;

        for (auto it = playbackSources.begin(); it != playbackSources.end();)
        {
            const std::string sourceId = it->first;
            auto &source = it->second;
            if (source.frames.empty())
            {
                if (now - source.lastSeen > SOURCE_IDLE_TIMEOUT)
                {
                    it = playbackSources.erase(it);
                }
                else
                {
                    ++it;
                }
                continue;
            }

            const auto &inputFrame = source.frames.front();
            const auto *inputSamples =
                reinterpret_cast<const int16_t *>(inputFrame.data());
            const float volume = GetSourceVolumeLocked(sourceId);
            source.frames.pop_front();
            consumedFrame = true;

            if (volume > 0.001f)
            {
                for (int i = 0; i < FRAME_SAMPLES; ++i)
                {
                    accumulator[i] += static_cast<int64_t>(
                        static_cast<float>(inputSamples[i]) * volume);
                }
                ++contributorCount;
            }

            if (source.frames.empty() &&
                now - source.lastSeen > SOURCE_IDLE_TIMEOUT)
            {
                it = playbackSources.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (!consumedFrame)
        {
            return false;
        }

        if (contributorCount == 0)
        {
            outFrame.fill(0);
            return true;
        }

        auto *outputSamples = reinterpret_cast<int16_t *>(outFrame.data());
        const int64_t divisor = static_cast<int64_t>(contributorCount);
        for (int i = 0; i < FRAME_SAMPLES; ++i)
        {
            const int32_t mixedSample = std::clamp<int32_t>(
                static_cast<int32_t>(accumulator[i] / divisor),
                static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                static_cast<int32_t>(std::numeric_limits<int16_t>::max()));
            outputSamples[i] = static_cast<int16_t>(mixedSample);
        }

        return true;
    }

    float GetSourceVolumeLocked(const std::string &sourceId) const
    {
        auto it = sourceVolumes.find(sourceId);
        if (it != sourceVolumes.end())
        {
            return it->second;
        }
        return 1.0f;
    }

    void CleanupIdleSourcesLocked(std::chrono::steady_clock::time_point now)
    {
        for (auto it = playbackSources.begin(); it != playbackSources.end();)
        {
            if (it->second.frames.empty() &&
                now - it->second.lastSeen > SOURCE_IDLE_TIMEOUT)
            {
                it = playbackSources.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    size_t GetMaxBufferedFramesLocked() const
    {
        size_t maxBuffered = 0;
        for (const auto &[sourceId, source] : playbackSources)
        {
            maxBuffered = std::max(maxBuffered, source.frames.size());
        }
        return maxBuffered;
    }

    size_t GetActiveSourceCountLocked() const
    {
        size_t count = 0;
        for (const auto &[sourceId, source] : playbackSources)
        {
            if (!source.frames.empty())
            {
                ++count;
            }
        }
        return count;
    }

    bool HasFreeOutputBufferLocked() const
    {
        for (bool isFree : outBufferFree)
        {
            if (isFree)
            {
                return true;
            }
        }
        return false;
    }

    size_t AcquireFreeOutputBufferLocked()
    {
        for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i)
        {
            if (outBufferFree[i])
            {
                outBufferFree[i] = false;
                return i;
            }
        }
        return OUTPUT_BUFFER_COUNT;
    }

    static int64_t ComputeFrameEnergy(const int16_t *samples, int count)
    {
        if (!samples || count <= 0)
        {
            return 0;
        }

        int64_t energy = 0;
        for (int i = 0; i < count; ++i)
        {
            const int64_t s = samples[i];
            energy += s * s;
        }
        return energy;
    }

    static int64_t ComputeDotProduct(const int16_t *a, const int16_t *b,
                                     int count)
    {
        if (!a || !b || count <= 0)
        {
            return 0;
        }

        int64_t dot = 0;
        for (int i = 0; i < count; ++i)
        {
            dot += static_cast<int64_t>(a[i]) * static_cast<int64_t>(b[i]);
        }
        return dot;
    }

    void PushEchoReferenceFrame(const std::array<char, FRAME_BYTES> &frame)
    {
        EchoReferenceFrame ref{};
        memcpy(ref.samples.data(), frame.data(), FRAME_BYTES);
        ref.energy = ComputeFrameEnergy(ref.samples.data(), FRAME_SAMPLES);

        std::lock_guard<std::mutex> lock(echoMutex);
        echoReferenceFrames.push_back(std::move(ref));
        while (echoReferenceFrames.size() > ECHO_HISTORY_FRAMES)
        {
            echoReferenceFrames.pop_front();
        }
    }

    void ApplyEchoSuppression(char *data, int len)
    {
        if (!data || len < static_cast<int>(sizeof(int16_t)))
        {
            return;
        }

        const int sampleCount =
            std::min(FRAME_SAMPLES, len / static_cast<int>(sizeof(int16_t)));
        if (sampleCount <= 0)
        {
            return;
        }

        std::array<int16_t, FRAME_SAMPLES> micSamples{};
        memcpy(micSamples.data(), data,
               static_cast<size_t>(sampleCount) * sizeof(int16_t));

        const int64_t micEnergy =
            ComputeFrameEnergy(micSamples.data(), sampleCount);
        if (micEnergy < ECHO_MIC_MIN_ENERGY)
        {
            return;
        }

        EchoReferenceFrame bestRef{};
        int64_t bestDot = 0;
        double bestCorr = 0.0;
        bool found = false;
        bool enabled = true;
        float corrThreshold = DEFAULT_ECHO_CORRELATION_THRESHOLD;
        float subtractionMaxGain = DEFAULT_ECHO_SUBTRACTION_MAX_GAIN;
        float residualAttenuation = DEFAULT_ECHO_RESIDUAL_ATTENUATION;

        {
            std::lock_guard<std::mutex> lock(echoMutex);
            enabled = echoSuppressionEnabled;
            corrThreshold = echoCorrelationThreshold;
            subtractionMaxGain = echoSubtractionMaxGain;
            residualAttenuation = echoResidualAttenuation;
            if (!enabled)
            {
                return;
            }

            for (const auto &ref : echoReferenceFrames)
            {
                if (ref.energy < ECHO_REFERENCE_MIN_ENERGY)
                {
                    continue;
                }

                const int64_t dot = ComputeDotProduct(
                    micSamples.data(), ref.samples.data(), sampleCount);
                const double denom =
                    std::sqrt(static_cast<double>(micEnergy) *
                              static_cast<double>(ref.energy)) +
                    1.0;
                const double corr = std::fabs(static_cast<double>(dot)) / denom;
                if (corr > bestCorr)
                {
                    bestCorr = corr;
                    bestDot = dot;
                    bestRef = ref;
                    found = true;
                }
            }
        }

        if (!found || bestCorr < corrThreshold || bestRef.energy <= 0)
        {
            return;
        }

        double echoGain =
            static_cast<double>(bestDot) / static_cast<double>(bestRef.energy);
        echoGain =
            std::clamp(echoGain, -static_cast<double>(subtractionMaxGain),
                       static_cast<double>(subtractionMaxGain));

        const double nearEndRatio = static_cast<double>(micEnergy) /
                                    static_cast<double>(bestRef.energy + 1);
        double subtractionMix = 1.0;
        if (nearEndRatio > 2.4)
        {
            subtractionMix = 0.35;
        }
        else if (nearEndRatio > 1.6)
        {
            subtractionMix = 0.55;
        }
        else if (nearEndRatio > 1.2)
        {
            subtractionMix = 0.75;
        }

        const bool mostlyEcho = (nearEndRatio < 1.05 && bestCorr > 0.70);

        for (int i = 0; i < sampleCount; ++i)
        {
            double cleaned = static_cast<double>(micSamples[i]) -
                             static_cast<double>(bestRef.samples[i]) *
                                 echoGain * subtractionMix;
            if (mostlyEcho)
            {
                cleaned *= residualAttenuation;
            }
            cleaned = std::clamp(
                cleaned,
                static_cast<double>(std::numeric_limits<int16_t>::min()),
                static_cast<double>(std::numeric_limits<int16_t>::max()));
            micSamples[i] = static_cast<int16_t>(cleaned);
        }

        memcpy(data, micSamples.data(),
               static_cast<size_t>(sampleCount) * sizeof(int16_t));

        const auto suppressed = echoSuppressedFrames.fetch_add(1) + 1;
        if (suppressed <= 3 || suppressed % 250 == 0)
        {
            std::cout << "[Audio] Echo suppression frame #" << suppressed
                      << " corr=" << bestCorr << " ratio=" << nearEndRatio
                      << std::endl;
        }
    }

    void SubmitOutputBuffer(size_t index,
                            const std::array<char, FRAME_BYTES> &frame)
    {
        if (!hOut || index >= OUTPUT_BUFFER_COUNT)
        {
            return;
        }

        PushEchoReferenceFrame(frame);
        memcpy(outBuffers[index].data(), frame.data(), FRAME_BYTES);
        outHeaders[index].dwBufferLength = FRAME_BYTES;
        outHeaders[index].dwFlags &= ~WHDR_DONE;

        MMRESULT res = waveOutWrite(hOut, &outHeaders[index], sizeof(WAVEHDR));
        if (res != MMSYSERR_NOERROR)
        {
            {
                std::lock_guard<std::mutex> lock(playbackMutex);
                outBufferFree[index] = true;
            }
            std::cerr << "[Audio] waveOutWrite failed: " << res << std::endl;
            playbackCv.notify_one();
        }
    }

    void ReleaseOutputBuffer(size_t index)
    {
        std::lock_guard<std::mutex> lock(playbackMutex);
        if (index < OUTPUT_BUFFER_COUNT)
        {
            outBufferFree[index] = true;
        }
        playbackCv.notify_one();
    }

    static void CALLBACK waveInProc(HWAVEIN hwi, UINT uMsg,
                                    DWORD_PTR dwInstance, DWORD_PTR dwParam1,
                                    DWORD_PTR)
    {
        if (uMsg != WIM_DATA)
        {
            return;
        }

        auto *engine = reinterpret_cast<AudioEngine *>(dwInstance);
        auto *hdr = reinterpret_cast<WAVEHDR *>(dwParam1);
        if (!engine || !hdr)
        {
            return;
        }

        std::array<char, FRAME_BYTES> captured{};
        int capturedLen = 0;

        if (engine->recording.load() && hdr->dwBytesRecorded > 0)
        {
            capturedLen = static_cast<int>(std::min<DWORD>(
                hdr->dwBytesRecorded, static_cast<DWORD>(FRAME_BYTES)));
            memcpy(captured.data(), hdr->lpData, capturedLen);
        }

        if (engine->recording.load() && engine->hIn)
        {
            MMRESULT res = waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
            if (res != MMSYSERR_NOERROR)
            {
                std::cerr << "[Audio] waveInAddBuffer failed: " << res
                          << std::endl;
            }
        }

        if (capturedLen <= 0)
        {
            return;
        }

        engine->ApplyEchoSuppression(captured.data(), capturedLen);

        AudioCallback cb;
        {
            EnterCriticalSection(&engine->callbackCS);
            cb = engine->onCapture;
            LeaveCriticalSection(&engine->callbackCS);
        }

        if (cb)
        {
            cb(captured.data(), capturedLen);
        }
    }

    static void CALLBACK waveOutProc(HWAVEOUT, UINT uMsg, DWORD_PTR dwInstance,
                                     DWORD_PTR dwParam1, DWORD_PTR)
    {
        if (uMsg != WOM_DONE)
        {
            return;
        }

        auto *engine = reinterpret_cast<AudioEngine *>(dwInstance);
        auto *hdr = reinterpret_cast<WAVEHDR *>(dwParam1);
        if (!engine || !hdr)
        {
            return;
        }

        engine->ReleaseOutputBuffer(static_cast<size_t>(hdr->dwUser));
    }
};
