#pragma once

#include <windows.h>
#include <mmsystem.h>
#include "rnnoise.h"

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

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <immintrin.h>
#define NEKOCHAT_HAS_SSE2 1
#endif

#pragma comment(lib, "winmm.lib")

class AudioEngine {
    using AudioCallback = std::function<void(void*, int)>;

public:
    struct DeviceInfo {
        UINT id = WAVE_MAPPER;
        std::string name;
        bool isDefault = false;
    };

    enum class VoicePreset {
        Custom = 0,
        Balanced,
        Headset,
        Speakers,
        SpeakerMax,
        Conference,
    };

private:
    static constexpr DWORD SAMPLE_RATE = 48000;
    static constexpr WORD CHANNELS = 1;
    static constexpr WORD BITS_PER_SAMPLE = 16;
    static constexpr int FRAME_BYTES = 960;
    static constexpr int FRAME_SAMPLES = FRAME_BYTES / static_cast<int>(sizeof(int16_t));
    static constexpr size_t INPUT_BUFFER_COUNT = 6;
    static constexpr size_t OUTPUT_BUFFER_COUNT = 12;
    static constexpr size_t STARTUP_BUFFER_FRAMES = 6;
    static constexpr size_t TARGET_SOURCE_BUFFER_FRAMES = 6;
    static constexpr size_t MAX_SOURCE_BUFFER_FRAMES = 18;
    static constexpr size_t MAX_CONCEALMENT_FRAMES = 2;
    static constexpr auto SOURCE_IDLE_TIMEOUT = std::chrono::seconds(3);
    static constexpr size_t ECHO_HISTORY_FRAMES = 40;
    static constexpr size_t ECHO_BLEND_REFERENCE_FRAMES = 3;
    static constexpr int64_t ECHO_REFERENCE_MIN_ENERGY = 8'000'000;
    static constexpr int64_t ECHO_MIC_MIN_ENERGY = 4'000'000;
    static constexpr float DEFAULT_ECHO_CORRELATION_THRESHOLD = 0.48f;
    static constexpr float DEFAULT_ECHO_SUBTRACTION_MAX_GAIN = 1.60f;
    static constexpr float DEFAULT_ECHO_RESIDUAL_ATTENUATION = 0.35f;
    static constexpr float DEFAULT_INPUT_GAIN = 1.0f;
    static constexpr float DEFAULT_AGC_TARGET_LEVEL = 0.18f;
    static constexpr float DEFAULT_AGC_MAX_BOOST = 3.0f;
    static constexpr float DEFAULT_NOISE_GATE_THRESHOLD = 0.012f;
    static constexpr float MIN_CAPTURE_GATE_ATTENUATION = 0.08f;
    static constexpr float MIX_LIMITER_THRESHOLD = 0.92f * 32767.0f;
    static constexpr float CAPTURE_LIMITER_THRESHOLD = 0.88f * 32767.0f;
    static constexpr int FRAME_EDGE_RAMP_SAMPLES = 24;

    struct SourceBuffer {
        std::deque<std::array<char, FRAME_BYTES>> frames;
        std::array<int16_t, FRAME_SAMPLES> lastFrame{};
        bool hasLastFrame = false;
        size_t concealmentFrames = 0;
        std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
    };

    struct EchoReferenceFrame {
        std::array<int16_t, FRAME_SAMPLES> samples{};
        int64_t energy = 0;
    };

    HWAVEIN hIn = nullptr;
    HWAVEOUT hOut = nullptr;
    WAVEFORMATEX wfx{};
    UINT inputDeviceId = WAVE_MAPPER;
    UINT outputDeviceId = WAVE_MAPPER;
    mutable std::mutex deviceMutex;

    std::vector<WAVEHDR*> inHeaders;
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
    bool automaticGainEnabled = true;
    float inputGain = DEFAULT_INPUT_GAIN;
    float automaticGainTargetLevel = DEFAULT_AGC_TARGET_LEVEL;
    float automaticGainMaxBoost = DEFAULT_AGC_MAX_BOOST;
    float noiseGateThreshold = DEFAULT_NOISE_GATE_THRESHOLD;
    float automaticGainCurrent = 1.0f;
    float captureGateCurrent = 1.0f;
    float captureLimiterGainCurrent = 1.0f;
    float captureTailSample = 0.0f;
    std::atomic<int> nearEndSpeechHoldFrames{0};
    std::atomic<int> echoDominantHoldFrames{0};
    std::atomic<float> adaptiveEchoStrength{0.0f};
    std::atomic<int> currentVoicePreset{static_cast<int>(VoicePreset::Balanced)};
    bool playbackPrimed = false;
    float playbackTailSample = 0.0f;
    std::array<int16_t, FRAME_SAMPLES> lastPlaybackFrame{};
    bool hasLastPlaybackFrame = false;
    size_t playbackHoldFrames = 0;

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
    std::mutex captureConfigMutex;

    std::atomic<unsigned long long> echoSuppressedFrames{0};
    std::atomic<unsigned long long> denoisedFrames{0};

