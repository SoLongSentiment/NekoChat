#define NEKOCHAT_NO_MAIN
#include "P2PChat.cpp"

#include <gl/GL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <commdlg.h>
#include <memory>
#include <mutex>
#include <shellapi.h>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>
#include <windowsx.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {

struct GuiLogBuffer : public std::streambuf {
    std::mutex mtx;
    std::vector<std::string> allLines;
    std::vector<std::string> connectionLines;
    std::string currentLine;

    int overflow(int ch) override {
        if (ch == EOF) {
            return traits_type::not_eof(ch);
        }

        std::lock_guard<std::mutex> lock(mtx);
        if (ch == '\n') {
            PushLineLocked(currentLine);
            currentLine.clear();
        } else if (ch != '\r') {
            currentLine.push_back(static_cast<char>(ch));
        }
        return ch;
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mtx);
        if (!currentLine.empty()) {
            PushLineLocked(currentLine);
            currentLine.clear();
        }
        return 0;
    }

    std::vector<std::string> SnapshotAll() {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::string> snapshot = allLines;
        if (!currentLine.empty()) {
            snapshot.push_back(currentLine);
        }
        return snapshot;
    }

    std::vector<std::string> SnapshotConnection() {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::string> snapshot = connectionLines;
        if (!currentLine.empty() && IsConnectionLine(currentLine)) {
            snapshot.push_back(currentLine);
        }
        return snapshot;
    }

private:
    static constexpr size_t MAX_GUI_LOG_LINES = 1000;

    static bool IsConnectionLine(const std::string& line) {
        if (line.empty()) {
            return false;
        }
        return line.rfind("[Audio]", 0) != 0 &&
               line.rfind("[AudioCtl]", 0) != 0;
    }

    static void TrimLinesLocked(std::vector<std::string>& lines) {
        if (lines.size() > MAX_GUI_LOG_LINES) {
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - MAX_GUI_LOG_LINES));
        }
    }

    void PushLineLocked(const std::string& line) {
        allLines.push_back(line);
        TrimLinesLocked(allLines);
        if (IsConnectionLine(line)) {
            connectionLines.push_back(line);
            TrimLinesLocked(connectionLines);
        }
    }
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct AppState {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC glrc = nullptr;
    HFONT font = nullptr;
    GLuint fontBase = 0;

    int width = 1280;
    int height = 800;
    POINT mousePos{};
    bool mouseClicked = false;
    bool mouseDown = false;
    bool connected = false;
    int activeField = 0;
    int activeTab = 0;

    std::string idInput;
    std::string commandInput;
    std::string streamTargetInput;
    std::string fileTargetInput;
    std::string filePathInput;
    size_t idCaret = 0;
    size_t commandCaret = 0;
    size_t streamTargetCaret = 0;
    size_t fileTargetCaret = 0;
    size_t filePathCaret = 0;

    std::unique_ptr<P2PClient> client;

    GLuint localScreenTexture = 0;
    GLuint remoteScreenTexture = 0;
    uint64_t localScreenSeq = 0;
    uint64_t remoteScreenSeq = 0;
    int localScreenW = 0;
    int localScreenH = 0;
    int remoteScreenW = 0;
    int remoteScreenH = 0;
    std::vector<uint8_t> remoteScreenBgra;
    std::string selectedRemotePeer;
    std::string remoteScreenPeer;
    HWND viewerHwnd = nullptr;
    int activeModal = 0;
    IncomingOfferPrompt activeOfferPrompt;
    PendingIncomingFilePrompt activeFilePrompt;

    GuiLogBuffer logBuffer;
    std::streambuf* oldCout = nullptr;
    std::streambuf* oldCerr = nullptr;
};

constexpr int FIELD_ID = 1;
constexpr int FIELD_COMMAND = 2;
constexpr int FIELD_STREAM_TARGET = 3;
constexpr int FIELD_FILE_TARGET = 4;
constexpr int FIELD_FILE_PATH = 5;
constexpr int TAB_CONNECTION = 0;
constexpr int TAB_LOGS = 1;
constexpr int TAB_VOICE = 2;
constexpr int TAB_SCREEN = 3;
constexpr int TAB_FILES = 4;
constexpr int MODAL_NONE = 0;
constexpr int MODAL_OFFER = 1;
constexpr int MODAL_FILE = 2;
constexpr const char* VIEWER_WINDOW_CLASS = "NekoChatStreamViewer";

bool PointInRect(const POINT& pt, float x, float y, float w, float h) {
    return pt.x >= x && pt.x <= x + w && pt.y >= y && pt.y <= y + h;
}

