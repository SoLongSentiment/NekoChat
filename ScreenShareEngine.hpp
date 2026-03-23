#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <objidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ScreenShareEngine {
public:
    struct EncodedFrame {
        std::vector<uint8_t> jpeg;
        int width = 0;
        int height = 0;
        uint64_t sequence = 0;
    };

    struct PreviewFrame {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        uint64_t sequence = 0;
    };

    struct SourceInfo {
        std::string id;
        std::string name;
        RECT rect{};
        bool isPrimary = false;
    };

    using FrameCallback = std::function<void(const EncodedFrame&)>;

    ScreenShareEngine() {
        StartGdiplus();
    }

    ~ScreenShareEngine() {
        Stop();
        StopGdiplus();
    }

    bool Start(FrameCallback callback) {
        if (!gdiplusStarted || !jpegEncoderReady) {
            return false;
        }

        Stop();

        {
            std::lock_guard<std::mutex> lock(callbackMtx);
            onFrame = std::move(callback);
        }

        running = true;
        worker = std::thread(&ScreenShareEngine::CaptureLoop, this);
        return true;
    }

    void Stop() {
        running = false;
        if (worker.joinable()) {
            worker.join();
        }
    }

    bool IsRunning() const {
        return running.load();
    }

    void SetFps(int value) {
        std::lock_guard<std::mutex> lock(configMtx);
        fps = std::clamp(value, 1, 60);
    }

    int GetFps() const {
        std::lock_guard<std::mutex> lock(configMtx);
        return fps;
    }

    void SetQuality(int value) {
        std::lock_guard<std::mutex> lock(configMtx);
        jpegQuality = std::clamp(value, 20, 95);
    }

    int GetQuality() const {
        std::lock_guard<std::mutex> lock(configMtx);
        return jpegQuality;
    }

    void SetScalePercent(int value) {
        std::lock_guard<std::mutex> lock(configMtx);
        scalePercent = std::clamp(value, 20, 100);
    }

    int GetScalePercent() const {
        std::lock_guard<std::mutex> lock(configMtx);
        return scalePercent;
    }

    std::vector<SourceInfo> GetSources() const {
        std::vector<SourceInfo> sources;

        RECT desktopRect{};
        desktopRect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        desktopRect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        desktopRect.right = desktopRect.left + std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        desktopRect.bottom = desktopRect.top + std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        if (desktopRect.right <= desktopRect.left || desktopRect.bottom <= desktopRect.top) {
            desktopRect.left = 0;
            desktopRect.top = 0;
            desktopRect.right = std::max(1, GetSystemMetrics(SM_CXSCREEN));
            desktopRect.bottom = std::max(1, GetSystemMetrics(SM_CYSCREEN));
        }

        sources.push_back(SourceInfo{"desktop", "Entire Desktop", desktopRect, false});

        struct MonitorCollector {
            std::vector<SourceInfo>* out = nullptr;
            int index = 0;
        } collector{&sources, 0};

        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM userData) -> BOOL {
                auto* collector = reinterpret_cast<MonitorCollector*>(userData);
                if (!collector || !collector->out) {
                    return TRUE;
                }

                MONITORINFOEXA info{};
                info.cbSize = sizeof(info);
                if (!GetMonitorInfoA(monitor, &info)) {
                    return TRUE;
                }

                const int width = std::max(1L, info.rcMonitor.right - info.rcMonitor.left);
                const int height = std::max(1L, info.rcMonitor.bottom - info.rcMonitor.top);
                ++collector->index;

                std::string label = "Monitor " + std::to_string(collector->index);
                if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
                    label += " (Primary)";
                }
                label += " - " + std::to_string(width) + "x" + std::to_string(height);

                collector->out->push_back(SourceInfo{
                    "monitor:" + std::to_string(collector->index),
                    label,
                    info.rcMonitor,
                    (info.dwFlags & MONITORINFOF_PRIMARY) != 0
                });
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&collector));

        return sources;
    }

    bool SetSourceId(const std::string& value) {
        if (value.empty()) {
            return false;
        }

        const auto sources = GetSources();
        auto it = std::find_if(sources.begin(), sources.end(), [&value](const SourceInfo& source) {
            return source.id == value;
        });
        if (it == sources.end()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(configMtx);
        sourceId = value;
        return true;
    }

    std::string GetSourceId() const {
        std::lock_guard<std::mutex> lock(configMtx);
        return sourceId;
    }

    bool GetLatestPreview(PreviewFrame& out) const {
        std::lock_guard<std::mutex> lock(previewMtx);
        if (latestPreview.bgra.empty() || latestPreview.width <= 0 || latestPreview.height <= 0) {
            return false;
        }
        out = latestPreview;
        return true;
    }

    bool DecodeJpegToPreview(const std::vector<uint8_t>& jpeg, PreviewFrame& out) const {
        if (!gdiplusStarted || jpeg.empty()) {
            return false;
        }

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK || !stream) {
            return false;
        }

        ULONG written = 0;
        const ULONG toWrite = static_cast<ULONG>(std::min<size_t>(jpeg.size(), static_cast<size_t>(0xFFFFFFFFu)));
        if (stream->Write(jpeg.data(), toWrite, &written) != S_OK || written != toWrite) {
            stream->Release();
            return false;
        }

        LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromStream(stream, FALSE);
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
            if (bitmap) {
                delete bitmap;
            }
            stream->Release();
            return false;
        }

        const int width = static_cast<int>(bitmap->GetWidth());
        const int height = static_cast<int>(bitmap->GetHeight());
        if (width <= 0 || height <= 0) {
            delete bitmap;
            stream->Release();
            return false;
        }

        Gdiplus::Rect rect(0, 0, width, height);
        Gdiplus::BitmapData lockData{};
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &lockData) != Gdiplus::Ok) {
            delete bitmap;
            stream->Release();
            return false;
        }

        out.width = width;
        out.height = height;
        out.bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        const uint8_t* src = static_cast<const uint8_t*>(lockData.Scan0);
        for (int y = 0; y < height; ++y) {
            memcpy(out.bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
                   src + static_cast<size_t>(y) * static_cast<size_t>(lockData.Stride),
                   static_cast<size_t>(width) * 4u);
        }

        bitmap->UnlockBits(&lockData);
        delete bitmap;
        stream->Release();
        return true;
    }