    DenoiseState* rnnoiseState = nullptr;

public:
    AudioEngine() {
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = CHANNELS;
        wfx.nSamplesPerSec = SAMPLE_RATE;
        wfx.wBitsPerSample = BITS_PER_SAMPLE;
        wfx.nBlockAlign = static_cast<WORD>((wfx.nChannels * wfx.wBitsPerSample) / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize = 0;

        InitializeCriticalSection(&callbackCS);
        outBufferFree.fill(true);

        rnnoiseState = rnnoise_create(nullptr);
        if (!rnnoiseState) {
            std::cerr << "[Audio] Failed to create RNNoise state!" << std::endl;
        }
    }

    ~AudioEngine() {
        if (rnnoiseState) {
            rnnoise_destroy(rnnoiseState);
        }
        StopRecording();
        StopPlaying();
        DeleteCriticalSection(&callbackCS);
    }

    void StartRecording(AudioCallback cb) {
        {
            EnterCriticalSection(&callbackCS);
            onCapture = std::move(cb);
            LeaveCriticalSection(&callbackCS);
        }

        if (hIn) {
            recording = true;
            return;
        }

        recording = true;
        echoSuppressedFrames = 0;
        denoisedFrames = 0;
        {
            std::lock_guard<std::mutex> lock(captureConfigMutex);
            automaticGainCurrent = 1.0f;
            captureGateCurrent = 1.0f;
            captureLimiterGainCurrent = 1.0f;
            captureTailSample = 0.0f;
            nearEndSpeechHoldFrames.store(0, std::memory_order_relaxed);
            echoDominantHoldFrames.store(0, std::memory_order_relaxed);
            adaptiveEchoStrength.store(0.0f, std::memory_order_relaxed);
        }

        UINT deviceId = WAVE_MAPPER;
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            deviceId = inputDeviceId;
        }

        MMRESULT res = waveInOpen(&hIn, deviceId, &wfx,
                                  reinterpret_cast<DWORD_PTR>(waveInProc),
                                  reinterpret_cast<DWORD_PTR>(this),
                                  CALLBACK_FUNCTION);
        if (res != MMSYSERR_NOERROR) {
            std::cerr << "waveInOpen failed: " << res << std::endl;
            recording = false;
            hIn = nullptr;
            return;
        }

        inHeaders.reserve(INPUT_BUFFER_COUNT);
        for (size_t i = 0; i < INPUT_BUFFER_COUNT; ++i) {
            auto* hdr = new WAVEHDR();
            ZeroMemory(hdr, sizeof(WAVEHDR));
            hdr->lpData = new char[FRAME_BYTES];
            hdr->dwBufferLength = FRAME_BYTES;

            if (waveInPrepareHeader(hIn, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                delete[] hdr->lpData;
                delete hdr;
                continue;
            }

            if (waveInAddBuffer(hIn, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                waveInUnprepareHeader(hIn, hdr, sizeof(WAVEHDR));
                delete[] hdr->lpData;
                delete hdr;
                continue;
            }

            inHeaders.push_back(hdr);
        }

        if (inHeaders.empty()) {
            std::cerr << "waveInAddBuffer failed for all buffers" << std::endl;
            waveInClose(hIn);
            hIn = nullptr;
            recording = false;
            return;
        }

        MMRESULT startRes = waveInStart(hIn);
        if (startRes != MMSYSERR_NOERROR) {
            std::cerr << "[Audio] waveInStart failed: " << startRes << std::endl;
            StopRecording();
            return;
        }
        std::cout << "[Audio] Recording started" << std::endl;
    }

    void StopRecording() {
        recording = false;

        {
            EnterCriticalSection(&callbackCS);
            onCapture = nullptr;
            LeaveCriticalSection(&callbackCS);
        }

        if (!hIn) {
            return;
        }

        HWAVEIN hTemp = hIn;
        hIn = nullptr;

        waveInStop(hTemp);
        waveInReset(hTemp);

        for (auto* hdr : inHeaders) {
            waveInUnprepareHeader(hTemp, hdr, sizeof(WAVEHDR));
            delete[] hdr->lpData;
            delete hdr;
        }
        inHeaders.clear();

        waveInClose(hTemp);
        std::cout << "[Audio] Recording stopped" << std::endl;
    }

    void StartPlaying() {
        if (hOut) {
            return;
        }

        UINT deviceId = WAVE_MAPPER;
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            deviceId = outputDeviceId;
        }

        MMRESULT res = waveOutOpen(&hOut, deviceId, &wfx,
                                   reinterpret_cast<DWORD_PTR>(waveOutProc),
                                   reinterpret_cast<DWORD_PTR>(this),
                                   CALLBACK_FUNCTION);
        if (res != MMSYSERR_NOERROR) {
            std::cerr << "waveOutOpen failed: " << res << std::endl;
            hOut = nullptr;
            return;
        }

        bool playbackInitOk = true;
        for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i) {
            ZeroMemory(&outHeaders[i], sizeof(WAVEHDR));
            outHeaders[i].lpData = outBuffers[i].data();
            outHeaders[i].dwBufferLength = FRAME_BYTES;
            outHeaders[i].dwUser = static_cast<DWORD_PTR>(i);

            res = waveOutPrepareHeader(hOut, &outHeaders[i], sizeof(WAVEHDR));
            if (res != MMSYSERR_NOERROR) {
                std::cerr << "[Audio] waveOutPrepareHeader failed during init: " << res << std::endl;
                playbackInitOk = false;
                break;
            }
        }

        if (!playbackInitOk) {
            waveOutReset(hOut);
            for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i) {
                if (outHeaders[i].dwFlags & WHDR_PREPARED) {
                    waveOutUnprepareHeader(hOut, &outHeaders[i], sizeof(WAVEHDR));
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
            playbackTailSample = 0.0f;
            hasLastPlaybackFrame = false;
            playbackHoldFrames = 0;
            outBufferFree.fill(true);
        }
        {
            std::lock_guard<std::mutex> lock(echoMutex);
            echoReferenceFrames.clear();
        }

        playbackOverflowDrops = 0;
        playbackUnderruns = 0;
        echoSuppressedFrames = 0;
        denoisedFrames = 0;
        playbackRunning = true;
        playbackWorker = std::thread(&AudioEngine::PlaybackLoop, this);
        std::cout << "[Audio] Playback started" << std::endl;
    }

    void StopPlaying() {
        playbackRunning = false;
        playbackCv.notify_all();

        if (playbackWorker.joinable()) {
            playbackWorker.join();
        }

        if (hOut) {
            waveOutReset(hOut);

            for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i) {
                waveOutUnprepareHeader(hOut, &outHeaders[i], sizeof(WAVEHDR));
            }

            waveOutClose(hOut);
            hOut = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(playbackMutex);
            playbackSources.clear();
            playbackPrimed = false;
            playbackTailSample = 0.0f;
            hasLastPlaybackFrame = false;
            playbackHoldFrames = 0;
            outBufferFree.fill(true);
        }
        {
            std::lock_guard<std::mutex> lock(echoMutex);
            echoReferenceFrames.clear();
        }
    }

    void PlayFrame(void* data, int len) {
        PlayFrameFromSource("default", data, len);
    }

    std::vector<DeviceInfo> GetInputDevices() const {
        std::vector<DeviceInfo> devices;
        devices.push_back(DeviceInfo{WAVE_MAPPER, "System Default Input", true});

        const UINT count = waveInGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            WAVEINCAPSA caps{};
            if (waveInGetDevCapsA(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                devices.push_back(DeviceInfo{index, caps.szPname, false});
            }
        }
        return devices;
    }

    std::vector<DeviceInfo> GetOutputDevices() const {
        std::vector<DeviceInfo> devices;
        devices.push_back(DeviceInfo{WAVE_MAPPER, "System Default Output", true});

        const UINT count = waveOutGetNumDevs();
        for (UINT index = 0; index < count; ++index) {
            WAVEOUTCAPSA caps{};
            if (waveOutGetDevCapsA(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                devices.push_back(DeviceInfo{index, caps.szPname, false});
            }
        }
        return devices;
    }

    UINT GetSelectedInputDeviceId() const {
        std::lock_guard<std::mutex> lock(deviceMutex);
        return inputDeviceId;
    }

    UINT GetSelectedOutputDeviceId() const {
        std::lock_guard<std::mutex> lock(deviceMutex);
        return outputDeviceId;
    }

    bool SetInputDevice(UINT deviceId) {
        if (deviceId != WAVE_MAPPER && deviceId >= waveInGetNumDevs()) {
            return false;
        }

        AudioCallback restartCallback;
        bool shouldRestart = false;
        {
            EnterCriticalSection(&callbackCS);
            restartCallback = onCapture;
            shouldRestart = (hIn != nullptr);
            LeaveCriticalSection(&callbackCS);
        }

        if (shouldRestart) {
            StopRecording();
        }

        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            inputDeviceId = deviceId;
        }

        if (shouldRestart && restartCallback) {
            StartRecording(std::move(restartCallback));
        }
        return true;
    }

    bool SetOutputDevice(UINT deviceId) {
        if (deviceId != WAVE_MAPPER && deviceId >= waveOutGetNumDevs()) {
            return false;
        }

        const bool shouldRestart = (hOut != nullptr);
        if (shouldRestart) {
            StopPlaying();
        }

        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            outputDeviceId = deviceId;
        }