void DrawFilledRect(float x, float y, float w, float h, const Color& color) {
    glColor4f(color.r, color.g, color.b, color.a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void DrawRect(float x, float y, float w, float h, const Color& color, float lineWidth = 1.0f) {
    glLineWidth(lineWidth);
    glColor4f(color.r, color.g, color.b, color.a);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

void DrawText(const AppState& app, float x, float y, const std::string& text, const Color& color) {
    if (text.empty() || app.fontBase == 0) {
        return;
    }

    glColor4f(color.r, color.g, color.b, color.a);
    glRasterPos2f(x, y + 14.0f);
    glListBase(app.fontBase - 32);
    glCallLists(static_cast<GLsizei>(text.size()), GL_UNSIGNED_BYTE, text.c_str());
}

void UpdateTextureFromPreview(GLuint& texture, const ScreenShareEngine::PreviewFrame& frame) {
    if (frame.bgra.empty() || frame.width <= 0 || frame.height <= 0) {
        return;
    }

    if (texture == 0) {
        glGenTextures(1, &texture);
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, frame.bgra.data());
}

void CopyByteBufferFast(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    if (&dst == &src) {
        return;
    }

    dst.resize(src.size());
    if (src.empty()) {
        return;
    }

#ifdef NEKOCHAT_TEXT_SSE2
    size_t i = 0;
    const uint8_t* srcPtr = src.data();
    uint8_t* dstPtr = dst.data();
    for (; i + 16 <= src.size(); i += 16) {
        const __m128i block = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(srcPtr + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dstPtr + i), block);
    }
    for (; i < src.size(); ++i) {
        dstPtr[i] = srcPtr[i];
    }
#else
    memcpy(dst.data(), src.data(), src.size());
#endif
}

void DrawTextureRect(GLuint texture, float x, float y, float w, float h) {
    if (texture == 0) {
        return;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + w, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

void DrawPreviewPane(const AppState& app, float x, float y, float w, float h, GLuint texture,
                     int frameW, int frameH, const std::string& fallbackText) {
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.12f, 1.0f});
    DrawRect(x, y, w, h, Color{0.24f, 0.31f, 0.41f, 1.0f});

    if (texture == 0 || frameW <= 0 || frameH <= 0) {
        DrawText(app, x + 14.0f, y + 14.0f, fallbackText, Color{0.71f, 0.78f, 0.86f, 1.0f});
        return;
    }

    const float maxW = w - 20.0f;
    const float maxH = h - 20.0f;
    const float aspect = static_cast<float>(frameW) / static_cast<float>(frameH);
    float drawW = maxW;
    float drawH = drawW / aspect;
    if (drawH > maxH) {
        drawH = maxH;
        drawW = drawH * aspect;
    }

    const float drawX = x + (w - drawW) * 0.5f;
    const float drawY = y + (h - drawH) * 0.5f;
    DrawTextureRect(texture, drawX, drawY, drawW, drawH);
}

float MeasureTextWidth(const AppState& app, const std::string& text) {
    if (text.empty() || !app.hdc) {
        return 0.0f;
    }

    SIZE textSize{};
    if (!GetTextExtentPoint32A(app.hdc, text.c_str(), static_cast<int>(text.size()), &textSize)) {
        return static_cast<float>(text.size()) * 8.0f;
    }
    return static_cast<float>(textSize.cx);
}

std::string FitTextToWidth(const AppState& app, std::string text, float maxWidth) {
    if (maxWidth <= 0.0f || text.empty()) {
        return std::string();
    }
    if (MeasureTextWidth(app, text) <= maxWidth) {
        return text;
    }

    while (text.size() > 3) {
        text.pop_back();
        const std::string candidate = text + "...";
        if (MeasureTextWidth(app, candidate) <= maxWidth) {
            return candidate;
        }
    }
    return "...";
}

std::string FormatBytes(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < 4) {
        value /= 1024.0;
        ++unit;
    }

    const int decimals = (value >= 10.0 || unit == 0) ? 0 : 1;
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(decimals);
    oss << value << ' ' << kUnits[unit];
    return oss.str();
}

int FindAudioDeviceIndex(const std::vector<AudioEngine::DeviceInfo>& devices, UINT selectedId) {
    for (size_t index = 0; index < devices.size(); ++index) {
        if (devices[index].id == selectedId) {
            return static_cast<int>(index);
        }
    }
    return devices.empty() ? -1 : 0;
}

int FindScreenSourceIndex(const std::vector<ScreenShareEngine::SourceInfo>& sources,
                          const std::string& selectedId) {
    for (size_t index = 0; index < sources.size(); ++index) {
        if (sources[index].id == selectedId) {
            return static_cast<int>(index);
        }
    }
    return sources.empty() ? -1 : 0;
}

void ClearActiveModal(AppState& app) {
    app.activeModal = MODAL_NONE;
    app.activeOfferPrompt = IncomingOfferPrompt{};
    app.activeFilePrompt = PendingIncomingFilePrompt{};
}

bool HandleActiveModalAccept(AppState& app) {
    if (!app.client) {
        ClearActiveModal(app);
        return false;
    }

    bool handled = false;
    if (app.activeModal == MODAL_OFFER) {
        handled = app.client->AcceptPendingOffer(app.activeOfferPrompt.peerId);
        if (!handled) {
            std::cout << "[Offer] Request is no longer available." << std::endl;
        }
    } else if (app.activeModal == MODAL_FILE) {
        handled = app.client->AcceptPendingFilePrompt(app.activeFilePrompt.peerId,
                                                      app.activeFilePrompt.transferId);
        if (!handled) {
            std::cout << "[File] Request is no longer available." << std::endl;
        }
    }

    ClearActiveModal(app);
    return handled;
}

bool HandleActiveModalDecline(AppState& app) {
    if (!app.client) {
        ClearActiveModal(app);
        return false;
    }

    bool handled = false;
    if (app.activeModal == MODAL_OFFER) {
        app.client->DeclinePendingOffer(app.activeOfferPrompt.peerId);
        handled = true;
    } else if (app.activeModal == MODAL_FILE) {
        handled = app.client->DeclinePendingFilePrompt(app.activeFilePrompt.peerId,
                                                       app.activeFilePrompt.transferId);
        if (!handled) {
            std::cout << "[File] Request is no longer available." << std::endl;
        }
    }

    ClearActiveModal(app);
    return handled;
}

void PumpModalRequests(AppState& app) {
    if (!app.client || app.activeModal != MODAL_NONE) {
        return;
    }

    IncomingOfferPrompt offerPrompt;
    if (app.client->PopPendingOfferPrompt(offerPrompt)) {
        app.activeModal = MODAL_OFFER;
        app.activeOfferPrompt = offerPrompt;
        return;
    }

    PendingIncomingFilePrompt filePrompt;
    if (app.client->PopPendingFilePrompt(filePrompt)) {
        app.activeModal = MODAL_FILE;
        app.activeFilePrompt = filePrompt;
    }
}

size_t* GetCaretForField(AppState& app, int fieldId) {
    if (fieldId == FIELD_ID) {
        return &app.idCaret;
    }
    if (fieldId == FIELD_COMMAND) {
        return &app.commandCaret;
    }
    if (fieldId == FIELD_STREAM_TARGET) {
        return &app.streamTargetCaret;
    }
    if (fieldId == FIELD_FILE_TARGET) {
        return &app.fileTargetCaret;
    }
    if (fieldId == FIELD_FILE_PATH) {
        return &app.filePathCaret;
    }
    return nullptr;
}

bool GetActiveFieldData(AppState& app, std::string*& value, size_t*& caret) {
    value = nullptr;
    caret = nullptr;
    if (app.activeField == FIELD_ID) {
        value = &app.idInput;
        caret = &app.idCaret;
        return true;
    }
    if (app.activeField == FIELD_COMMAND) {
        value = &app.commandInput;
        caret = &app.commandCaret;
        return true;
    }
    if (app.activeField == FIELD_STREAM_TARGET) {
        value = &app.streamTargetInput;
        caret = &app.streamTargetCaret;
        return true;
    }
    if (app.activeField == FIELD_FILE_TARGET) {
        value = &app.fileTargetInput;
        caret = &app.fileTargetCaret;
        return true;
    }
    if (app.activeField == FIELD_FILE_PATH) {
        value = &app.filePathInput;
        caret = &app.filePathCaret;
        return true;
    }
    return false;
}

void ClampCaret(std::string& value, size_t& caret) {
    if (caret > value.size()) {
        caret = value.size();
    }
}

float MeasureTextSlice(const AppState& app, const std::string& text, size_t from, size_t to) {
    if (from >= to || from >= text.size()) {
        return 0.0f;
    }
    to = std::min(to, text.size());
    return MeasureTextWidth(app, text.substr(from, to - from));
}

struct TextVisibleRange {
    size_t start = 0;
    size_t end = 0;
};

TextVisibleRange ComputeVisibleRange(const AppState& app, const std::string& value, size_t caret, float maxWidth) {
    TextVisibleRange range{};
    range.start = 0;
    range.end = value.size();
    if (value.empty() || maxWidth <= 0.0f) {
        return range;
    }

    caret = std::min(caret, value.size());
    while (range.start < caret &&
           MeasureTextSlice(app, value, range.start, caret) > maxWidth) {
        ++range.start;
    }

    while (range.end > caret &&
           MeasureTextSlice(app, value, range.start, range.end) > maxWidth) {
        --range.end;
    }

    while (MeasureTextSlice(app, value, range.start, range.end) > maxWidth && range.start < range.end) {
        if (range.start < caret) {
            ++range.start;
        } else {
            --range.end;
        }
    }

    return range;
}

size_t FindCaretByPixel(const AppState& app, const std::string& value, const TextVisibleRange& range, float localX) {
    if (value.empty()) {
        return 0;
    }

    localX = std::max(0.0f, localX);
    size_t bestIndex = range.start;
    float bestDistance = 1e9f;
    for (size_t i = range.start; i <= range.end; ++i) {
        const float width = MeasureTextSlice(app, value, range.start, i);
        const float distance = std::abs(width - localX);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    return bestIndex;
}

bool Button(const AppState& app, float x, float y, float w, float h, const std::string& label) {
    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    DrawFilledRect(x, y, w, h, hot ? Color{0.26f, 0.34f, 0.46f, 1.0f} : Color{0.19f, 0.23f, 0.30f, 1.0f});
    DrawRect(x, y, w, h, Color{0.45f, 0.55f, 0.70f, 1.0f}, 1.5f);
    DrawText(app, x + 12.0f, y + 10.0f, label, Color{0.92f, 0.95f, 0.99f, 1.0f});
    return hot && app.mouseClicked;
}

bool TabButton(const AppState& app, float x, float y, float w, float h, const std::string& label, bool active) {
    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    const Color fill = active
        ? Color{0.20f, 0.29f, 0.41f, 1.0f}
        : (hot ? Color{0.15f, 0.21f, 0.29f, 1.0f} : Color{0.11f, 0.15f, 0.21f, 1.0f});
    DrawFilledRect(x, y, w, h, fill);
    DrawRect(x, y, w, h, active ? Color{0.58f, 0.74f, 0.96f, 1.0f} : Color{0.29f, 0.37f, 0.47f, 1.0f}, active ? 2.0f : 1.0f);
    DrawText(app, x + 12.0f, y + 8.0f, label, active ? Color{0.95f, 0.97f, 1.0f, 1.0f} : Color{0.78f, 0.85f, 0.93f, 1.0f});
    return hot && app.mouseClicked;
}

bool Slider(AppState& app, float x, float y, float w, float h, float minValue, float maxValue, float& value) {
    if (maxValue <= minValue || w <= 0.0f || h <= 0.0f) {
        return false;
    }

    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    DrawFilledRect(x, y, w, h, Color{0.10f, 0.14f, 0.20f, 1.0f});
    DrawRect(x, y, w, h, Color{0.30f, 0.38f, 0.48f, 1.0f});

    float ratio = std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
    DrawFilledRect(x, y, w * ratio, h, Color{0.39f, 0.62f, 0.92f, 1.0f});

    const float knobX = x + w * ratio;
    DrawFilledRect(knobX - 3.0f, y - 3.0f, 6.0f, h + 6.0f, hot ? Color{0.90f, 0.95f, 1.0f, 1.0f} : Color{0.80f, 0.88f, 0.97f, 1.0f});

    if (hot && (app.mouseClicked || app.mouseDown)) {
        const float clickRatio = std::clamp((static_cast<float>(app.mousePos.x) - x) / w, 0.0f, 1.0f);
        value = minValue + (maxValue - minValue) * clickRatio;
        return true;
    }
    return false;
}

bool TextBox(AppState& app, int fieldId, float x, float y, float w, float h,
             std::string& value, const std::string& placeholder) {
    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    const bool active = (app.activeField == fieldId);
    size_t* caret = GetCaretForField(app, fieldId);
    if (caret) {
        ClampCaret(value, *caret);
    }

    constexpr float textPaddingX = 12.0f;
    const float maxTextWidth = std::max(1.0f, w - textPaddingX * 2.0f - 2.0f);
    const size_t caretValue = caret ? *caret : value.size();
    TextVisibleRange range = ComputeVisibleRange(app, value, caretValue, maxTextWidth);

    if (hot && app.mouseClicked) {
        app.activeField = fieldId;
        if (caret) {
            const float localX = static_cast<float>(app.mousePos.x) - (x + textPaddingX);
            *caret = FindCaretByPixel(app, value, range, localX);
        }
        return true;
    }

    DrawFilledRect(x, y, w, h, active ? Color{0.12f, 0.16f, 0.21f, 1.0f} : Color{0.09f, 0.12f, 0.16f, 1.0f});
    DrawRect(x, y, w, h, active ? Color{0.52f, 0.70f, 0.94f, 1.0f} : Color{0.28f, 0.34f, 0.42f, 1.0f}, active ? 2.0f : 1.0f);

    std::string display;
    Color textColor{};
    if (value.empty() && !active) {
        display = placeholder;
        while (!display.empty() && MeasureTextWidth(app, display) > maxTextWidth) {
            display.pop_back();
        }
        textColor = Color{0.50f, 0.56f, 0.64f, 1.0f};
    } else {
        display = value.substr(range.start, range.end - range.start);
        textColor = Color{0.93f, 0.96f, 0.99f, 1.0f};
    }
    DrawText(app, x + textPaddingX, y + 10.0f, display, textColor);

    if (active && caret && (GetTickCount64() / 500ULL) % 2ULL == 0ULL) {
        const float caretOffset = MeasureTextSlice(app, value, range.start, *caret);
        float caretX = x + textPaddingX + caretOffset;
        caretX = std::min(caretX, x + w - 8.0f);
        DrawFilledRect(caretX, y + 8.0f, 2.0f, h - 16.0f, Color{0.88f, 0.92f, 0.96f, 1.0f});
    }

    return false;
}

void InsertAsciiAtCaret(std::string& value, size_t& caret, WPARAM ch) {
    if (ch >= 32 && ch <= 126) {
        ClampCaret(value, caret);
        value.insert(value.begin() + static_cast<std::ptrdiff_t>(caret), static_cast<char>(ch));
        ++caret;
    }
}

void BackspaceAtCaret(std::string& value, size_t& caret) {
    ClampCaret(value, caret);
    if (caret > 0 && !value.empty()) {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(caret - 1));
        --caret;
    }
}

void DeleteAtCaret(std::string& value, size_t& caret) {
    ClampCaret(value, caret);
    if (caret < value.size()) {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(caret));
    }
}

