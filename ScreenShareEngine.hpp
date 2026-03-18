#pragma once

#include <objidl.h>
#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ScreenShareEngine
{
  public:
    struct EncodedFrame
    {
        std::vector<uint8_t> jpeg;
        int width = 0;
        int height = 0;
        uint64_t sequence = 0;
    };

    struct PreviewFrame
    {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        uint64_t sequence = 0;
    };

    using FrameCallback = std::function<void(const EncodedFrame &)>;

    ScreenShareEngine()
    {
        StartGdiplus();
    }

    ~ScreenShareEngine()
    {
        Stop();
        StopGdiplus();
    }

    bool Start(FrameCallback callback)
    {
        if (!gdiplusStarted || !jpegEncoderReady)
        {
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

    void Stop()
    {
        running = false;
        if (worker.joinable())
        {
            worker.join();
        }
    }

    bool IsRunning() const
    {
        return running.load();
    }

    void SetFps(int value)
    {
        std::lock_guard<std::mutex> lock(configMtx);
        fps = std::clamp(value, 1, 60);
    }

    int GetFps() const
    {
        std::lock_guard<std::mutex> lock(configMtx);
        return fps;
    }

    void SetQuality(int value)
    {
        std::lock_guard<std::mutex> lock(configMtx);
        jpegQuality = std::clamp(value, 20, 95);
    }

    int GetQuality() const
    {
        std::lock_guard<std::mutex> lock(configMtx);
        return jpegQuality;
    }

    void SetScalePercent(int value)
    {
        std::lock_guard<std::mutex> lock(configMtx);
        scalePercent = std::clamp(value, 20, 100);
    }

    int GetScalePercent() const
    {
        std::lock_guard<std::mutex> lock(configMtx);
        return scalePercent;
    }

    bool GetLatestPreview(PreviewFrame &out) const
    {
        std::lock_guard<std::mutex> lock(previewMtx);
        if (latestPreview.bgra.empty() || latestPreview.width <= 0 ||
            latestPreview.height <= 0)
        {
            return false;
        }
        out = latestPreview;
        return true;
    }

    bool DecodeJpegToPreview(const std::vector<uint8_t> &jpeg,
                             PreviewFrame &out) const
    {
        if (!gdiplusStarted || jpeg.empty())
        {
            return false;
        }

        IStream *stream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK || !stream)
        {
            return false;
        }

        ULONG written = 0;
        const ULONG toWrite = static_cast<ULONG>(
            std::min<size_t>(jpeg.size(), static_cast<size_t>(0xFFFFFFFFu)));
        if (stream->Write(jpeg.data(), toWrite, &written) != S_OK ||
            written != toWrite)
        {
            stream->Release();
            return false;
        }

        LARGE_INTEGER zero{};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        Gdiplus::Bitmap *bitmap = Gdiplus::Bitmap::FromStream(stream, FALSE);
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        {
            if (bitmap)
            {
                delete bitmap;
            }
            stream->Release();
            return false;
        }

        const int width = static_cast<int>(bitmap->GetWidth());
        const int height = static_cast<int>(bitmap->GetHeight());
        if (width <= 0 || height <= 0)
        {
            delete bitmap;
            stream->Release();
            return false;
        }

        Gdiplus::Rect rect(0, 0, width, height);
        Gdiplus::BitmapData lockData{};
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead,
                             PixelFormat32bppARGB, &lockData) != Gdiplus::Ok)
        {
            delete bitmap;
            stream->Release();
            return false;
        }

        out.width = width;
        out.height = height;
        out.bgra.resize(static_cast<size_t>(width) *
                        static_cast<size_t>(height) * 4u);
        const uint8_t *src = static_cast<const uint8_t *>(lockData.Scan0);
        for (int y = 0; y < height; ++y)
        {
            memcpy(out.bgra.data() +
                       static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
                   src + static_cast<size_t>(y) *
                             static_cast<size_t>(lockData.Stride),
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

    mutable std::mutex callbackMtx;
    FrameCallback onFrame;

    mutable std::mutex previewMtx;
    PreviewFrame latestPreview;

    ULONG_PTR gdiplusToken = 0;
    bool gdiplusStarted = false;
    bool jpegEncoderReady = false;
    CLSID jpegClsid{};

    std::atomic<uint64_t> frameSequence{0};

    void StartGdiplus()
    {
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) ==
            Gdiplus::Ok)
        {
            gdiplusStarted = true;
            jpegEncoderReady = GetEncoderClsid(L"image/jpeg", jpegClsid);
        }
    }

    void StopGdiplus()
    {
        if (gdiplusStarted)
        {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusStarted = false;
            gdiplusToken = 0;
        }
        jpegEncoderReady = false;
    }

    static bool GetEncoderClsid(const WCHAR *mimeType, CLSID &outClsid)
    {
        UINT num = 0;
        UINT size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size == 0 || num == 0)
        {
            return false;
        }

        std::vector<uint8_t> buffer(size);
        auto *encoders =
            reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
        if (Gdiplus::GetImageEncoders(num, size, encoders) != Gdiplus::Ok)
        {
            return false;
        }

        for (UINT i = 0; i < num; ++i)
        {
            if (wcscmp(encoders[i].MimeType, mimeType) == 0)
            {
                outClsid = encoders[i].Clsid;
                return true;
            }
        }
        return false;
    }

    bool EncodeJpeg(const std::vector<uint8_t> &bgra, int width, int height,
                    int quality, std::vector<uint8_t> &out)
    {
        out.clear();
        if (!jpegEncoderReady || width <= 0 || height <= 0 || bgra.empty())
        {
            return false;
        }

        Gdiplus::Bitmap bitmap(
            width, height, width * 4, PixelFormat32bppARGB,
            const_cast<BYTE *>(reinterpret_cast<const BYTE *>(bgra.data())));

        IStream *stream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK || !stream)
        {
            return false;
        }

        ULONG qualityParam = static_cast<ULONG>(std::clamp(quality, 20, 95));
        Gdiplus::EncoderParameters encParams{};
        encParams.Count = 1;
        encParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
        encParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        encParams.Parameter[0].NumberOfValues = 1;
        encParams.Parameter[0].Value = &qualityParam;

        if (bitmap.Save(stream, &jpegClsid, &encParams) != Gdiplus::Ok)
        {
            stream->Release();
            return false;
        }

        HGLOBAL memory = nullptr;
        if (GetHGlobalFromStream(stream, &memory) != S_OK || !memory)
        {
            stream->Release();
            return false;
        }

        const SIZE_T bytes = GlobalSize(memory);
        if (bytes == 0)
        {
            stream->Release();
            return false;
        }

        void *ptr = GlobalLock(memory);
        if (!ptr)
        {
            stream->Release();
            return false;
        }

        out.resize(bytes);
        memcpy(out.data(), ptr, bytes);
        GlobalUnlock(memory);
        stream->Release();
        return true;
    }

    void CaptureLoop()
    {
        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
        {
            running = false;
            return;
        }

        HDC memDc = CreateCompatibleDC(screenDc);
        if (!memDc)
        {
            ReleaseDC(nullptr, screenDc);
            running = false;
            return;
        }

        HBITMAP dib = nullptr;
        HGDIOBJ oldObj = nullptr;
        void *bits = nullptr;
        int captureW = 0;
        int captureH = 0;

        auto recreateDib = [&](int width, int height) -> bool
        {
            if (dib)
            {
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

            dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
            if (!dib || !bits)
            {
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

        while (running.load())
        {
            const auto frameStart = std::chrono::steady_clock::now();

            int localFps = 8;
            int localQuality = 60;
            int localScale = 50;
            {
                std::lock_guard<std::mutex> lock(configMtx);
                localFps = fps;
                localQuality = jpegQuality;
                localScale = scalePercent;
            }

            localFps = std::clamp(localFps, 1, 60);
            localScale = std::clamp(localScale, 20, 100);
            localQuality = std::clamp(localQuality, 20, 95);

            const int screenW = std::max(1, GetSystemMetrics(SM_CXSCREEN));
            const int screenH = std::max(1, GetSystemMetrics(SM_CYSCREEN));
            int targetW = std::max(320, (screenW * localScale) / 100);
            int targetH = std::max(180, (screenH * localScale) / 100);

            if (targetW != captureW || targetH != captureH || !dib || !bits)
            {
                if (!recreateDib(targetW, targetH))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
            }

            if (!StretchBlt(memDc, 0, 0, captureW, captureH, screenDc, 0, 0,
                            screenW, screenH, SRCCOPY | CAPTUREBLT))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::vector<uint8_t> frameBgra(static_cast<size_t>(captureW) *
                                           static_cast<size_t>(captureH) * 4u);
            memcpy(frameBgra.data(), bits, frameBgra.size());

            const uint64_t seq = frameSequence.fetch_add(1) + 1;
            {
                std::lock_guard<std::mutex> lock(previewMtx);
                latestPreview.bgra = frameBgra;
                latestPreview.width = captureW;
                latestPreview.height = captureH;
                latestPreview.sequence = seq;
            }

            std::vector<uint8_t> jpeg;
            if (EncodeJpeg(frameBgra, captureW, captureH, localQuality, jpeg))
            {
                FrameCallback cb;
                {
                    std::lock_guard<std::mutex> lock(callbackMtx);
                    cb = onFrame;
                }
                if (cb)
                {
                    EncodedFrame frame;
                    frame.jpeg = std::move(jpeg);
                    frame.width = captureW;
                    frame.height = captureH;
                    frame.sequence = seq;
                    cb(frame);
                }
            }

            const auto frameTime =
                std::chrono::milliseconds(std::max(1, 1000 / localFps));
            const auto elapsed = std::chrono::steady_clock::now() - frameStart;
            if (elapsed < frameTime)
            {
                std::this_thread::sleep_for(frameTime - elapsed);
            }
        }

        if (dib)
        {
            SelectObject(memDc, oldObj);
            DeleteObject(dib);
        }
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
    }
};