        if (shouldRestart) {
            StartPlaying();
        }
        return true;
    }

    void PlayFrameFromSource(const std::string& sourceId, const void* data, int len) {
        if (!data || len <= 0) {
            return;
        }

        if (!hOut) {
            StartPlaying();
            if (!hOut) {
                return;
            }
        }

        const int alignedLen = std::min(FRAME_BYTES, len - (len % wfx.nBlockAlign));
        if (alignedLen <= 0) {
            return;
        }

        const std::string key = sourceId.empty() ? "default" : sourceId;
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(playbackMutex);
        auto& source = playbackSources[key];
        source.lastSeen = now;

        if (source.frames.size() >= MAX_SOURCE_BUFFER_FRAMES) {
            size_t removed = 0;
            while (source.frames.size() > TARGET_SOURCE_BUFFER_FRAMES) {
                source.frames.pop_front();
                ++removed;
            }

            auto totalDrops = playbackOverflowDrops.fetch_add(removed) + removed;
            if (totalDrops <= 5 || totalDrops % 50 == 0) {
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

    void SetSourceVolume(const std::string& sourceId, float volume) {
        const std::string key = sourceId.empty() ? "default" : sourceId;
        const float clamped = std::clamp(volume, 0.0f, 2.0f);

        std::lock_guard<std::mutex> lock(playbackMutex);
        if (clamped > 0.999f && clamped < 1.001f) {
            sourceVolumes.erase(key);
        } else {
            sourceVolumes[key] = clamped;
        }
    }

    float GetSourceVolume(const std::string& sourceId) {
        const std::string key = sourceId.empty() ? "default" : sourceId;
        std::lock_guard<std::mutex> lock(playbackMutex);
        return GetSourceVolumeLocked(key);
    }

    void SetEchoSuppressionEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoSuppressionEnabled = enabled;
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    bool IsEchoSuppressionEnabled() {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoSuppressionEnabled;
    }

    void SetEchoCorrelationThreshold(float value) {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoCorrelationThreshold = std::clamp(value, 0.30f, 0.95f);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetEchoCorrelationThreshold() {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoCorrelationThreshold;
    }

    void SetEchoSubtractionMaxGain(float value) {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoSubtractionMaxGain = std::clamp(value, 0.50f, 2.00f);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetEchoSubtractionMaxGain() {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoSubtractionMaxGain;
    }

    void SetEchoResidualAttenuation(float value) {
        std::lock_guard<std::mutex> lock(echoMutex);
        echoResidualAttenuation = std::clamp(value, 0.10f, 1.00f);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetEchoResidualAttenuation() {
        std::lock_guard<std::mutex> lock(echoMutex);
        return echoResidualAttenuation;
    }

    void SetInputGain(float value) {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        inputGain = std::clamp(value, 0.25f, 4.0f);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetInputGain() {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        return inputGain;
    }

    void SetAutomaticGainEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        automaticGainEnabled = enabled;
        if (!enabled) {
            automaticGainCurrent = 1.0f;
        }
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    bool IsAutomaticGainEnabled() {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        return automaticGainEnabled;
    }

    void SetAutomaticGainTargetLevel(float value) {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        automaticGainTargetLevel = std::clamp(value, 0.06f, 0.40f);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetAutomaticGainTargetLevel() {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        return automaticGainTargetLevel;
    }

    void SetAutomaticGainMaxBoost(float value) {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        automaticGainMaxBoost = std::clamp(value, 1.0f, 8.0f);
        automaticGainCurrent = std::clamp(automaticGainCurrent, 0.35f, automaticGainMaxBoost);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetAutomaticGainMaxBoost() {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        return automaticGainMaxBoost;
    }

    void SetNoiseGateThreshold(float value) {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        noiseGateThreshold = std::clamp(value, 0.002f, 0.08f);
        currentVoicePreset = static_cast<int>(VoicePreset::Custom);
    }

    float GetNoiseGateThreshold() {
        std::lock_guard<std::mutex> lock(captureConfigMutex);
        return noiseGateThreshold;
    }

    VoicePreset GetVoicePreset() const {
        return static_cast<VoicePreset>(currentVoicePreset.load());
    }

    void ApplyVoicePreset(VoicePreset preset) {
        switch (preset) {
        case VoicePreset::Balanced: {
            std::lock_guard<std::mutex> captureLock(captureConfigMutex);
            std::lock_guard<std::mutex> echoLock(echoMutex);
            echoSuppressionEnabled = true;
            echoCorrelationThreshold = 0.54f;
            echoSubtractionMaxGain = 1.10f;
            echoResidualAttenuation = 0.72f;
            automaticGainEnabled = true;
            inputGain = 1.00f;
            automaticGainTargetLevel = 0.16f;
            automaticGainMaxBoost = 1.90f;
            noiseGateThreshold = 0.006f;
            automaticGainCurrent = 1.0f;
            captureGateCurrent = 1.0f;
            captureLimiterGainCurrent = 1.0f;
            captureTailSample = 0.0f;
            nearEndSpeechHoldFrames.store(0, std::memory_order_relaxed);
            echoDominantHoldFrames.store(0, std::memory_order_relaxed);
            adaptiveEchoStrength.store(0.0f, std::memory_order_relaxed);
            break;
        }
        case VoicePreset::Headset: {
            std::lock_guard<std::mutex> captureLock(captureConfigMutex);
            std::lock_guard<std::mutex> echoLock(echoMutex);
            echoSuppressionEnabled = true;
            echoCorrelationThreshold = 0.70f;
            echoSubtractionMaxGain = 0.90f;
            echoResidualAttenuation = 0.88f;
            automaticGainEnabled = true;
            inputGain = 1.05f;
            automaticGainTargetLevel = 0.18f;
            automaticGainMaxBoost = 1.70f;
            noiseGateThreshold = 0.004f;
            automaticGainCurrent = 1.0f;
            captureGateCurrent = 1.0f;
            captureLimiterGainCurrent = 1.0f;
            captureTailSample = 0.0f;
            nearEndSpeechHoldFrames.store(0, std::memory_order_relaxed);
            echoDominantHoldFrames.store(0, std::memory_order_relaxed);
            adaptiveEchoStrength.store(0.0f, std::memory_order_relaxed);
            break;
        }
        case VoicePreset::Speakers: {
            std::lock_guard<std::mutex> captureLock(captureConfigMutex);
            std::lock_guard<std::mutex> echoLock(echoMutex);
            echoSuppressionEnabled = true;
            echoCorrelationThreshold = 0.43f;
            echoSubtractionMaxGain = 1.68f;
            echoResidualAttenuation = 0.12f;
            automaticGainEnabled = true;
            inputGain = 0.96f;
            automaticGainTargetLevel = 0.145f;
            automaticGainMaxBoost = 1.75f;
            noiseGateThreshold = 0.011f;
            automaticGainCurrent = 1.0f;
            captureGateCurrent = 1.0f;
            captureLimiterGainCurrent = 1.0f;
            captureTailSample = 0.0f;
            nearEndSpeechHoldFrames.store(0, std::memory_order_relaxed);
            echoDominantHoldFrames.store(0, std::memory_order_relaxed);
            adaptiveEchoStrength.store(0.0f, std::memory_order_relaxed);
            break;
        }
        case VoicePreset::SpeakerMax: {
            std::lock_guard<std::mutex> captureLock(captureConfigMutex);
            std::lock_guard<std::mutex> echoLock(echoMutex);
            echoSuppressionEnabled = true;
            echoCorrelationThreshold = 0.40f;
            echoSubtractionMaxGain = 1.88f;
            echoResidualAttenuation = 0.10f;
            automaticGainEnabled = false;
            inputGain = 0.93f;
            automaticGainTargetLevel = 0.135f;
            automaticGainMaxBoost = 1.60f;
            noiseGateThreshold = 0.013f;
            automaticGainCurrent = 1.0f;
            captureGateCurrent = 1.0f;
            captureLimiterGainCurrent = 1.0f;
            captureTailSample = 0.0f;
            nearEndSpeechHoldFrames.store(0, std::memory_order_relaxed);
            echoDominantHoldFrames.store(0, std::memory_order_relaxed);
            adaptiveEchoStrength.store(0.0f, std::memory_order_relaxed);
            break;
        }
        case VoicePreset::Conference: {
            std::lock_guard<std::mutex> captureLock(captureConfigMutex);
            std::lock_guard<std::mutex> echoLock(echoMutex);
            echoSuppressionEnabled = true;
            echoCorrelationThreshold = 0.47f;
            echoSubtractionMaxGain = 1.45f;
            echoResidualAttenuation = 0.18f;
            automaticGainEnabled = true;
            inputGain = 0.94f;
            automaticGainTargetLevel = 0.14f;
            automaticGainMaxBoost = 1.65f;
            noiseGateThreshold = 0.014f;
            automaticGainCurrent = 1.0f;
            captureGateCurrent = 1.0f;
            captureLimiterGainCurrent = 1.0f;
            captureTailSample = 0.0f;
            nearEndSpeechHoldFrames.store(0, std::memory_order_relaxed);
            echoDominantHoldFrames.store(0, std::memory_order_relaxed);
            adaptiveEchoStrength.store(0.0f, std::memory_order_relaxed);
            break;
        }
        case VoicePreset::Custom:
        default:
            return;
        }

        currentVoicePreset = static_cast<int>(preset);
    }

private:
    void PlaybackLoop() {
        size_t consecutiveUnderruns = 0;
        while (playbackRunning.load()) {
            std::array<char, FRAME_BYTES> frame{};
            size_t outIndex = OUTPUT_BUFFER_COUNT;
            size_t contributorCount = 0;
            bool hasAudio = false;

            {
                std::unique_lock<std::mutex> lock(playbackMutex);
                playbackCv.wait(lock, [this]() {
                    return !playbackRunning.load() ||
                           (HasFreeOutputBufferLocked() &&
                            (playbackPrimed || GetMaxBufferedFramesLocked() >= STARTUP_BUFFER_FRAMES));
                });

                if (!playbackRunning.load()) {
                    break;
                }

                CleanupIdleSourcesLocked(std::chrono::steady_clock::now());

                outIndex = AcquireFreeOutputBufferLocked();
                if (outIndex == OUTPUT_BUFFER_COUNT) {
                    continue;
                }

                if (!playbackPrimed) {
                    playbackPrimed = true;
                    std::cout << "[Audio] Playback primed with "
                              << GetMaxBufferedFramesLocked()
                              << " buffered frames across "
                              << GetActiveSourceCountLocked()
                              << " sources" << std::endl;
                }

                hasAudio = MixNextFrameLocked(frame, contributorCount);
                if (hasAudio) {
                    consecutiveUnderruns = 0;
                    playbackHoldFrames = 0;
                    if (contributorCount > 0) {
                        memcpy(lastPlaybackFrame.data(), frame.data(), FRAME_BYTES);
                        hasLastPlaybackFrame = true;
                    }
                } else {
                    auto underrun = playbackUnderruns.fetch_add(1) + 1;
                    if (underrun <= 5 || underrun % 50 == 0) {
                        std::cerr << "[Audio] Playback underrun #" << underrun << std::endl;
                    }

                    ++consecutiveUnderruns;
                    if (consecutiveUnderruns >= OUTPUT_BUFFER_COUNT) {
                        playbackPrimed = false;
                        outBufferFree[outIndex] = true;
                        if (underrun <= 5 || underrun % 50 == 0) {
                            std::cerr << "[Audio] Playback rebuffering after underrun" << std::endl;
                        }
                        playbackCv.notify_one();
                        continue;
                    }

                    if (hasLastPlaybackFrame &&
                        playbackHoldFrames < MAX_CONCEALMENT_FRAMES) {
                        std::array<int16_t, FRAME_SAMPLES> concealmentFrame{};
                        BuildConcealmentFrame(
                            lastPlaybackFrame, concealmentFrame, playbackHoldFrames);
                        memcpy(frame.data(), concealmentFrame.data(), FRAME_BYTES);
                        ++playbackHoldFrames;
                    } else {
                        frame.fill(0);
                    }
                }
            }

            SubmitOutputBuffer(outIndex, frame);
        }
    }

    bool MixNextFrameLocked(std::array<char, FRAME_BYTES>& outFrame, size_t& contributorCount) {
        std::array<float, FRAME_SAMPLES> accumulator{};
        contributorCount = 0;
        auto now = std::chrono::steady_clock::now();
        bool consumedFrame = false;

        for (auto it = playbackSources.begin(); it != playbackSources.end();) {
            const std::string sourceId = it->first;
            auto& source = it->second;
            if (source.frames.empty()) {
                if (now - source.lastSeen > SOURCE_IDLE_TIMEOUT) {
                    it = playbackSources.erase(it);
                } else {
                    ++it;
                }
                continue;
            }

            const auto& inputFrame = source.frames.front();
            const auto* inputSamples = reinterpret_cast<const int16_t*>(inputFrame.data());
            const float volume = GetSourceVolumeLocked(sourceId);
            source.frames.pop_front();
            consumedFrame = true;

            if (volume > 0.001f) {
                AccumulateScaledSamples(accumulator.data(), inputSamples, volume, FRAME_SAMPLES);
                ++contributorCount;
            }

            if (source.frames.empty() && now - source.lastSeen > SOURCE_IDLE_TIMEOUT) {
                it = playbackSources.erase(it);
            } else {
                ++it;
            }
        }

        if (!consumedFrame) {
            return false;
        }

        if (contributorCount == 0) {
            outFrame.fill(0);
            return true;
        }

        auto* outputSamples = reinterpret_cast<int16_t*>(outFrame.data());
        FinalizeMixedSamples(accumulator.data(), outputSamples,
                             static_cast<float>(contributorCount),
                             FRAME_SAMPLES);

        return true;
    }

    float GetSourceVolumeLocked(const std::string& sourceId) const {
        auto it = sourceVolumes.find(sourceId);
        if (it != sourceVolumes.end()) {
            return it->second;
        }
        return 1.0f;
    }

    void CleanupIdleSourcesLocked(std::chrono::steady_clock::time_point now) {
        for (auto it = playbackSources.begin(); it != playbackSources.end();) {
            if (it->second.frames.empty() && now - it->second.lastSeen > SOURCE_IDLE_TIMEOUT) {
                it = playbackSources.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t GetMaxBufferedFramesLocked() const {
        size_t maxBuffered = 0;
        for (const auto& [sourceId, source] : playbackSources) {
            maxBuffered = std::max(maxBuffered, source.frames.size());
        }
        return maxBuffered;
    }

    size_t GetActiveSourceCountLocked() const {
        size_t count = 0;
        for (const auto& [sourceId, source] : playbackSources) {
            if (!source.frames.empty()) {
                ++count;
            }
        }
        return count;
    }

    bool HasFreeOutputBufferLocked() const {
        for (bool isFree : outBufferFree) {
            if (isFree) {
                return true;
            }
        }
        return false;
    }

    size_t AcquireFreeOutputBufferLocked() {
        for (size_t i = 0; i < OUTPUT_BUFFER_COUNT; ++i) {
            if (outBufferFree[i]) {
                outBufferFree[i] = false;
                return i;
            }
        }
        return OUTPUT_BUFFER_COUNT;
    }

    static int64_t ComputeFrameEnergy(const int16_t* samples, int count) {
        if (!samples || count <= 0) {
            return 0;
        }

#ifdef NEKOCHAT_HAS_SSE2
        __m128i energy64 = _mm_setzero_si128();
        int i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m128i values = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(samples + i));
            const __m128i madd = _mm_madd_epi16(values, values);
            const __m128i sign = _mm_srai_epi32(madd, 31);
            energy64 = _mm_add_epi64(energy64, _mm_unpacklo_epi32(madd, sign));
            energy64 = _mm_add_epi64(energy64, _mm_unpackhi_epi32(madd, sign));
        }

        alignas(16) std::array<int64_t, 2> partial{};
        _mm_storeu_si128(reinterpret_cast<__m128i*>(partial.data()), energy64);
        int64_t energy = partial[0] + partial[1];
        for (; i < count; ++i) {
            const int64_t s = samples[i];
            energy += s * s;
        }
        return energy;
#else
        int64_t energy = 0;
        for (int i = 0; i < count; ++i) {
            const int64_t s = samples[i];
            energy += s * s;
        }
        return energy;
#endif
    }

    static int64_t ComputeDotProduct(const int16_t* a, const int16_t* b, int count) {
        if (!a || !b || count <= 0) {
            return 0;
        }

#ifdef NEKOCHAT_HAS_SSE2
        __m128i dot64 = _mm_setzero_si128();
        int i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m128i va = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(a + i));
            const __m128i vb = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(b + i));
            const __m128i madd = _mm_madd_epi16(va, vb);
            const __m128i sign = _mm_srai_epi32(madd, 31);
            dot64 = _mm_add_epi64(dot64, _mm_unpacklo_epi32(madd, sign));
            dot64 = _mm_add_epi64(dot64, _mm_unpackhi_epi32(madd, sign));
        }

        alignas(16) std::array<int64_t, 2> partial{};
        _mm_storeu_si128(reinterpret_cast<__m128i*>(partial.data()), dot64);
        int64_t dot = partial[0] + partial[1];
        for (; i < count; ++i) {
            dot += static_cast<int64_t>(a[i]) * static_cast<int64_t>(b[i]);
        }
        return dot;
#else
        int64_t dot = 0;
        for (int i = 0; i < count; ++i) {
            dot += static_cast<int64_t>(a[i]) * static_cast<int64_t>(b[i]);
        }
        return dot;
#endif
    }

    static void ConvertInt16ToFloatSamples(const int16_t* src, float* dst, int count) {
        if (!src || !dst || count <= 0) {
            return;
        }

#ifdef NEKOCHAT_HAS_SSE2
        const __m128i zero = _mm_setzero_si128();
        int i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m128i values = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(src + i));
            const __m128i sign = _mm_cmpgt_epi16(zero, values);
            const __m128i lo = _mm_unpacklo_epi16(values, sign);
            const __m128i hi = _mm_unpackhi_epi16(values, sign);
            _mm_storeu_ps(dst + i, _mm_cvtepi32_ps(lo));
            _mm_storeu_ps(dst + i + 4, _mm_cvtepi32_ps(hi));
        }
        for (; i < count; ++i) {
            dst[i] = static_cast<float>(src[i]);
        }
#else
        for (int i = 0; i < count; ++i) {
            dst[i] = static_cast<float>(src[i]);
        }
#endif
    }

    static void ConvertFloatToInt16Samples(const float* src, int16_t* dst, int count) {
        if (!src || !dst || count <= 0) {
            return;
        }

#ifdef NEKOCHAT_HAS_SSE2
        const __m128 minValue = _mm_set1_ps(-32768.0f);
        const __m128 maxValue = _mm_set1_ps(32767.0f);
        int i = 0;
        for (; i + 8 <= count; i += 8) {
            __m128 lo = _mm_loadu_ps(src + i);
            __m128 hi = _mm_loadu_ps(src + i + 4);
            lo = _mm_min_ps(_mm_max_ps(lo, minValue), maxValue);
            hi = _mm_min_ps(_mm_max_ps(hi, minValue), maxValue);
            const __m128i lo32 = _mm_cvttps_epi32(lo);
            const __m128i hi32 = _mm_cvttps_epi32(hi);
            const __m128i packed = _mm_packs_epi32(lo32, hi32);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), packed);
        }
        for (; i < count; ++i) {
            const float sample = std::clamp(src[i], -32768.0f, 32767.0f);
            dst[i] = static_cast<int16_t>(sample);
        }
#else
        for (int i = 0; i < count; ++i) {
            const float sample = std::clamp(src[i], -32768.0f, 32767.0f);
            dst[i] = static_cast<int16_t>(sample);
        }
#endif
    }

    static void AccumulateScaledSamples(float* accumulator, const int16_t* inputSamples,
                                        float volume, int count) {
        if (!accumulator || !inputSamples || count <= 0) {
            return;
        }

#ifdef NEKOCHAT_HAS_SSE2
        const __m128 volumeVec = _mm_set1_ps(volume);
        const __m128i zero = _mm_setzero_si128();
        int i = 0;
        for (; i + 8 <= count; i += 8) {
            const __m128i values = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(inputSamples + i));
            const __m128i sign = _mm_cmpgt_epi16(zero, values);
            const __m128i lo = _mm_unpacklo_epi16(values, sign);
            const __m128i hi = _mm_unpackhi_epi16(values, sign);
            const __m128 scaledLo = _mm_mul_ps(_mm_cvtepi32_ps(lo), volumeVec);
            const __m128 scaledHi = _mm_mul_ps(_mm_cvtepi32_ps(hi), volumeVec);
            _mm_storeu_ps(accumulator + i,
                          _mm_add_ps(_mm_loadu_ps(accumulator + i), scaledLo));
            _mm_storeu_ps(accumulator + i + 4,
                          _mm_add_ps(_mm_loadu_ps(accumulator + i + 4), scaledHi));
        }
        for (; i < count; ++i) {
            accumulator[i] += static_cast<float>(inputSamples[i]) * volume;
        }
#else
        for (int i = 0; i < count; ++i) {
            accumulator[i] += static_cast<float>(inputSamples[i]) * volume;
        }
#endif
    }

    static void FinalizeMixedSamples(const float* accumulator, int16_t* outputSamples,
                                     float contributorCount, int count) {
        if (!accumulator || !outputSamples || count <= 0 || contributorCount <= 0.0f) {
            return;
        }

        for (int i = 0; i < count; ++i) {
            const float mixed = accumulator[i] / contributorCount;
            const float compressed = ApplySoftKneeCompression(
                mixed, MIX_LIMITER_THRESHOLD, 3.0f);
            const float limited = ApplySoftLimiter(compressed, 32767.0f);
            outputSamples[i] = static_cast<int16_t>(std::clamp(
                limited, -32768.0f, 32767.0f));
        }
    }

    static float ComputePeakAbs(const float* samples, int count) {
        if (!samples || count <= 0) {
            return 0.0f;
        }

        float peak = 0.0f;
        for (int i = 0; i < count; ++i) {
            peak = std::max(peak, std::fabs(samples[i]));
        }
        return peak;
    }

    static float ApplySoftKneeCompression(float sample, float threshold, float ratio) {
        if (threshold <= 0.0f || ratio <= 1.0f) {
            return sample;
        }

        const float absSample = std::fabs(sample);
        if (absSample <= threshold) {
            return sample;
        }

        const float sign = (sample < 0.0f) ? -1.0f : 1.0f;
        const float compressed = threshold + (absSample - threshold) / ratio;
        return sign * compressed;
    }

    static float ApplySoftLimiter(float sample, float threshold) {
        if (threshold <= 0.0f) {
            return 0.0f;
        }

        if (std::fabs(sample) <= threshold) {
            return sample;
        }

        const float normalized = sample / threshold;
        return threshold * std::tanh(normalized);
    }

    static void BuildConcealmentFrame(const std::array<int16_t, FRAME_SAMPLES>& lastFrame,
                                      std::array<int16_t, FRAME_SAMPLES>& outFrame,
                                      size_t concealmentIndex) {
        const float decay = std::pow(0.72f, static_cast<float>(concealmentIndex + 1));
        float previous = 0.0f;
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
            float sample = static_cast<float>(lastFrame[i]) * decay;
            sample = sample * 0.86f + previous * 0.14f;
            sample = std::clamp(sample, -32768.0f, 32767.0f);
            outFrame[i] = static_cast<int16_t>(sample);
            previous = sample;
        }
    }

    static void ApplyFrameEdgeRamp(float* samples, int count, float& previousTail) {
        if (!samples || count <= 0) {
            return;
        }

        const int rampSamples = std::min(count, FRAME_EDGE_RAMP_SAMPLES);
        const float startDelta = samples[0] - previousTail;
        if (rampSamples > 0 && std::fabs(startDelta) > 320.0f) {
            for (int i = 0; i < rampSamples; ++i) {
                const float mix = static_cast<float>(i + 1) /
                                  static_cast<float>(rampSamples + 1);
                const float bridged =
                    previousTail + (samples[i] - previousTail) * mix;
                samples[i] = samples[i] * 0.70f + bridged * 0.30f;
            }
        }

        previousTail = samples[count - 1];
    }

    static void ApplyFrameEdgeRamp(int16_t* samples, int count, float& previousTail) {
        if (!samples || count <= 0) {
            return;
        }

        const int rampSamples = std::min(count, FRAME_EDGE_RAMP_SAMPLES);
        const float startDelta = static_cast<float>(samples[0]) - previousTail;
        if (rampSamples > 0 && std::fabs(startDelta) > 320.0f) {
            for (int i = 0; i < rampSamples; ++i) {
                const float mix = static_cast<float>(i + 1) /
                                  static_cast<float>(rampSamples + 1);
                const float current = static_cast<float>(samples[i]);
                const float bridged = previousTail + (current - previousTail) * mix;
                const float smoothed = current * 0.70f + bridged * 0.30f;
                samples[i] = static_cast<int16_t>(std::clamp(
                    smoothed, -32768.0f, 32767.0f));
            }
        }

        previousTail = static_cast<float>(samples[count - 1]);
    }

    void PushEchoReferenceFrame(const std::array<char, FRAME_BYTES>& frame) {
        EchoReferenceFrame ref{};
        memcpy(ref.samples.data(), frame.data(), FRAME_BYTES);
        ref.energy = ComputeFrameEnergy(ref.samples.data(), FRAME_SAMPLES);

        std::lock_guard<std::mutex> lock(echoMutex);
        echoReferenceFrames.push_back(std::move(ref));
        while (echoReferenceFrames.size() > ECHO_HISTORY_FRAMES) {
            echoReferenceFrames.pop_front();
        }
    }

    void ApplyCaptureProcessing(char* data, int len) {
        if (!data || len < static_cast<int>(sizeof(int16_t))) {
            return;
        }

        const int sampleCount =
            std::min(FRAME_SAMPLES, len / static_cast<int>(sizeof(int16_t)));
        if (sampleCount <= 0) {
            return;
        }

        std::array<int16_t, FRAME_SAMPLES> micSamples{};
        memcpy(micSamples.data(), data,
               static_cast<size_t>(sampleCount) * sizeof(int16_t));

        const int64_t micEnergy =
            ComputeFrameEnergy(micSamples.data(), sampleCount);
        int aecNearEndHoldFrames = std::max(
            0, nearEndSpeechHoldFrames.load(std::memory_order_relaxed) - 1);
        int aecEchoHoldFrames = std::max(
            0, echoDominantHoldFrames.load(std::memory_order_relaxed) - 1);
        float adaptiveEchoLocal =
            adaptiveEchoStrength.load(std::memory_order_relaxed);
        bool adaptiveEchoTouched = false;

        if (micEnergy >= ECHO_MIC_MIN_ENERGY) {
            bool enabled = true;
            float corrThreshold = DEFAULT_ECHO_CORRELATION_THRESHOLD;
            float subtractionMaxGain = DEFAULT_ECHO_SUBTRACTION_MAX_GAIN;
            float residualAttenuation = DEFAULT_ECHO_RESIDUAL_ATTENUATION;

            struct EchoMatch {
                EchoReferenceFrame ref{};
                double corr = 0.0;
                double gain = 0.0;
                double score = 0.0;
                bool valid = false;
            };
            std::array<EchoMatch, ECHO_BLEND_REFERENCE_FRAMES> bestMatches{};

            {
                std::lock_guard<std::mutex> lock(echoMutex);
                enabled = echoSuppressionEnabled;
                corrThreshold = echoCorrelationThreshold;
                subtractionMaxGain = echoSubtractionMaxGain;
                residualAttenuation = echoResidualAttenuation;

                if (enabled) {
                    for (const auto& ref : echoReferenceFrames) {
                        if (ref.energy < ECHO_REFERENCE_MIN_ENERGY) {
                            continue;
                        }

                        const int64_t dot = ComputeDotProduct(
                            micSamples.data(), ref.samples.data(), sampleCount);
                        const double denom =
                            std::sqrt(static_cast<double>(micEnergy) *
                                      static_cast<double>(ref.energy)) +
                            1.0;
                        const double corr =
                            std::fabs(static_cast<double>(dot)) / denom;
                        if (corr < static_cast<double>(corrThreshold) * 0.72) {
                            continue;
                        }

                        double gain =
                            static_cast<double>(dot) /
                            static_cast<double>(ref.energy + 1);
                        gain = std::clamp(
                            gain,
                            -static_cast<double>(subtractionMaxGain),
                            static_cast<double>(subtractionMaxGain));

                        const double energyWeight = std::min(
                            1.25,
                            static_cast<double>(ref.energy) /
                                static_cast<double>(micEnergy + 1));
                        const double score = corr * corr * (0.85 + energyWeight * 0.40);

                        for (size_t slot = 0; slot < bestMatches.size(); ++slot) {
                            if (!bestMatches[slot].valid || score > bestMatches[slot].score) {
                                for (size_t shift = bestMatches.size() - 1; shift > slot; --shift) {
                                    bestMatches[shift] = bestMatches[shift - 1];
                                }
                                bestMatches[slot].ref = ref;
                                bestMatches[slot].corr = corr;
                                bestMatches[slot].gain = gain;
                                bestMatches[slot].score = score;
                                bestMatches[slot].valid = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (bestMatches[0].valid &&
                bestMatches[0].corr >= static_cast<double>(corrThreshold)) {
                std::array<float, FRAME_SAMPLES> echoEstimate{};
                double totalScore = 0.0;
                size_t activeMatches = 0;
                for (const auto& match : bestMatches) {
                    if (!match.valid) {
                        continue;
                    }
                    totalScore += match.score;
                    ++activeMatches;
                }

                if (totalScore > 0.0 && activeMatches > 0) {
                    for (const auto& match : bestMatches) {
                        if (!match.valid) {
                            continue;
                        }

                        const float weight = static_cast<float>(match.score / totalScore);
                        const float scaledGain = static_cast<float>(match.gain) * weight;
                        for (int i = 0; i < sampleCount; ++i) {
                            echoEstimate[i] +=
                                static_cast<float>(match.ref.samples[i]) * scaledGain;
                        }
                    }

                    double echoEstimateEnergy = 0.0;
                    for (int i = 0; i < sampleCount; ++i) {
                        const double sample = static_cast<double>(echoEstimate[i]);
                        echoEstimateEnergy += sample * sample;
                    }

                    const double primaryCorr = bestMatches[0].corr;
                    const double echoConfidence = std::clamp(
                        (primaryCorr - static_cast<double>(corrThreshold)) /
                            std::max(0.05, 1.0 - static_cast<double>(corrThreshold)),
                        0.0, 1.0);
                    const double nearEndRatio =
                        static_cast<double>(micEnergy) / (echoEstimateEnergy + 1.0);
                    const double doubleTalk =
                        std::clamp((nearEndRatio - 1.10) / 1.40, 0.0, 1.0);
                    const double nearEndSpeech =
                        std::clamp((nearEndRatio - 1.02) / 0.88, 0.0, 1.0);
                    const bool nearEndSpeechNow =
                        nearEndRatio > 1.12 ||
                        (doubleTalk > 0.18 && nearEndSpeech > 0.10);
                    const bool echoDominantNow =
                        nearEndRatio < 0.98 &&
                        primaryCorr > std::max(static_cast<double>(corrThreshold) + 0.18, 0.78);

                    if (nearEndSpeechNow) {
                        aecNearEndHoldFrames = 14;
                    }

                    if (echoDominantNow) {
                        aecEchoHoldFrames = 7;
                    }

                    const bool protectNearEnd = aecNearEndHoldFrames > 0;
                    const bool hardEchoMode =
                        aecEchoHoldFrames > 0 && !protectNearEnd;

                    const double echoToMicRatio =
                        echoEstimateEnergy /
                        (static_cast<double>(micEnergy) + 1.0);
                    const double echoToMicNorm = std::clamp(
                        (echoToMicRatio - 0.20) / 1.80, 0.0, 1.0);
                    double adaptiveTarget = std::clamp(
                        0.60 * echoConfidence + 0.70 * echoToMicNorm -
                            0.78 * nearEndSpeech - 0.36 * doubleTalk,
                        0.0, 1.0);
                    if (hardEchoMode) {
                        adaptiveTarget = std::max(adaptiveTarget, 0.82);
                    }
                    if (protectNearEnd) {
                        adaptiveTarget *= 0.32;
                    }

                    const float adaptiveRate =
                        (adaptiveTarget > static_cast<double>(adaptiveEchoLocal))
                            ? 0.30f
                            : 0.06f;
                    adaptiveEchoLocal +=
                        static_cast<float>(adaptiveTarget - adaptiveEchoLocal) *
                        adaptiveRate;
                    adaptiveEchoLocal = std::clamp(adaptiveEchoLocal, 0.0f, 1.0f);
                    adaptiveEchoTouched = true;

                    double subtractionMix =
                        (0.58 + 0.48 * echoConfidence +
                         0.28 * static_cast<double>(adaptiveEchoLocal)) *
                        (1.0 - 0.58 * doubleTalk) *
                        (1.0 - 0.68 * nearEndSpeech);
                    if (protectNearEnd) {
                        subtractionMix *= std::clamp(
                            0.58 - 0.16 * nearEndSpeech + 0.10 * doubleTalk,
                            0.30, 0.72);
                    } else if (hardEchoMode) {
                        subtractionMix +=
                            0.20 + 0.26 * echoConfidence +
                            0.22 * static_cast<double>(adaptiveEchoLocal);
                    }
                    subtractionMix = std::clamp(subtractionMix, 0.08, 1.12);

                    const bool mostlyEcho =
                        nearEndRatio < 1.12 &&
                        doubleTalk < 0.22 &&
                        !protectNearEnd &&
                        primaryCorr > std::max(static_cast<double>(corrThreshold) + 0.12, 0.74);
                    const double residualGate = hardEchoMode
                        ? std::clamp(
                              static_cast<double>(residualAttenuation) *
                                  (0.62 - 0.48 * echoConfidence -
                                   0.30 * static_cast<double>(adaptiveEchoLocal)),
                              0.05, 0.22)
                        : mostlyEcho
                        ? std::clamp(
                              static_cast<double>(residualAttenuation) *
                                  (0.78 - 0.32 * echoConfidence -
                                   0.22 * static_cast<double>(adaptiveEchoLocal)),
                              0.08, 0.32)
                        : std::clamp(
                              1.0 -
                                  (0.20 + 0.26 * static_cast<double>(adaptiveEchoLocal)) *
                                      echoConfidence *
                                      std::max(0.0, 1.0 - nearEndRatio / 2.0),
                              0.60, 1.0);

                    double cleanedEnergy = 0.0;

                    for (int i = 0; i < sampleCount; ++i) {
                        const double echoSample = static_cast<double>(echoEstimate[i]);
                        double cleaned = static_cast<double>(micSamples[i]) -
                                         echoSample * subtractionMix;

                        if (mostlyEcho) {
                            const double echoAbs = std::fabs(echoSample);
                            const double residualFloor = std::max(
                                120.0,
                                echoAbs * (0.10 + (1.0 - static_cast<double>(residualAttenuation)) * 0.32));
                            if (echoAbs > 1.0 && std::fabs(cleaned) < residualFloor) {
                                cleaned *= 0.24;
                            } else if (echoAbs > 1.0 && std::fabs(cleaned) < echoAbs * 0.55) {
                                const double keep = std::clamp(residualGate, 0.10, 0.38);
                                cleaned *= keep;
                            }
                        } else if (hardEchoMode) {
                            const double echoAbs = std::fabs(echoSample);
                            const double hardFloor = std::max(
                                180.0,
                                echoAbs * (0.24 + (1.0 - static_cast<double>(residualAttenuation)) * 0.56 +
                                           0.30 * static_cast<double>(adaptiveEchoLocal)));
                            if (echoAbs > 1.0 && std::fabs(cleaned) < hardFloor) {
                                cleaned *= 0.14;
                            } else if (echoAbs > 1.0 && std::fabs(cleaned) < echoAbs * 0.90) {
                                const double keep = std::clamp(residualGate, 0.07, 0.28);
                                cleaned *= keep;
                            }
                        } else if (!protectNearEnd &&
                                   adaptiveEchoLocal > 0.72f &&
                                   echoConfidence > 0.64) {
                            const double echoAbs = std::fabs(echoSample);
                            const double adaptiveFloor = std::max(
                                120.0,
                                echoAbs * (0.14 + 0.40 * static_cast<double>(adaptiveEchoLocal)));
                            if (echoAbs > 1.0 && std::fabs(cleaned) < adaptiveFloor) {
                                cleaned *= 0.30;
                            }
                        } else if (echoConfidence > 0.82 && doubleTalk < 0.10) {
                            const double echoAbs = std::fabs(echoSample);
                            if (echoAbs > 1.0 && std::fabs(cleaned) < echoAbs * 0.12) {
                                cleaned *= 0.35;
                            }
                        } else if (echoConfidence > 0.55 && nearEndRatio < 1.35) {
                            cleaned *= std::clamp(1.0 - 0.06 * echoConfidence, 0.90, 1.0);
                        }

                        cleaned = std::clamp(
                            cleaned,
                            static_cast<double>(std::numeric_limits<int16_t>::min()),
                            static_cast<double>(std::numeric_limits<int16_t>::max()));
                        cleanedEnergy += cleaned * cleaned;
                        micSamples[i] = static_cast<int16_t>(cleaned);
                    }

                    if (hardEchoMode) {
                        const double cleanedToEchoRatio =
                            cleanedEnergy / (echoEstimateEnergy + 1.0);
                        const double duckThreshold =
                            1.55 - 1.05 * static_cast<double>(adaptiveEchoLocal);
                        if (cleanedToEchoRatio < duckThreshold) {
                            const double frameDuck = std::clamp(
                                static_cast<double>(residualAttenuation) *
                                    (0.58 - 0.38 * echoConfidence -
                                     0.24 * static_cast<double>(adaptiveEchoLocal)),
                                0.10, 0.35);
                            for (int i = 0; i < sampleCount; ++i) {
                                const double ducked =
                                    static_cast<double>(micSamples[i]) * frameDuck;
                                micSamples[i] = static_cast<int16_t>(std::clamp(
                                    ducked,
                                    static_cast<double>(std::numeric_limits<int16_t>::min()),
                                    static_cast<double>(std::numeric_limits<int16_t>::max())));
                            }
                        }

                        
                        float lowpass = static_cast<float>(micSamples[0]);
                        const float lowpassCoeff = std::clamp(
                            0.07f + 0.10f * adaptiveEchoLocal, 0.07f, 0.17f);
                        for (int i = 1; i < sampleCount; ++i) {
                            const float original =
                                static_cast<float>(micSamples[i]);
                            lowpass += lowpassCoeff *
                                (original - lowpass);
                            const float blended =
                                original * 0.78f + lowpass * 0.22f;
                            micSamples[i] = static_cast<int16_t>(std::clamp(
                                blended, -32768.0f, 32767.0f));
                        }
                    }

                    echoSuppressedFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        if (!adaptiveEchoTouched) {
            adaptiveEchoLocal *= 0.94f;
        }
        if (adaptiveEchoLocal < 0.001f) {
            adaptiveEchoLocal = 0.0f;
        }
        adaptiveEchoStrength.store(adaptiveEchoLocal, std::memory_order_relaxed);
        nearEndSpeechHoldFrames.store(
            aecNearEndHoldFrames, std::memory_order_relaxed);
        echoDominantHoldFrames.store(
            aecEchoHoldFrames, std::memory_order_relaxed);

        std::array<float, FRAME_SAMPLES> processedFloat{};
        ConvertInt16ToFloatSamples(micSamples.data(), processedFloat.data(),
                                   sampleCount);

        if (rnnoiseState && sampleCount == FRAME_SAMPLES) {
            rnnoise_process_frame(
                rnnoiseState, processedFloat.data(), processedFloat.data());
            denoisedFrames.fetch_add(1, std::memory_order_relaxed);
        }

        bool agcEnabled = true;
        float localInputGain = DEFAULT_INPUT_GAIN;
        float localAgcTargetLevel = DEFAULT_AGC_TARGET_LEVEL;
        float localAgcMaxBoost = DEFAULT_AGC_MAX_BOOST;
        float localNoiseGateThreshold = DEFAULT_NOISE_GATE_THRESHOLD;
        float localAgcCurrent = 1.0f;
        float localGateCurrent = 1.0f;
        float localLimiterGainCurrent = 1.0f;
        float localCaptureTailSample = 0.0f;
        int localNearEndHoldFrames = nearEndSpeechHoldFrames.load(std::memory_order_relaxed);
        int localEchoHoldFrames = echoDominantHoldFrames.load(std::memory_order_relaxed);
        const float adaptiveEchoForDynamics =
            adaptiveEchoStrength.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(captureConfigMutex);
            agcEnabled = automaticGainEnabled;
            localInputGain = inputGain;
            localAgcTargetLevel = automaticGainTargetLevel;
            localAgcMaxBoost = automaticGainMaxBoost;
            localNoiseGateThreshold = noiseGateThreshold;
            localAgcCurrent = automaticGainCurrent;
            localGateCurrent = captureGateCurrent;
            localLimiterGainCurrent = captureLimiterGainCurrent;
            localCaptureTailSample = captureTailSample;
        }

        float rms = 0.0f;
        for (int i = 0; i < sampleCount; ++i) {
            rms += processedFloat[i] * processedFloat[i];
        }
        rms = std::sqrt(rms / static_cast<float>(sampleCount));

        const float gateLevel = localNoiseGateThreshold * 32767.0f;
        float gateTarget = 1.0f;
        if (rms < gateLevel) {
            const float gateRatio =
                std::clamp(rms / std::max(1.0f, gateLevel), 0.0f, 1.0f);
            gateTarget = std::clamp(
                gateRatio * gateRatio, MIN_CAPTURE_GATE_ATTENUATION, 1.0f);
        }
        const bool echoDominantNoNearEnd =
            localEchoHoldFrames > 0 && localNearEndHoldFrames <= 0;
        if (echoDominantNoNearEnd && rms < gateLevel * 1.8f) {
            gateTarget = std::min(gateTarget, 0.26f);
        }
        if (localNearEndHoldFrames <= 0 &&
            adaptiveEchoForDynamics > 0.62f &&
            rms < gateLevel * 2.6f) {
            const float adaptiveGateCap = std::clamp(
                0.30f - 0.18f * (adaptiveEchoForDynamics - 0.62f), 0.12f, 0.30f);
            gateTarget = std::min(gateTarget, adaptiveGateCap);
        }
        const float gateAdaptRate = (gateTarget > localGateCurrent) ? 0.34f : 0.12f;
        localGateCurrent += (gateTarget - localGateCurrent) * gateAdaptRate;
        localGateCurrent = std::clamp(
            localGateCurrent, MIN_CAPTURE_GATE_ATTENUATION, 1.0f);
        const float gateAttenuation = localGateCurrent;

        float agcGain = 1.0f;
        if (agcEnabled) {
            const float agcTarget = localAgcTargetLevel * 32767.0f;
            const float sensedLevel = std::max(
                rms * localInputGain, gateLevel * 0.85f + 1.0f);
            float desiredGain = std::clamp(
                agcTarget / sensedLevel, 0.35f, localAgcMaxBoost);
            if (rms < gateLevel * 1.25f) {
                desiredGain = std::min(desiredGain, 1.20f);
            }
            if (echoDominantNoNearEnd) {
                if (rms < gateLevel * 2.2f) {
                    desiredGain = std::min(desiredGain, 0.78f);
                } else {
                    desiredGain = std::min(desiredGain, 0.95f);
                }
            }
            if (localNearEndHoldFrames <= 0 && adaptiveEchoForDynamics > 0.58f) {
                const float adaptiveAgcCap = std::clamp(
                    0.95f - 0.42f * (adaptiveEchoForDynamics - 0.58f), 0.56f, 0.95f);
                desiredGain = std::min(desiredGain, adaptiveAgcCap);
            }

            const float adaptRate =
                (desiredGain < localAgcCurrent) ? 0.18f : 0.045f;
            localAgcCurrent += (desiredGain - localAgcCurrent) * adaptRate;
            localAgcCurrent = std::clamp(
                localAgcCurrent, 0.35f, localAgcMaxBoost);
            agcGain = localAgcCurrent;
        } else {
            localAgcCurrent = 1.0f;
        }

        const float totalGain = localInputGain * agcGain;
        const float predictedPeak =
            ComputePeakAbs(processedFloat.data(), sampleCount) * totalGain * gateAttenuation;
        float limiterTargetGain = 1.0f;
        if (predictedPeak > CAPTURE_LIMITER_THRESHOLD) {
            limiterTargetGain = CAPTURE_LIMITER_THRESHOLD / predictedPeak;
        }
        const float limiterAdaptRate =
            (limiterTargetGain < localLimiterGainCurrent) ? 0.40f : 0.08f;
        localLimiterGainCurrent +=
            (limiterTargetGain - localLimiterGainCurrent) * limiterAdaptRate;
        localLimiterGainCurrent = std::clamp(localLimiterGainCurrent, 0.20f, 1.0f);

        for (int i = 0; i < sampleCount; ++i) {
            float sample =
                processedFloat[i] * totalGain * gateAttenuation * localLimiterGainCurrent;
            sample = ApplySoftKneeCompression(sample, CAPTURE_LIMITER_THRESHOLD, 2.2f);
            sample = ApplySoftLimiter(sample, 32767.0f);
            processedFloat[i] = sample;
        }

        ApplyFrameEdgeRamp(processedFloat.data(), sampleCount, localCaptureTailSample);

        {
            std::lock_guard<std::mutex> lock(captureConfigMutex);
            automaticGainCurrent = localAgcCurrent;
            captureGateCurrent = localGateCurrent;
            captureLimiterGainCurrent = localLimiterGainCurrent;
            captureTailSample = localCaptureTailSample;
        }

        ConvertFloatToInt16Samples(processedFloat.data(), micSamples.data(), sampleCount);
        memcpy(data, micSamples.data(),
               static_cast<size_t>(sampleCount) * sizeof(int16_t));
    }
    void ApplyEchoSuppression(char* data, int len) {
    if (!data || !rnnoiseState || len < static_cast<int>(FRAME_SAMPLES * sizeof(int16_t))) {
        return;
    }

    int16_t* s16 = reinterpret_cast<int16_t*>(data);
    float frame_f[FRAME_SAMPLES];

    ConvertInt16ToFloatSamples(s16, frame_f, FRAME_SAMPLES);

    float voice_prob = rnnoise_process_frame(rnnoiseState, frame_f, frame_f);

    ConvertFloatToInt16Samples(frame_f, s16, FRAME_SAMPLES);

    const auto suppressed = echoSuppressedFrames.fetch_add(1) + 1;
    if (suppressed <= 3 || suppressed % 250 == 0) {
        std::cout << "[RNNoise] Frame #" << suppressed 
                  << " Voice Probability: " << (voice_prob * 100.0f) << "%" << std::endl;
    }
}

    void SubmitOutputBuffer(size_t index, const std::array<char, FRAME_BYTES>& frame) {
        if (!hOut || index >= OUTPUT_BUFFER_COUNT) {
            return;
        }

        PushEchoReferenceFrame(frame);
        memcpy(outBuffers[index].data(), frame.data(), FRAME_BYTES);
        outHeaders[index].dwBufferLength = FRAME_BYTES;
        outHeaders[index].dwFlags &= ~WHDR_DONE;

        MMRESULT res = waveOutWrite(hOut, &outHeaders[index], sizeof(WAVEHDR));
        if (res != MMSYSERR_NOERROR) {
            {
                std::lock_guard<std::mutex> lock(playbackMutex);
                outBufferFree[index] = true;
            }
            std::cerr << "[Audio] waveOutWrite failed: " << res << std::endl;
            playbackCv.notify_one();
        }
    }

    void ReleaseOutputBuffer(size_t index) {
        std::lock_guard<std::mutex> lock(playbackMutex);
        if (index < OUTPUT_BUFFER_COUNT) {
            outBufferFree[index] = true;
        }
        playbackCv.notify_one();
    }

    static void CALLBACK waveInProc(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                                    DWORD_PTR dwParam1, DWORD_PTR ) {
        if (uMsg != WIM_DATA) {
            return;
        }

        auto* engine = reinterpret_cast<AudioEngine*>(dwInstance);
        auto* hdr = reinterpret_cast<WAVEHDR*>(dwParam1);
        if (!engine || !hdr) {
            return;
        }

        std::array<char, FRAME_BYTES> captured{};
        int capturedLen = 0;

        if (engine->recording.load() && hdr->dwBytesRecorded > 0) {
            capturedLen = static_cast<int>(std::min<DWORD>(hdr->dwBytesRecorded, static_cast<DWORD>(FRAME_BYTES)));
            memcpy(captured.data(), hdr->lpData, capturedLen);
        }

        if (engine->recording.load() && engine->hIn) {
            MMRESULT res = waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
            if (res != MMSYSERR_NOERROR) {
                std::cerr << "[Audio] waveInAddBuffer failed: " << res << std::endl;
            }
        }

        if (capturedLen <= 0) {
            return;
        }

        engine->ApplyCaptureProcessing(captured.data(), capturedLen);

        AudioCallback cb;
        {
            EnterCriticalSection(&engine->callbackCS);
            cb = engine->onCapture;
            LeaveCriticalSection(&engine->callbackCS);
        }

        if (cb) {
            cb(captured.data(), capturedLen);
        }
    }

    static void CALLBACK waveOutProc(HWAVEOUT , UINT uMsg, DWORD_PTR dwInstance,
                                     DWORD_PTR dwParam1, DWORD_PTR ) {
        if (uMsg != WOM_DONE) {
            return;
        }

        auto* engine = reinterpret_cast<AudioEngine*>(dwInstance);
        auto* hdr = reinterpret_cast<WAVEHDR*>(dwParam1);
        if (!engine || !hdr) {
            return;
        }

        engine->ReleaseOutputBuffer(static_cast<size_t>(hdr->dwUser));
    }
};