std::string TrimCopy(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::vector<int> GetConnectedFieldOrder(const AppState& app) {
    std::vector<int> fields{FIELD_COMMAND};
    if (app.activeTab == TAB_SCREEN) {
        fields.push_back(FIELD_STREAM_TARGET);
    } else if (app.activeTab == TAB_FILES) {
        fields.push_back(FIELD_FILE_TARGET);
        fields.push_back(FIELD_FILE_PATH);
    }
    return fields;
}

void AdvanceConnectedField(AppState& app, bool reverse) {
    auto fields = GetConnectedFieldOrder(app);
    if (fields.empty()) {
        app.activeField = FIELD_COMMAND;
        return;
    }

    auto it = std::find(fields.begin(), fields.end(), app.activeField);
    if (it == fields.end()) {
        app.activeField = fields.front();
        return;
    }

    const ptrdiff_t index = std::distance(fields.begin(), it);
    const ptrdiff_t size = static_cast<ptrdiff_t>(fields.size());
    const ptrdiff_t next =
        reverse ? ((index - 1 + size) % size) : ((index + 1) % size);
    app.activeField = fields[static_cast<size_t>(next)];
}

bool BrowseForFile(AppState& app) {
    char filePath[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.hwnd;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(filePath));
    ofn.lpstrFilter = "All Files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0Archives\0*.zip;*.rar;*.7z\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) {
        return false;
    }

    app.filePathInput = filePath;
    app.filePathCaret = app.filePathInput.size();
    app.activeField = FIELD_FILE_PATH;
    return true;
}