private:
    std::atomic<bool> running{false};
    std::thread worker;

    mutable std::mutex configMtx;
    int fps = 8;
    int jpegQuality = 60;
    int scalePercent = 50;
    std::string sourceId = "desktop";

    mutable std::mutex callbackMtx;
    FrameCallback onFrame;

    mutable std::mutex previewMtx;
    PreviewFrame latestPreview;

    ULONG_PTR gdiplusToken = 0;
    bool gdiplusStarted = false;
    bool jpegEncoderReady = false;
    CLSID jpegClsid{};

    std::atomic<uint64_t> frameSequence{0};

    void StartGdiplus() {
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) == Gdiplus::Ok) {
            gdiplusStarted = true;
            jpegEncoderReady = GetEncoderClsid(L"image/jpeg", jpegClsid);
        }
    }

    void StopGdiplus() {
        if (gdiplusStarted) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusStarted = false;
            gdiplusToken = 0;
        }
        jpegEncoderReady = false;
    }

    static bool GetEncoderClsid(const WCHAR* mimeType, CLSID& outClsid) {
        UINT num = 0;
        UINT size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size == 0 || num == 0) {
            return false;
        }

        std::vector<uint8_t> buffer(size);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        if (Gdiplus::GetImageEncoders(num, size, encoders) != Gdiplus::Ok) {
            return false;
        }

        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(encoders[i].MimeType, mimeType) == 0) {
                outClsid = encoders[i].Clsid;
                return true;
            }
        }
        return false;
    }

    bool EncodeJpeg(const uint8_t* bgra, int width, int height, int stride, int quality, std::vector<uint8_t>& out) {
        out.clear();
        if (!jpegEncoderReady || width <= 0 || height <= 0 || stride <= 0 || !bgra) {
            return false;
        }

        Gdiplus::Bitmap bitmap(
            width,
            height,
            stride,
            PixelFormat32bppARGB,
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bgra)));

        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK || !stream) {
            return false;
        }

        ULONG qualityParam = static_cast<ULONG>(std::clamp(quality, 20, 95));
        Gdiplus::EncoderParameters encParams{};
        encParams.Count = 1;
        encParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
        encParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        encParams.Parameter[0].NumberOfValues = 1;
        encParams.Parameter[0].Value = &qualityParam;

        if (bitmap.Save(stream, &jpegClsid, &encParams) != Gdiplus::Ok) {
            stream->Release();
            return false;
        }

        HGLOBAL memory = nullptr;
        if (GetHGlobalFromStream(stream, &memory) != S_OK || !memory) {
            stream->Release();
            return false;
        }

        const SIZE_T bytes = GlobalSize(memory);
        if (bytes == 0) {
            stream->Release();
            return false;
        }

        void* ptr = GlobalLock(memory);
        if (!ptr) {
            stream->Release();
            return false;
        }

        out.resize(bytes);
        memcpy(out.data(), ptr, bytes);
        GlobalUnlock(memory);
        stream->Release();
        return true;
    }

    void CaptureLoop() {
        HDC screenDc = GetDC(nullptr);
        if (!screenDc) {
            running = false;
            return;
        }

        HDC memDc = CreateCompatibleDC(screenDc);
        if (!memDc) {
            ReleaseDC(nullptr, screenDc);
            running = false;
            return;
        }

        HBITMAP dib = nullptr;
        HGDIOBJ oldObj = nullptr;
        void* bits = nullptr;
        int captureW = 0;
        int captureH = 0;

        auto recreateDib = [&](int width, int height) -> bool {
            if (dib) {
                SelectObject(memDc, oldObj);
                DeleteObject(dib);
                dib = nullptr;
                oldObj = nullptr;
                bits = nullptr;
            }

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib || !bits) {
                dib = nullptr;
                bits = nullptr;
                return false;
            }

            oldObj = SelectObject(memDc, dib);
            SetStretchBltMode(memDc, HALFTONE);
            SetBrushOrgEx(memDc, 0, 0, nullptr);
            captureW = width;
            captureH = height;
            return true;
        };

        std::vector<uint8_t> previewScratch;
        std::vector<uint8_t> jpeg;

        while (running.load()) {
            const auto frameStart = std::chrono::steady_clock::now();

            int localFps = 8;
            int localQuality = 60;
            int localScale = 50;
            std::string localSourceId = "desktop";
            {
                std::lock_guard<std::mutex> lock(configMtx);
                localFps = fps;
                localQuality = jpegQuality;
                localScale = scalePercent;
                localSourceId = sourceId;
            }

            localFps = std::clamp(localFps, 1, 60);
            localScale = std::clamp(localScale, 20, 100);
            localQuality = std::clamp(localQuality, 20, 95);

            const auto sources = GetSources();
            auto selectedSource = std::find_if(sources.begin(), sources.end(), [&localSourceId](const SourceInfo& source) {
                return source.id == localSourceId;
            });
            if (selectedSource == sources.end()) {
                selectedSource = sources.begin();
            }

            RECT sourceRect{};
            if (selectedSource != sources.end()) {
                sourceRect = selectedSource->rect;
            } else {
                sourceRect.left = 0;
                sourceRect.top = 0;
                sourceRect.right = std::max(1, GetSystemMetrics(SM_CXSCREEN));
                sourceRect.bottom = std::max(1, GetSystemMetrics(SM_CYSCREEN));
            }

            const int sourceW = std::max(1L, sourceRect.right - sourceRect.left);
            const int sourceH = std::max(1L, sourceRect.bottom - sourceRect.top);
            int targetW = std::max(320, (sourceW * localScale) / 100);
            int targetH = std::max(180, (sourceH * localScale) / 100);

            if (targetW != captureW || targetH != captureH || !dib || !bits) {
                if (!recreateDib(targetW, targetH)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
            }

            if (!StretchBlt(memDc, 0, 0, captureW, captureH,
                            screenDc, sourceRect.left, sourceRect.top, sourceW, sourceH,
                            SRCCOPY | CAPTUREBLT)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            const size_t frameBytes =
                static_cast<size_t>(captureW) * static_cast<size_t>(captureH) * 4u;
            if (previewScratch.size() != frameBytes) {
                previewScratch.resize(frameBytes);
            }
            memcpy(previewScratch.data(), bits, frameBytes);

            const uint64_t seq = frameSequence.fetch_add(1) + 1;
            {
                std::lock_guard<std::mutex> lock(previewMtx);
                latestPreview.bgra.swap(previewScratch);
                latestPreview.width = captureW;
                latestPreview.height = captureH;
                latestPreview.sequence = seq;
            }

            if (EncodeJpeg(static_cast<const uint8_t*>(bits), captureW, captureH, captureW * 4, localQuality, jpeg)) {
                FrameCallback cb;
                {
                    std::lock_guard<std::mutex> lock(callbackMtx);
                    cb = onFrame;
                }
                if (cb) {
                    EncodedFrame frame;
                    frame.jpeg.swap(jpeg);
                    frame.width = captureW;
                    frame.height = captureH;
                    frame.sequence = seq;
                    cb(frame);
                    jpeg.swap(frame.jpeg);
                    jpeg.clear();
                }
            }

            const auto frameTime = std::chrono::milliseconds(std::max(1, 1000 / localFps));
            const auto elapsed = std::chrono::steady_clock::now() - frameStart;
            if (elapsed < frameTime) {
                std::this_thread::sleep_for(frameTime - elapsed);
            }
        }

        if (dib) {
            SelectObject(memDc, oldObj);
            DeleteObject(dib);
        }
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
    }
};