void OpenReceivedFilesFolder(AppState& app) {
    if (!app.client) {
        return;
    }

    const auto folder = app.client->GetReceivedFilesDirectory();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    ShellExecuteA(app.hwnd, "open", folder.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SendSelectedFile(AppState& app) {
    if (!app.connected || !app.client) {
        return;
    }

    const std::string target = TrimCopy(app.fileTargetInput);
    const std::string path = TrimCopy(app.filePathInput);
    if (target.empty()) {
        std::cout << "[Usage] file <id> <path>" << std::endl;
        app.activeField = FIELD_FILE_TARGET;
        return;
    }
    if (path.empty()) {
        std::cout << "[Usage] file <id> <path>" << std::endl;
        app.activeField = FIELD_FILE_PATH;
        return;
    }

    app.client->SendFileToPeer(target, std::filesystem::path(path));
}

void DrawValueBox(const AppState& app, float x, float y, float w, float h, const std::string& text) {
    DrawFilledRect(x, y, w, h, Color{0.09f, 0.12f, 0.17f, 1.0f});
    DrawRect(x, y, w, h, Color{0.28f, 0.34f, 0.42f, 1.0f});
    DrawText(app, x + 12.0f, y + 9.0f, FitTextToWidth(app, text, w - 24.0f),
             Color{0.92f, 0.95f, 0.99f, 1.0f});
}

void DrawActiveModal(AppState& app) {
    if (app.activeModal == MODAL_NONE) {
        return;
    }

    DrawFilledRect(0.0f, 0.0f, static_cast<float>(app.width), static_cast<float>(app.height),
                   Color{0.01f, 0.02f, 0.04f, 0.68f});

    const float modalW = 520.0f;
    const float modalH = (app.activeModal == MODAL_FILE) ? 278.0f : 214.0f;
    const float modalX = (static_cast<float>(app.width) - modalW) * 0.5f;
    const float modalY = (static_cast<float>(app.height) - modalH) * 0.5f;

    DrawFilledRect(modalX, modalY, modalW, modalH, Color{0.08f, 0.11f, 0.15f, 0.98f});
    DrawRect(modalX, modalY, modalW, modalH, Color{0.50f, 0.66f, 0.90f, 1.0f}, 2.0f);

    if (app.activeModal == MODAL_OFFER) {
        DrawText(app, modalX + 24.0f, modalY + 20.0f, "Incoming Offer", Color{0.94f, 0.97f, 1.0f, 1.0f});
        DrawText(app, modalX + 24.0f, modalY + 54.0f,
                 "Peer wants to establish a direct session:",
                 Color{0.72f, 0.80f, 0.89f, 1.0f});
        DrawValueBox(app, modalX + 24.0f, modalY + 76.0f, modalW - 48.0f, 38.0f,
                     app.activeOfferPrompt.peerId);
        DrawText(app, modalX + 24.0f, modalY + 132.0f,
                 "Accepting starts ICE gathering and responds with ANSWER.",
                 Color{0.70f, 0.78f, 0.87f, 1.0f});
    } else if (app.activeModal == MODAL_FILE) {
        DrawText(app, modalX + 24.0f, modalY + 20.0f, "Incoming File", Color{0.94f, 0.97f, 1.0f, 1.0f});
        DrawText(app, modalX + 24.0f, modalY + 52.0f,
                 "Peer is requesting file transfer:",
                 Color{0.72f, 0.80f, 0.89f, 1.0f});
        DrawText(app, modalX + 24.0f, modalY + 82.0f,
                 "From: " + app.activeFilePrompt.peerId,
                 Color{0.86f, 0.92f, 0.98f, 1.0f});
        DrawText(app, modalX + 24.0f, modalY + 106.0f,
                 "File: " + FitTextToWidth(app, app.activeFilePrompt.fileName, modalW - 100.0f),
                 Color{0.86f, 0.92f, 0.98f, 1.0f});
        DrawText(app, modalX + 24.0f, modalY + 130.0f,
                 "Size: " + FormatBytes(app.activeFilePrompt.totalSize),
                 Color{0.86f, 0.92f, 0.98f, 1.0f});
        DrawText(app, modalX + 24.0f, modalY + 154.0f,
                 "Chunks: " + std::to_string(app.activeFilePrompt.chunkCount),
                 Color{0.86f, 0.92f, 0.98f, 1.0f});
        if (app.client) {
            DrawText(app, modalX + 24.0f, modalY + 186.0f,
                     "Inbox: " + FitTextToWidth(app, app.client->GetReceivedFilesDirectory().string(), modalW - 72.0f),
                     Color{0.68f, 0.76f, 0.86f, 1.0f});
        }
    }

    const float buttonY = modalY + modalH - 54.0f;
    const float buttonW = 132.0f;
    const float gap = 10.0f;
    const float acceptX = modalX + modalW - buttonW - 24.0f;
    const float declineX = acceptX - buttonW - gap;

    if (Button(app, declineX, buttonY, buttonW, 34.0f, "Decline")) {
        HandleActiveModalDecline(app);
    }
    if (Button(app, acceptX, buttonY, buttonW, 34.0f, "Accept")) {
        HandleActiveModalAccept(app);
    }
}

void SetupProjection(const AppState& app) {
    glViewport(0, 0, app.width, app.height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(app.width), static_cast<double>(app.height), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void ConnectClient(AppState& app) {
    if (app.connected || app.idInput.empty()) {
        return;
    }

    app.client = std::make_unique<P2PClient>();
    app.client->id = app.idInput;
    app.client->SetIncomingOfferApprovalRequired(true);
    app.client->SetIncomingFileApprovalRequired(true);
    app.client->Start("ENTERYOURIP", 27015);
    app.client->PrintHelp();
    app.connected = true;
    app.activeField = FIELD_COMMAND;
    app.commandCaret = app.commandInput.size();
    app.activeTab = TAB_CONNECTION;
    app.streamTargetCaret = app.streamTargetInput.size();
    app.fileTargetCaret = app.fileTargetInput.size();
    app.filePathCaret = app.filePathInput.size();
}

void SendCommand(AppState& app) {
    if (!app.connected || !app.client || app.commandInput.empty()) {
        return;
    }

    app.client->ExecuteCommand(app.commandInput);
    app.commandInput.clear();
    app.commandCaret = 0;
}

void DrawHelpPanel(const AppState& app, float x, float y, float w, float h) {
    DrawFilledRect(x, y, w, h, Color{0.08f, 0.11f, 0.15f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});

    DrawText(app, x + 18.0f, y + 16.0f, "Quick Actions", Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 54.0f, "Type commands below:", Color{0.68f, 0.75f, 0.84f, 1.0f});
    DrawText(app, x + 18.0f, y + 78.0f, "offer alice", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 100.0f, "msg alice hello", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 122.0f, "voice on alice", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 144.0f, "reconnect alice", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 166.0f, "volume alice 70", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 188.0f, "file alice C:\\tmp\\cat.png", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 210.0f, "register neko", Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 232.0f, "stream on alice", Color{0.92f, 0.95f, 0.99f, 1.0f});
}

void DrawLinePanel(const AppState& app, float x, float y, float w, float h,
                   const std::string& title, const std::vector<std::string>& lines) {
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, title, Color{0.94f, 0.96f, 0.99f, 1.0f});

    constexpr float lineHeight = 18.0f;
    const int visibleLines = std::max(1, static_cast<int>((h - 40.0f) / lineHeight));
    const int startIndex = std::max(0, static_cast<int>(lines.size()) - visibleLines);
    float lineY = y + 42.0f;
    for (int i = startIndex; i < static_cast<int>(lines.size()); ++i) {
        DrawText(app, x + 12.0f, lineY, lines[static_cast<size_t>(i)], Color{0.80f, 0.86f, 0.93f, 1.0f});
        lineY += lineHeight;
    }
}

void DrawConnectionPanel(AppState& app, float x, float y, float w, float h) {
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, "Connection Control", Color{0.94f, 0.96f, 0.99f, 1.0f});

    if (!app.client) {
        DrawText(app, x + 18.0f, y + 46.0f, "Client is not ready.", Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    if (Button(app, x + w - 140.0f, y + 10.0f, 124.0f, 30.0f, "Refresh List")) {
        app.client->ExecuteCommand("list");
    }

    auto onlineUsers = app.client->GetOnlineUsers();
    std::sort(onlineUsers.begin(), onlineUsers.end());
    const auto connectedPeers = app.client->GetConnectedPeerIds();
    const std::unordered_set<std::string> connectedSet(
        connectedPeers.begin(), connectedPeers.end());

    const float usersBoxX = x + 10.0f;
    const float usersBoxY = y + 46.0f;
    const float usersBoxW = w - 20.0f;
    const float usersBoxH = std::min(260.0f, h * 0.50f);
    DrawFilledRect(usersBoxX, usersBoxY, usersBoxW, usersBoxH, Color{0.09f, 0.12f, 0.17f, 1.0f});
    DrawRect(usersBoxX, usersBoxY, usersBoxW, usersBoxH, Color{0.23f, 0.31f, 0.41f, 1.0f});
    DrawText(app, usersBoxX + 12.0f, usersBoxY + 10.0f,
             "Online Users (auto refresh every ~6s)", Color{0.90f, 0.95f, 0.99f, 1.0f});

    float rowY = usersBoxY + 34.0f;
    size_t shown = 0;
    if (onlineUsers.empty()) {
        DrawText(app, usersBoxX + 12.0f, rowY + 4.0f,
                 "No users online yet.", Color{0.64f, 0.72f, 0.81f, 1.0f});
    } else {
        for (const auto& peerId : onlineUsers) {
            if (rowY + 30.0f > usersBoxY + usersBoxH - 8.0f) {
                break;
            }

            DrawFilledRect(usersBoxX + 8.0f, rowY, usersBoxW - 16.0f, 28.0f, Color{0.08f, 0.10f, 0.15f, 1.0f});
            DrawRect(usersBoxX + 8.0f, rowY, usersBoxW - 16.0f, 28.0f, Color{0.20f, 0.27f, 0.35f, 1.0f});
            DrawText(app, usersBoxX + 18.0f, rowY + 7.0f, peerId, Color{0.91f, 0.95f, 0.99f, 1.0f});

            const bool iceReady = connectedSet.find(peerId) != connectedSet.end();
            DrawText(app, usersBoxX + usersBoxW - 230.0f, rowY + 7.0f,
                     iceReady ? "ICE: ready" : "ICE: pending",
                     iceReady ? Color{0.55f, 0.90f, 0.60f, 1.0f}
                              : Color{0.73f, 0.79f, 0.86f, 1.0f});

            if (Button(app, usersBoxX + usersBoxW - 94.0f, rowY + 1.0f, 76.0f, 24.0f, "Offer")) {
                app.client->ExecuteCommand("offer " + peerId);
            }

            rowY += 32.0f;
            ++shown;
        }
    }

    if (shown < onlineUsers.size()) {
        DrawText(app, usersBoxX + 12.0f, usersBoxY + usersBoxH - 20.0f,
                 "+" + std::to_string(onlineUsers.size() - shown) + " more users...",
                 Color{0.63f, 0.71f, 0.80f, 1.0f});
    }

    const float logsY = usersBoxY + usersBoxH + 12.0f;
    const float logsH = h - (logsY - y) - 10.0f;
    DrawLinePanel(app, x + 10.0f, logsY, w - 20.0f, std::max(70.0f, logsH),
                  "Connection Events", app.logBuffer.SnapshotConnection());
}

std::string VolumeLabel(float volume) {
    const int percent = static_cast<int>(volume * 100.0f + 0.5f);
    return std::to_string(percent) + "%";
}

void ResetRemotePreview(AppState& app) {
    app.remoteScreenSeq = 0;
    app.remoteScreenW = 0;
    app.remoteScreenH = 0;
    app.remoteScreenBgra.clear();
    if (app.remoteScreenTexture != 0) {
        glDeleteTextures(1, &app.remoteScreenTexture);
        app.remoteScreenTexture = 0;
    }
}

void UpdateScreenPreviewTextures(AppState& app) {
    if (!app.client) {
        return;
    }

    ScreenShareEngine::PreviewFrame localPreview;
    if (app.client->GetLocalScreenPreview(localPreview) &&
        localPreview.sequence != app.localScreenSeq) {
        UpdateTextureFromPreview(app.localScreenTexture, localPreview);
        app.localScreenSeq = localPreview.sequence;
        app.localScreenW = localPreview.width;
        app.localScreenH = localPreview.height;
    }

    if (!app.selectedRemotePeer.empty()) {
        ScreenShareEngine::PreviewFrame remotePreview;
        if (app.client->GetRemoteScreenPreviewByPeer(app.selectedRemotePeer, remotePreview) &&
            remotePreview.sequence != app.remoteScreenSeq) {
            UpdateTextureFromPreview(app.remoteScreenTexture, remotePreview);
            app.remoteScreenSeq = remotePreview.sequence;
            app.remoteScreenW = remotePreview.width;
            app.remoteScreenH = remotePreview.height;
            CopyByteBufferFast(app.remoteScreenBgra, remotePreview.bgra);
        }
    }
}

void PaintViewerContent(AppState& app, HDC hdc, const RECT& clientRect) {
    const int panelW = std::max(long(1), clientRect.right - clientRect.left);
    const int panelH = std::max(long(1), clientRect.bottom - clientRect.top);
    HDC backDc = CreateCompatibleDC(hdc);
    HBITMAP backBmp = CreateCompatibleBitmap(hdc, panelW, panelH);
    HGDIOBJ oldBmp = SelectObject(backDc, backBmp);

    RECT backRect{};
    backRect.left = 0;
    backRect.top = 0;
    backRect.right = panelW;
    backRect.bottom = panelH;
    HBRUSH bg = CreateSolidBrush(RGB(10, 12, 16));
    FillRect(backDc, &backRect, bg);
    DeleteObject(bg);

    SetBkMode(backDc, TRANSPARENT);
    SetTextColor(backDc, RGB(218, 228, 238));

    if (app.remoteScreenBgra.empty() || app.remoteScreenW <= 0 || app.remoteScreenH <= 0) {
        std::string label = app.selectedRemotePeer.empty()
            ? "No remote stream selected."
            : ("Waiting for stream from " + app.selectedRemotePeer + "...");
        TextOutA(backDc, 16, 16, label.c_str(), static_cast<int>(label.size()));
    } else {
        const float srcAspect = static_cast<float>(app.remoteScreenW) / static_cast<float>(app.remoteScreenH);
        float dstW = static_cast<float>(panelW);
        float dstH = dstW / srcAspect;
        if (dstH > static_cast<float>(panelH)) {
            dstH = static_cast<float>(panelH);
            dstW = dstH * srcAspect;
        }

        const int drawW = static_cast<int>(dstW);
        const int drawH = static_cast<int>(dstH);
        const int drawX = (panelW - drawW) / 2;
        const int drawY = (panelH - drawH) / 2;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = app.remoteScreenW;
        bmi.bmiHeader.biHeight = -app.remoteScreenH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(backDc, HALFTONE);
        StretchDIBits(
            backDc,
            drawX, drawY, drawW, drawH,
            0, 0, app.remoteScreenW, app.remoteScreenH,
            app.remoteScreenBgra.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY);

        const std::string streamLabel = app.selectedRemotePeer.empty()
            ? "Viewing stream: auto"
            : ("Viewing stream: " + app.selectedRemotePeer);
        TextOutA(backDc, 16, 16, streamLabel.c_str(), static_cast<int>(streamLabel.size()));
    }

    BitBlt(hdc, 0, 0, panelW, panelH, backDc, 0, 0, SRCCOPY);
    SelectObject(backDc, oldBmp);
    DeleteObject(backBmp);
    DeleteDC(backDc);
}

void RefreshViewerWindow(AppState& app) {
    if (!app.viewerHwnd) {
        return;
    }
    if (!IsWindow(app.viewerHwnd)) {
        app.viewerHwnd = nullptr;
        return;
    }
    InvalidateRect(app.viewerHwnd, nullptr, FALSE);
}

void OpenViewerWindow(AppState& app) {
    if (app.viewerHwnd && IsWindow(app.viewerHwnd)) {
        ShowWindow(app.viewerHwnd, SW_SHOW);
        SetForegroundWindow(app.viewerHwnd);
        return;
    }

    app.viewerHwnd = CreateWindowExA(
        0,
        VIEWER_WINDOW_CLASS,
        "NekoChat Stream Viewer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        960, 540,
        app.hwnd,
        nullptr,
        GetModuleHandle(nullptr),
        &app);
    if (app.viewerHwnd) {
        ShowWindow(app.viewerHwnd, SW_SHOW);
        InvalidateRect(app.viewerHwnd, nullptr, FALSE);
    }
}

void CloseViewerWindow(AppState& app) {
    if (app.viewerHwnd && IsWindow(app.viewerHwnd)) {
        DestroyWindow(app.viewerHwnd);
    }
    app.viewerHwnd = nullptr;
}

void DrawVoicePanel(AppState& app, float x, float y, float w, float h) {
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, "Voice Mix", Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 36.0f, "AEC + AGC + gate + limiter + per-peer mix", Color{0.68f, 0.75f, 0.84f, 1.0f});

    if (!app.client) {
        DrawText(app, x + 18.0f, y + 64.0f, "Client is not ready.", Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float settingsX = x + 10.0f;
    const float settingsY = y + 58.0f;
    const float settingsW = w - 20.0f;
    const float settingsH = 364.0f;
    DrawFilledRect(settingsX, settingsY, settingsW, settingsH, Color{0.09f, 0.12f, 0.17f, 1.0f});
    DrawRect(settingsX, settingsY, settingsW, settingsH, Color{0.23f, 0.31f, 0.41f, 1.0f});
    DrawText(app, settingsX + 12.0f, settingsY + 12.0f, "Presets", Color{0.90f, 0.95f, 0.99f, 1.0f});

    const auto preset = app.client->GetVoicePreset();
    const float presetY = settingsY + 30.0f;
    const float presetW = 96.0f;
    const float presetGap = 8.0f;
    if (TabButton(app, settingsX + 12.0f, presetY, presetW, 26.0f, "Balanced",
                  preset == AudioEngine::VoicePreset::Balanced)) {
        app.client->ApplyVoicePreset(AudioEngine::VoicePreset::Balanced);
    }
    if (TabButton(app, settingsX + 12.0f + (presetW + presetGap), presetY, presetW, 26.0f, "Headset",
                  preset == AudioEngine::VoicePreset::Headset)) {
        app.client->ApplyVoicePreset(AudioEngine::VoicePreset::Headset);
    }
    if (TabButton(app, settingsX + 12.0f + (presetW + presetGap) * 2.0f, presetY, presetW, 26.0f, "Speakers",
                  preset == AudioEngine::VoicePreset::Speakers)) {
        app.client->ApplyVoicePreset(AudioEngine::VoicePreset::Speakers);
    }
    if (TabButton(app, settingsX + 12.0f + (presetW + presetGap) * 3.0f, presetY, presetW, 26.0f, "Speaker Max",
                  preset == AudioEngine::VoicePreset::SpeakerMax)) {
        app.client->ApplyVoicePreset(AudioEngine::VoicePreset::SpeakerMax);
    }
    if (TabButton(app, settingsX + 12.0f + (presetW + presetGap) * 4.0f, presetY, presetW, 26.0f, "Conference",
                  preset == AudioEngine::VoicePreset::Conference)) {
        app.client->ApplyVoicePreset(AudioEngine::VoicePreset::Conference);
    }

    DrawText(app, settingsX + 12.0f, settingsY + 68.0f, "Echo Suppression", Color{0.90f, 0.95f, 0.99f, 1.0f});

    bool echoEnabled = app.client->IsEchoSuppressionEnabled();
    if (Button(app, settingsX + 180.0f, settingsY + 62.0f, 112.0f, 28.0f, echoEnabled ? "AEC: ON" : "AEC: OFF")) {
        app.client->SetEchoSuppressionEnabled(!echoEnabled);
        echoEnabled = !echoEnabled;
    }

    const float sliderX = settingsX + 154.0f;
    const float sliderW = std::max(120.0f, settingsW - 220.0f);
    const float valueX = sliderX + sliderW + 8.0f;

    float detectPercent = app.client->GetEchoCorrelationThreshold() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 90.0f, "Detect", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 98.0f, sliderW, 8.0f, 30.0f, 95.0f, detectPercent)) {
        app.client->SetEchoCorrelationThreshold(detectPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 90.0f, std::to_string(static_cast<int>(detectPercent + 0.5f)) + "%", Color{0.84f, 0.90f, 0.97f, 1.0f});

    float subtractPercent = app.client->GetEchoSubtractionMaxGain() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 112.0f, "Subtract", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 120.0f, sliderW, 8.0f, 50.0f, 200.0f, subtractPercent)) {
        app.client->SetEchoSubtractionMaxGain(subtractPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 112.0f, std::to_string(static_cast<int>(subtractPercent + 0.5f)) + "%", Color{0.84f, 0.90f, 0.97f, 1.0f});

    float residualPercent = app.client->GetEchoResidualAttenuation() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 134.0f, "Residual", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 142.0f, sliderW, 8.0f, 10.0f, 100.0f, residualPercent)) {
        app.client->SetEchoResidualAttenuation(residualPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 134.0f, std::to_string(static_cast<int>(residualPercent + 0.5f)) + "%", Color{0.84f, 0.90f, 0.97f, 1.0f});

    DrawText(app, settingsX + 12.0f, settingsY + 168.0f, "Capture Dynamics", Color{0.90f, 0.95f, 0.99f, 1.0f});

    bool agcEnabled = app.client->IsAutomaticGainEnabled();
    if (Button(app, settingsX + 180.0f, settingsY + 162.0f, 112.0f, 28.0f, agcEnabled ? "AGC: ON" : "AGC: OFF")) {
        app.client->SetAutomaticGainEnabled(!agcEnabled);
        agcEnabled = !agcEnabled;
    }

    float inputGainPercent = app.client->GetInputGain() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 192.0f, "Input Gain", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 200.0f, sliderW, 8.0f, 25.0f, 400.0f, inputGainPercent)) {
        app.client->SetInputGain(inputGainPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 192.0f, std::to_string(static_cast<int>(inputGainPercent + 0.5f)) + "%", Color{0.84f, 0.90f, 0.97f, 1.0f});

    float agcTargetPercent = app.client->GetAutomaticGainTargetLevel() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 214.0f, "AGC Target", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 222.0f, sliderW, 8.0f, 6.0f, 40.0f, agcTargetPercent)) {
        app.client->SetAutomaticGainTargetLevel(agcTargetPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 214.0f, std::to_string(static_cast<int>(agcTargetPercent + 0.5f)) + "%", Color{0.84f, 0.90f, 0.97f, 1.0f});

    float agcBoostPercent = app.client->GetAutomaticGainMaxBoost() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 236.0f, "Max Boost", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 244.0f, sliderW, 8.0f, 100.0f, 800.0f, agcBoostPercent)) {
        app.client->SetAutomaticGainMaxBoost(agcBoostPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 236.0f, std::to_string(static_cast<int>(agcBoostPercent + 0.5f)) + "%", Color{0.84f, 0.90f, 0.97f, 1.0f});

    float gatePercent = app.client->GetNoiseGateThreshold() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 258.0f, "Noise Gate", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 266.0f, sliderW, 8.0f, 0.2f, 8.0f, gatePercent)) {
        app.client->SetNoiseGateThreshold(gatePercent / 100.0f);
    }
    const int gateTenths = static_cast<int>(gatePercent * 10.0f + 0.5f);
    DrawText(app, valueX, settingsY + 258.0f,
             std::to_string(gateTenths / 10) + "." + std::to_string(gateTenths % 10) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    const auto inputDevices = app.client->GetInputDevices();
    const auto outputDevices = app.client->GetOutputDevices();
    const int inputIndex = FindAudioDeviceIndex(inputDevices, app.client->GetSelectedInputDeviceId());
    const int outputIndex = FindAudioDeviceIndex(outputDevices, app.client->GetSelectedOutputDeviceId());

    const float deviceRow1Y = settingsY + 292.0f;
    const float deviceRow2Y = settingsY + 326.0f;
    const float deviceBoxX = sliderX - 12.0f;
    const float deviceBoxW = std::max(120.0f, settingsW - 240.0f);
    const float navBtnW = 32.0f;
    const float nextBtnX = settingsX + settingsW - 12.0f - navBtnW;
    const float prevBtnX = nextBtnX - 6.0f - navBtnW;

    DrawText(app, sliderX - 126.0f, deviceRow1Y + 4.0f, "Input", Color{0.78f, 0.85f, 0.93f, 1.0f});
    DrawValueBox(app, deviceBoxX, deviceRow1Y, deviceBoxW, 28.0f,
                 (inputIndex >= 0 && inputIndex < static_cast<int>(inputDevices.size()))
                     ? inputDevices[static_cast<size_t>(inputIndex)].name
                     : "No input devices");
    if (!inputDevices.empty() && Button(app, prevBtnX, deviceRow1Y, navBtnW, 28.0f, "<")) {
        const int nextIndex = (inputIndex <= 0)
            ? static_cast<int>(inputDevices.size()) - 1
            : inputIndex - 1;
        app.client->SetInputDevice(inputDevices[static_cast<size_t>(nextIndex)].id);
    }
    if (!inputDevices.empty() && Button(app, nextBtnX, deviceRow1Y, navBtnW, 28.0f, ">")) {
        const int nextIndex = (inputIndex + 1) % static_cast<int>(inputDevices.size());
        app.client->SetInputDevice(inputDevices[static_cast<size_t>(nextIndex)].id);
    }

    DrawText(app, sliderX - 126.0f, deviceRow2Y + 4.0f, "Output", Color{0.78f, 0.85f, 0.93f, 1.0f});
    DrawValueBox(app, deviceBoxX, deviceRow2Y, deviceBoxW, 28.0f,
                 (outputIndex >= 0 && outputIndex < static_cast<int>(outputDevices.size()))
                     ? outputDevices[static_cast<size_t>(outputIndex)].name
                     : "No output devices");
    if (!outputDevices.empty() && Button(app, prevBtnX, deviceRow2Y, navBtnW, 28.0f, "<")) {
        const int nextIndex = (outputIndex <= 0)
            ? static_cast<int>(outputDevices.size()) - 1
            : outputIndex - 1;
        app.client->SetOutputDevice(outputDevices[static_cast<size_t>(nextIndex)].id);
    }
    if (!outputDevices.empty() && Button(app, nextBtnX, deviceRow2Y, navBtnW, 28.0f, ">")) {
        const int nextIndex = (outputIndex + 1) % static_cast<int>(outputDevices.size());
        app.client->SetOutputDevice(outputDevices[static_cast<size_t>(nextIndex)].id);
    }

    auto peers = app.client->GetPeerIds();
    std::sort(peers.begin(), peers.end());
    if (peers.empty()) {
        DrawText(app, x + 18.0f, settingsY + settingsH + 22.0f, "No participants yet.", Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float rowHeight = 52.0f;
    float rowY = settingsY + settingsH + 12.0f;
    size_t shown = 0;

    for (const auto& peerId : peers) {
        if (rowY + rowHeight > y + h - 10.0f) {
            break;
        }

        DrawFilledRect(x + 10.0f, rowY, w - 20.0f, rowHeight - 6.0f, Color{0.09f, 0.12f, 0.17f, 1.0f});
        DrawRect(x + 10.0f, rowY, w - 20.0f, rowHeight - 6.0f, Color{0.23f, 0.31f, 0.41f, 1.0f});
        DrawText(app, x + 20.0f, rowY + 14.0f, peerId, Color{0.92f, 0.95f, 0.99f, 1.0f});

        const float buttonW = 56.0f;
        const float buttonGap = 8.0f;
        const float buttonY = rowY + 9.0f;
        const float rightPad = 14.0f;
        const float fullBtnX = x + w - rightPad - buttonW;
        const float muteBtnX = fullBtnX - buttonGap - buttonW;

        const float sliderX2 = x + 170.0f;
        const float sliderY = rowY + 19.0f;
        const float sliderH = 10.0f;
        const float percentTextW = 44.0f;
        const float sliderW2 = std::max(120.0f, muteBtnX - sliderX2 - percentTextW - 8.0f);

        float volume = app.client->GetPeerVolume(peerId);
        float adjusted = volume;
        if (Slider(app, sliderX2, sliderY, sliderW2, sliderH, 0.0f, 2.0f, adjusted)) {
            app.client->SetPeerVolume(peerId, adjusted);
            volume = adjusted;
        }

        DrawText(app, sliderX2 + sliderW2 + 10.0f, rowY + 12.0f, VolumeLabel(volume), Color{0.85f, 0.91f, 0.98f, 1.0f});

        if (Button(app, muteBtnX, buttonY, buttonW, 28.0f, "Mute")) {
            app.client->SetPeerVolume(peerId, 0.0f);
        }
        if (Button(app, fullBtnX, buttonY, buttonW, 28.0f, "100%")) {
            app.client->SetPeerVolume(peerId, 1.0f);
        }

        rowY += rowHeight;
        ++shown;
    }

    if (shown < peers.size()) {
        DrawText(app, x + 18.0f, y + h - 24.0f,
                 "+" + std::to_string(peers.size() - shown) + " more peers...",
                 Color{0.66f, 0.73f, 0.81f, 1.0f});
    }
}
void DrawScreenPanel(AppState& app, float x, float y, float w, float h) {
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, "Screen Stream", Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 36.0f, "Native Windows capture + UDP streaming", Color{0.68f, 0.75f, 0.84f, 1.0f});

    if (!app.client) {
        DrawText(app, x + 18.0f, y + 64.0f, "Client is not ready.", Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float controlsY = y + 54.0f;
    DrawText(app, x + 12.0f, controlsY + 6.0f, "Target:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    TextBox(app, FIELD_STREAM_TARGET, x + 72.0f, controlsY, 200.0f, 34.0f, app.streamTargetInput, "alice / *");

    const float startBtnX = x + 286.0f;
    const float startBtnW = 112.0f;
    const float broadcastBtnX = startBtnX + startBtnW + 8.0f;
    const float broadcastBtnW = 128.0f;
    const float viewerBtnX = broadcastBtnX + broadcastBtnW + 8.0f;
    const float viewerBtnW = 122.0f;

    const bool streaming = app.client->IsScreenShareRunning();
    if (Button(app, startBtnX, controlsY, startBtnW, 34.0f, streaming ? "Stop Stream" : "Start Stream")) {
        if (streaming) {
            app.client->StopScreenShare();
        } else {
            const std::string target = app.streamTargetInput.empty() ? "*" : app.streamTargetInput;
            app.client->StartScreenShare(target);
        }
    }

    if (Button(app, broadcastBtnX, controlsY, broadcastBtnW, 34.0f, "Broadcast (*)")) {
        app.streamTargetInput = "*";
        app.streamTargetCaret = app.streamTargetInput.size();
        app.client->StartScreenShare("*");
    }

    bool viewerOpen = app.viewerHwnd && IsWindow(app.viewerHwnd);
    if (!viewerOpen) {
        app.viewerHwnd = nullptr;
    }
    if (Button(app, viewerBtnX, controlsY, viewerBtnW, 34.0f, viewerOpen ? "Close Viewer" : "Open Viewer")) {
        if (viewerOpen) {
            CloseViewerWindow(app);
        } else {
            OpenViewerWindow(app);
        }
        viewerOpen = app.viewerHwnd && IsWindow(app.viewerHwnd);
    }

    auto connectedPeers = app.client->GetConnectedPeerIds();
    std::sort(connectedPeers.begin(), connectedPeers.end());

    const float peersY = controlsY + 44.0f;
    DrawText(app, x + 12.0f, peersY + 4.0f, "ICE peers:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    float peerBtnX = x + 84.0f;
    float peerBtnY = peersY;
    float peersBottom = peersY + 26.0f;
    if (connectedPeers.empty()) {
        DrawText(app, x + 114.0f, peersY + 4.0f,
                 "No active ICE peers yet.", Color{0.61f, 0.69f, 0.78f, 1.0f});
        peersBottom = peersY + 18.0f;
    } else {
        for (const auto& peerId : connectedPeers) {
            const float buttonW = std::clamp(MeasureTextWidth(app, peerId) + 26.0f, 86.0f, 180.0f);
            if (peerBtnX + buttonW > x + w - 14.0f) {
                peerBtnX = x + 84.0f;
                peerBtnY += 32.0f;
            }
            if (TabButton(app, peerBtnX, peerBtnY, buttonW, 26.0f, peerId,
                          app.streamTargetInput == peerId)) {
                app.streamTargetInput = peerId;
                app.streamTargetCaret = app.streamTargetInput.size();
                app.activeField = FIELD_STREAM_TARGET;
            }
            peerBtnX += buttonW + 8.0f;
            peersBottom = std::max(peersBottom, peerBtnY + 26.0f);
        }
    }

    const auto sources = app.client->GetScreenShareSources();
    const int sourceIndex = FindScreenSourceIndex(sources, app.client->GetScreenShareSourceId());
    const float sourceY = peersBottom + 14.0f;
    const float sourceBoxX = x + 118.0f;
    const float sourceBoxW = std::max(180.0f, w - 220.0f);
    const float navBtnW = 32.0f;
    const float nextBtnX = sourceBoxX + sourceBoxW + 8.0f;
    const float prevBtnX = nextBtnX + navBtnW + 6.0f > x + w - 12.0f
        ? x + w - 12.0f - navBtnW * 2.0f - 6.0f
        : sourceBoxX + sourceBoxW + 8.0f;
    const float actualNextBtnX = prevBtnX + navBtnW + 6.0f;

    DrawText(app, x + 12.0f, sourceY + 6.0f, "Source:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    DrawValueBox(app, sourceBoxX, sourceY, std::max(120.0f, prevBtnX - sourceBoxX - 8.0f), 30.0f,
                 (sourceIndex >= 0 && sourceIndex < static_cast<int>(sources.size()))
                     ? sources[static_cast<size_t>(sourceIndex)].name
                     : "Desktop");
    if (!sources.empty() && Button(app, prevBtnX, sourceY, navBtnW, 30.0f, "<")) {
        const int nextIndex = (sourceIndex <= 0)
            ? static_cast<int>(sources.size()) - 1
            : sourceIndex - 1;
        app.client->SetScreenShareSource(sources[static_cast<size_t>(nextIndex)].id);
    }
    if (!sources.empty() && Button(app, actualNextBtnX, sourceY, navBtnW, 30.0f, ">")) {
        const int nextIndex = (sourceIndex + 1) % static_cast<int>(sources.size());
        app.client->SetScreenShareSource(sources[static_cast<size_t>(nextIndex)].id);
    }

    const std::string activeTarget = app.client->GetScreenShareTarget();
    DrawText(app, x + 12.0f, sourceY + 40.0f,
             "Tx: " + activeTarget + (streaming ? " (LIVE)" : " (idle)"),
             streaming ? Color{0.53f, 0.89f, 0.57f, 1.0f} : Color{0.75f, 0.79f, 0.85f, 1.0f});

    auto streamIds = app.client->GetRemoteScreenStreamIds();
    std::sort(streamIds.begin(), streamIds.end());

    if (!app.selectedRemotePeer.empty()) {
        auto it = std::find(streamIds.begin(), streamIds.end(), app.selectedRemotePeer);
        if (it == streamIds.end()) {
            app.selectedRemotePeer.clear();
            app.remoteScreenPeer.clear();
            ResetRemotePreview(app);
        }
    }
    if (app.selectedRemotePeer.empty() && !streamIds.empty()) {
        app.selectedRemotePeer = streamIds.front();
    }
    if (app.remoteScreenPeer != app.selectedRemotePeer) {
        app.remoteScreenPeer = app.selectedRemotePeer;
        ResetRemotePreview(app);
    }

    float watchY = sourceY + 62.0f;
    DrawText(app, x + 12.0f, watchY + 4.0f, "Watch:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    float watchBtnX = x + 72.0f;
    float watchBtnY = watchY;
    float watchRowBottom = watchY + 28.0f;
    if (streamIds.empty()) {
        DrawText(app, x + 72.0f, watchY + 4.0f, "No incoming streams yet", Color{0.61f, 0.69f, 0.78f, 1.0f});
    } else {
        for (const auto& peerId : streamIds) {
            const float buttonW = std::clamp(MeasureTextWidth(app, peerId) + 26.0f, 86.0f, 180.0f);
            if (watchBtnX + buttonW > x + w - 14.0f) {
                watchBtnX = x + 72.0f;
                watchBtnY += 32.0f;
            }
            if (TabButton(app, watchBtnX, watchBtnY, buttonW, 26.0f, peerId, app.selectedRemotePeer == peerId)) {
                app.selectedRemotePeer = peerId;
                app.remoteScreenPeer = peerId;
                ResetRemotePreview(app);
            }
            watchBtnX += buttonW + 8.0f;
            watchRowBottom = std::max(watchRowBottom, watchBtnY + 26.0f);
        }
    }

    const float sliderY = watchRowBottom + 14.0f;
    const float sliderW = std::max(120.0f, w - 220.0f);
    const float sliderX = x + 118.0f;

    float fpsValue = static_cast<float>(app.client->GetScreenShareFps());
    DrawText(app, x + 12.0f, sliderY + 2.0f, "FPS", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, sliderY + 8.0f, sliderW, 8.0f, 1.0f, 60.0f, fpsValue)) {
        app.client->SetScreenShareFps(static_cast<int>(fpsValue + 0.5f));
    }
    DrawText(app, sliderX + sliderW + 10.0f, sliderY + 2.0f,
             std::to_string(static_cast<int>(fpsValue + 0.5f)), Color{0.84f, 0.90f, 0.97f, 1.0f});

    float qualityValue = static_cast<float>(app.client->GetScreenShareQuality());
    DrawText(app, x + 12.0f, sliderY + 24.0f, "Quality", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, sliderY + 30.0f, sliderW, 8.0f, 20.0f, 95.0f, qualityValue)) {
        app.client->SetScreenShareQuality(static_cast<int>(qualityValue + 0.5f));
    }
    DrawText(app, sliderX + sliderW + 10.0f, sliderY + 24.0f,
             std::to_string(static_cast<int>(qualityValue + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    float scaleValue = static_cast<float>(app.client->GetScreenShareScalePercent());
    DrawText(app, x + 12.0f, sliderY + 46.0f, "Scale", Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, sliderY + 52.0f, sliderW, 8.0f, 20.0f, 100.0f, scaleValue)) {
        app.client->SetScreenShareScalePercent(static_cast<int>(scaleValue + 0.5f));
    }
    DrawText(app, sliderX + sliderW + 10.0f, sliderY + 46.0f,
             std::to_string(static_cast<int>(scaleValue + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    UpdateScreenPreviewTextures(app);

    const float previewY = sliderY + 78.0f;
    const float previewH = h - (previewY - y) - 10.0f;
    const float previewW = (w - 30.0f) * 0.5f;

    DrawText(app, x + 12.0f, previewY - 22.0f, "Local Capture Preview", Color{0.86f, 0.92f, 0.99f, 1.0f});
    DrawPreviewPane(app, x + 10.0f, previewY, previewW, previewH,
                    app.localScreenTexture, app.localScreenW, app.localScreenH,
                    "No local frame yet");

    DrawText(app, x + 20.0f + previewW, previewY - 22.0f,
             app.selectedRemotePeer.empty() ? "Incoming Stream Preview"
                                            : ("Incoming: " + app.selectedRemotePeer),
             Color{0.86f, 0.92f, 0.99f, 1.0f});
    DrawPreviewPane(app, x + 20.0f + previewW, previewY, previewW, previewH,
                    app.remoteScreenTexture, app.remoteScreenW, app.remoteScreenH,
                    app.selectedRemotePeer.empty() ? "No remote stream yet"
                                                   : "Waiting for selected stream");
}
void DrawFilesPanel(AppState& app, float x, float y, float w, float h) {
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, "File Transfer", Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 36.0f, "Reliable chunks over ICE with ACK + retry", Color{0.68f, 0.75f, 0.84f, 1.0f});

    if (!app.client) {
        DrawText(app, x + 18.0f, y + 64.0f, "Client is not ready.", Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float row1Y = y + 58.0f;
    DrawText(app, x + 12.0f, row1Y + 6.0f, "Peer:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    TextBox(app, FIELD_FILE_TARGET, x + 72.0f, row1Y, 180.0f, 34.0f,
            app.fileTargetInput, "alice");

    const float openBtnW = 126.0f;
    const float sendBtnW = 108.0f;
    const float browseBtnW = 102.0f;
    const float buttonGap = 8.0f;
    const float openBtnX = x + w - openBtnW - 12.0f;
    const float sendBtnX = openBtnX - buttonGap - sendBtnW;
    const float browseBtnX = sendBtnX - buttonGap - browseBtnW;

    if (Button(app, browseBtnX, row1Y, browseBtnW, 34.0f, "Browse")) {
        BrowseForFile(app);
    }
    if (Button(app, sendBtnX, row1Y, sendBtnW, 34.0f, "Send File")) {
        SendSelectedFile(app);
    }
    if (Button(app, openBtnX, row1Y, openBtnW, 34.0f, "Open Inbox")) {
        OpenReceivedFilesFolder(app);
    }

    const float row2Y = row1Y + 46.0f;
    DrawText(app, x + 12.0f, row2Y + 6.0f, "Path:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    TextBox(app, FIELD_FILE_PATH, x + 72.0f, row2Y,
            w - 84.0f, 36.0f, app.filePathInput, "C:\\tmp\\cat.png");

    const auto inboxDir = app.client->GetReceivedFilesDirectory();
    DrawText(app, x + 12.0f, row2Y + 48.0f,
             "Inbox: " + inboxDir.string(),
             Color{0.62f, 0.70f, 0.79f, 1.0f});
    DrawText(app, x + 12.0f, row2Y + 70.0f,
             "Console command still works: file <peer> <path>",
             Color{0.62f, 0.70f, 0.79f, 1.0f});

    auto peers = app.client->GetConnectedPeerIds();
    std::sort(peers.begin(), peers.end());
    const float peersY = row2Y + 106.0f;
    DrawText(app, x + 12.0f, peersY, "ICE peers:", Color{0.82f, 0.89f, 0.96f, 1.0f});

    float btnX = x + 112.0f;
    float btnY = peersY - 6.0f;
    float bottomY = btnY + 26.0f;
    if (peers.empty()) {
        DrawText(app, x + 112.0f, peersY, "No active ICE peers yet. Use offer first.",
                 Color{0.62f, 0.70f, 0.79f, 1.0f});
        bottomY = peersY + 18.0f;
    } else {
        for (const auto& peerId : peers) {
            const float buttonW = std::clamp(MeasureTextWidth(app, peerId) + 26.0f, 86.0f, 180.0f);
            if (btnX + buttonW > x + w - 14.0f) {
                btnX = x + 112.0f;
                btnY += 32.0f;
            }
            if (TabButton(app, btnX, btnY, buttonW, 26.0f, peerId,
                          app.fileTargetInput == peerId)) {
                app.fileTargetInput = peerId;
                app.fileTargetCaret = app.fileTargetInput.size();
                app.activeField = FIELD_FILE_TARGET;
            }
            btnX += buttonW + 8.0f;
            bottomY = std::max(bottomY, btnY + 26.0f);
        }
    }

    const float helpY = bottomY + 20.0f;
    DrawFilledRect(x + 10.0f, helpY, w - 20.0f, std::max(110.0f, h - (helpY - y) - 10.0f),
                   Color{0.09f, 0.12f, 0.17f, 1.0f});
    DrawRect(x + 10.0f, helpY, w - 20.0f, std::max(110.0f, h - (helpY - y) - 10.0f),
             Color{0.23f, 0.31f, 0.41f, 1.0f});
    DrawText(app, x + 24.0f, helpY + 16.0f, "How It Works", Color{0.90f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 24.0f, helpY + 42.0f, "1. Choose a peer with an active ICE session.", Color{0.82f, 0.89f, 0.96f, 1.0f});
    DrawText(app, x + 24.0f, helpY + 64.0f, "2. Pick a file or type a path manually.", Color{0.82f, 0.89f, 0.96f, 1.0f});
    DrawText(app, x + 24.0f, helpY + 86.0f, "3. The receiver saves files into ReceivedFiles/<your id>/<peer>/", Color{0.82f, 0.89f, 0.96f, 1.0f});
    DrawText(app, x + 24.0f, helpY + 108.0f, "4. Progress and errors appear in the logs panel.", Color{0.82f, 0.89f, 0.96f, 1.0f});
}

void RenderConnectScreen(AppState& app) {
    DrawText(app, 120.0f, 120.0f, "NekoChat OpenGL Client", Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, 120.0f, 150.0f, "Pure OpenGL application for the P2P client.", Color{0.68f, 0.75f, 0.84f, 1.0f});

    const float panelX = 120.0f;
    const float panelY = 220.0f;
    const float panelW = 480.0f;
    const float panelH = 170.0f;

    DrawFilledRect(panelX, panelY, panelW, panelH, Color{0.08f, 0.11f, 0.15f, 1.0f});
    DrawRect(panelX, panelY, panelW, panelH, Color{0.22f, 0.28f, 0.36f, 1.0f});

    DrawText(app, panelX + 20.0f, panelY + 20.0f, "Enter your ID", Color{0.90f, 0.94f, 0.99f, 1.0f});
    TextBox(app, FIELD_ID, panelX + 20.0f, panelY + 54.0f, panelW - 40.0f, 46.0f, app.idInput, "alice");

    if (Button(app, panelX + 20.0f, panelY + 118.0f, 180.0f, 34.0f, "Connect")) {
        ConnectClient(app);
    }
}

void RenderConnectedScreen(AppState& app) {
    DrawText(app, 24.0f, 20.0f, "NekoChat OpenGL", Color{0.96f, 0.97f, 0.99f, 1.0f});
    DrawText(app, 24.0f, 44.0f, "ID: " + app.idInput, Color{0.68f, 0.75f, 0.84f, 1.0f});

    const float leftX = 24.0f;
    const float leftY = 84.0f;
    const float leftW = 260.0f;
    const float leftH = static_cast<float>(app.height) - 108.0f;
    DrawHelpPanel(app, leftX, leftY, leftW, leftH);

    float buttonY = leftY + 236.0f;
    if (Button(app, leftX + 18.0f, buttonY, leftW - 36.0f, 34.0f, "List Users")) {
        app.client->ExecuteCommand("list");
    }
    buttonY += 46.0f;
    if (Button(app, leftX + 18.0f, buttonY, leftW - 36.0f, 34.0f, "Voice Off")) {
        app.client->ExecuteCommand("voice off");
    }
    buttonY += 46.0f;
    if (Button(app, leftX + 18.0f, buttonY, leftW - 36.0f, 34.0f, "Broadcast Voice")) {
        app.client->ExecuteCommand("brdvoice on");
    }

    const float logX = leftX + leftW + 20.0f;
    const float logY = 24.0f;
    const float logW = static_cast<float>(app.width) - logX - 24.0f;
    const float logH = static_cast<float>(app.height) - 136.0f;

    const float tabY = logY;
    const float tabH = 34.0f;
    const float tabW = 118.0f;
    const float tabGap = 10.0f;

    if (TabButton(app, logX, tabY, tabW, tabH, "Connection", app.activeTab == TAB_CONNECTION)) {
        app.activeTab = TAB_CONNECTION;
    }
    if (TabButton(app, logX + tabW + tabGap, tabY, tabW, tabH, "Logs", app.activeTab == TAB_LOGS)) {
        app.activeTab = TAB_LOGS;
    }
    if (TabButton(app, logX + (tabW + tabGap) * 2.0f, tabY, tabW, tabH, "Voice", app.activeTab == TAB_VOICE)) {
        app.activeTab = TAB_VOICE;
    }
    if (TabButton(app, logX + (tabW + tabGap) * 3.0f, tabY, tabW, tabH, "Screen", app.activeTab == TAB_SCREEN)) {
        app.activeTab = TAB_SCREEN;
    }
    if (TabButton(app, logX + (tabW + tabGap) * 4.0f, tabY, tabW, tabH, "Files", app.activeTab == TAB_FILES)) {
        app.activeTab = TAB_FILES;
    }

    const float panelY = tabY + tabH + 10.0f;
    const float panelH = logH - tabH - 10.0f;
    if (app.activeTab == TAB_CONNECTION) {
        DrawConnectionPanel(app, logX, panelY, logW, panelH);
    } else if (app.activeTab == TAB_LOGS) {
        DrawLinePanel(app, logX, panelY, logW, panelH, "All Logs", app.logBuffer.SnapshotAll());
    } else if (app.activeTab == TAB_VOICE) {
        DrawVoicePanel(app, logX, panelY, logW, panelH);
    } else if (app.activeTab == TAB_SCREEN) {
        DrawScreenPanel(app, logX, panelY, logW, panelH);
    } else {
        DrawFilesPanel(app, logX, panelY, logW, panelH);
    }

    const float inputY = static_cast<float>(app.height) - 88.0f;
    TextBox(app, FIELD_COMMAND, logX, inputY, logW - 132.0f, 44.0f, app.commandInput,
            "file alice C:\\tmp\\cat.png / stream on alice / offer alice");
    if (Button(app, logX + logW - 108.0f, inputY, 108.0f, 44.0f, "Send")) {
        SendCommand(app);
    }
}

void Render(AppState& app) {
    PumpModalRequests(app);
    SetupProjection(app);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    DrawFilledRect(0.0f, 0.0f, static_cast<float>(app.width), static_cast<float>(app.height),
                   Color{0.05f, 0.06f, 0.08f, 1.0f});

    const bool modalActive = (app.activeModal != MODAL_NONE);
    const bool savedMouseClicked = app.mouseClicked;
    const bool savedMouseDown = app.mouseDown;

    if (app.connected) {
        UpdateScreenPreviewTextures(app);
        if (modalActive) {
            app.mouseClicked = false;
            app.mouseDown = false;
            RenderConnectedScreen(app);
            app.mouseClicked = savedMouseClicked;
            app.mouseDown = savedMouseDown;
            DrawActiveModal(app);
        } else {
            RenderConnectedScreen(app);
        }
    } else {
        RenderConnectScreen(app);
    }

    RefreshViewerWindow(app);
    app.mouseClicked = false;
    SwapBuffers(app.hdc);
}

bool InitOpenGL(AppState& app) {
    app.hdc = GetDC(app.hwnd);
    if (!app.hdc) {
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(app.hdc, &pfd);
    if (pixelFormat == 0 || !SetPixelFormat(app.hdc, pixelFormat, &pfd)) {
        return false;
    }

    app.glrc = wglCreateContext(app.hdc);
    if (!app.glrc || !wglMakeCurrent(app.hdc, app.glrc)) {
        return false;
    }

    app.font = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, FF_DONTCARE, "Consolas");
    SelectObject(app.hdc, app.font);

    app.fontBase = glGenLists(96);
    if (!wglUseFontBitmapsA(app.hdc, 32, 96, app.fontBase)) {
        return false;
    }

    return true;
}

void Shutdown(AppState& app) {
    CloseViewerWindow(app);

    if (app.client) {
        app.client->Stop();
        app.client.reset();
    }

    std::cout.flush();
    std::cerr.flush();

    if (app.oldCout) {
        std::cout.rdbuf(app.oldCout);
    }
    if (app.oldCerr) {
        std::cerr.rdbuf(app.oldCerr);
    }

    if (app.fontBase != 0) {
        glDeleteLists(app.fontBase, 96);
        app.fontBase = 0;
    }
    if (app.localScreenTexture != 0) {
        glDeleteTextures(1, &app.localScreenTexture);
        app.localScreenTexture = 0;
    }
    if (app.remoteScreenTexture != 0) {
        glDeleteTextures(1, &app.remoteScreenTexture);
        app.remoteScreenTexture = 0;
    }
    if (app.font) {
        DeleteObject(app.font);
        app.font = nullptr;
    }
    if (app.glrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(app.glrc);
        app.glrc = nullptr;
    }
    if (app.hdc && app.hwnd) {
        ReleaseDC(app.hwnd, app.hdc);
        app.hdc = nullptr;
    }
}

LRESULT CALLBACK ViewerWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* state = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (app) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            PaintViewerContent(*app, dc, clientRect);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app && app->viewerHwnd == hwnd) {
            app->viewerHwnd = nullptr;
        }
        return 0;
    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* state = reinterpret_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_SIZE:
        if (app) {
            app->width = LOWORD(lParam);
            app->height = HIWORD(lParam);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app) {
            app->mousePos.x = GET_X_LPARAM(lParam);
            app->mousePos.y = GET_Y_LPARAM(lParam);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app) {
            app->mousePos.x = GET_X_LPARAM(lParam);
            app->mousePos.y = GET_Y_LPARAM(lParam);
            app->mouseDown = false;
            app->mouseClicked = true;
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (app) {
            app->mousePos.x = GET_X_LPARAM(lParam);
            app->mousePos.y = GET_Y_LPARAM(lParam);
            app->mouseDown = true;
        }
        return 0;
    case WM_CHAR: {
        if (!app) {
            return 0;
        }
        if (wParam == '\r') {
            if (app->activeField == FIELD_ID && !app->connected) {
                ConnectClient(*app);
            } else if (app->activeField == FIELD_COMMAND && app->connected) {
                SendCommand(*app);
            } else if (app->activeField == FIELD_STREAM_TARGET && app->connected && app->client) {
                const std::string target = app->streamTargetInput.empty() ? "*" : app->streamTargetInput;
                app->client->StartScreenShare(target);
            } else if ((app->activeField == FIELD_FILE_TARGET ||
                        app->activeField == FIELD_FILE_PATH) &&
                       app->connected && app->client) {
                SendSelectedFile(*app);
            }
            return 0;
        }
        if (wParam == '\b' || wParam == '\t') {
            return 0;
        }

        std::string* value = nullptr;
        size_t* caret = nullptr;
        if (GetActiveFieldData(*app, value, caret) && value && caret) {
            InsertAsciiAtCaret(*value, *caret, wParam);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (!app) {
            return 0;
        }
        if (app->activeModal != MODAL_NONE) {
            if (wParam == VK_ESCAPE) {
                HandleActiveModalDecline(*app);
            } else if (wParam == VK_RETURN) {
                HandleActiveModalAccept(*app);
            }
            return 0;
        }
        if (wParam == VK_TAB) {
            if (!app->connected) {
                app->activeField = FIELD_ID;
            } else {
                const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                AdvanceConnectedField(*app, reverse);
            }
            return 0;
        }

        std::string* value = nullptr;
        size_t* caret = nullptr;
        if (!GetActiveFieldData(*app, value, caret) || !value || !caret) {
            return 0;
        }

        switch (wParam) {
        case VK_BACK:
            BackspaceAtCaret(*value, *caret);
            return 0;
        case VK_DELETE:
            DeleteAtCaret(*value, *caret);
            return 0;
        case VK_LEFT:
            if (*caret > 0) {
                --(*caret);
            }
            return 0;
        case VK_RIGHT:
            if (*caret < value->size()) {
                ++(*caret);
            }
            return 0;
        case VK_HOME:
            *caret = 0;
            return 0;
        case VK_END:
            *caret = value->size();
            return 0;
        default:
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

} 

int main() {
    HINSTANCE instance = GetModuleHandle(nullptr);

    WNDCLASSA wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = "NekoChatOpenGLWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassA(&wc);

    WNDCLASSA viewerWc{};
    viewerWc.style = CS_HREDRAW | CS_VREDRAW;
    viewerWc.lpfnWndProc = ViewerWindowProc;
    viewerWc.hInstance = instance;
    viewerWc.lpszClassName = VIEWER_WINDOW_CLASS;
    viewerWc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    viewerWc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassA(&viewerWc);

    AppState app;
    app.oldCout = std::cout.rdbuf(&app.logBuffer);
    app.oldCerr = std::cerr.rdbuf(&app.logBuffer);
    app.idInput = "user";
    app.streamTargetInput.clear();
    app.fileTargetInput.clear();
    app.filePathInput.clear();
    app.activeField = FIELD_ID;
    app.idCaret = app.idInput.size();
    app.streamTargetCaret = app.streamTargetInput.size();
    app.fileTargetCaret = app.fileTargetInput.size();
    app.filePathCaret = app.filePathInput.size();

    app.hwnd = CreateWindowA(
        wc.lpszClassName,
        "NekoChat OpenGL",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        app.width, app.height,
        nullptr, nullptr, instance, &app);

    if (!app.hwnd || !InitOpenGL(app)) {
        Shutdown(app);
        return 1;
    }

    ShowWindow(app.hwnd, SW_SHOW);
    UpdateWindow(app.hwnd);

    MSG msg{};
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                Shutdown(app);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        Render(app);
        Sleep(16);
    }
}








