#define NEKOCHAT_NO_MAIN
#include "P2PChat.cpp"

#include <gl/GL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>
#include <windowsx.h>

#pragma comment(lib, "opengl32.lib")

namespace
{

struct GuiLogBuffer : public std::streambuf
{
    std::mutex mtx;
    std::vector<std::string> allLines;
    std::vector<std::string> connectionLines;
    std::string currentLine;

    int overflow(int ch) override
    {
        if (ch == EOF)
        {
            return traits_type::not_eof(ch);
        }

        std::lock_guard<std::mutex> lock(mtx);
        if (ch == '\n')
        {
            PushLineLocked(currentLine);
            currentLine.clear();
        }
        else if (ch != '\r')
        {
            currentLine.push_back(static_cast<char>(ch));
        }
        return ch;
    }

    int sync() override
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!currentLine.empty())
        {
            PushLineLocked(currentLine);
            currentLine.clear();
        }
        return 0;
    }

    std::vector<std::string> SnapshotAll()
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::string> snapshot = allLines;
        if (!currentLine.empty())
        {
            snapshot.push_back(currentLine);
        }
        return snapshot;
    }

    std::vector<std::string> SnapshotConnection()
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::string> snapshot = connectionLines;
        if (!currentLine.empty() && IsConnectionLine(currentLine))
        {
            snapshot.push_back(currentLine);
        }
        return snapshot;
    }

  private:
    static constexpr size_t MAX_GUI_LOG_LINES = 1000;

    static bool IsConnectionLine(const std::string &line)
    {
        if (line.empty())
        {
            return false;
        }
        return line.rfind("[Audio]", 0) != 0 &&
               line.rfind("[AudioCtl]", 0) != 0;
    }

    static void TrimLinesLocked(std::vector<std::string> &lines)
    {
        if (lines.size() > MAX_GUI_LOG_LINES)
        {
            lines.erase(lines.begin(),
                        lines.begin() + static_cast<std::ptrdiff_t>(
                                            lines.size() - MAX_GUI_LOG_LINES));
        }
    }

    void PushLineLocked(const std::string &line)
    {
        allLines.push_back(line);
        TrimLinesLocked(allLines);
        if (IsConnectionLine(line))
        {
            connectionLines.push_back(line);
            TrimLinesLocked(connectionLines);
        }
    }
};

struct Color
{
    float r;
    float g;
    float b;
    float a;
};

struct AppState
{
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
    size_t idCaret = 0;
    size_t commandCaret = 0;
    size_t streamTargetCaret = 0;

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

    GuiLogBuffer logBuffer;
    std::streambuf *oldCout = nullptr;
    std::streambuf *oldCerr = nullptr;
};

constexpr int FIELD_ID = 1;
constexpr int FIELD_COMMAND = 2;
constexpr int FIELD_STREAM_TARGET = 3;
constexpr int TAB_CONNECTION = 0;
constexpr int TAB_LOGS = 1;
constexpr int TAB_VOICE = 2;
constexpr int TAB_SCREEN = 3;
constexpr const char *VIEWER_WINDOW_CLASS = "NekoChatStreamViewer";

bool PointInRect(const POINT &pt, float x, float y, float w, float h)
{
    return pt.x >= x && pt.x <= x + w && pt.y >= y && pt.y <= y + h;
}

void DrawFilledRect(float x, float y, float w, float h, const Color &color)
{
    glColor4f(color.r, color.g, color.b, color.a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void DrawRect(float x, float y, float w, float h, const Color &color,
              float lineWidth = 1.0f)
{
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

void DrawText(const AppState &app, float x, float y, const std::string &text,
              const Color &color)
{
    if (text.empty() || app.fontBase == 0)
    {
        return;
    }

    glColor4f(color.r, color.g, color.b, color.a);
    glRasterPos2f(x, y + 14.0f);
    glListBase(app.fontBase - 32);
    glCallLists(static_cast<GLsizei>(text.size()), GL_UNSIGNED_BYTE,
                text.c_str());
}

void UpdateTextureFromPreview(GLuint &texture,
                              const ScreenShareEngine::PreviewFrame &frame)
{
    if (frame.bgra.empty() || frame.width <= 0 || frame.height <= 0)
    {
        return;
    }

    if (texture == 0)
    {
        glGenTextures(1, &texture);
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    std::vector<uint8_t> rgba(frame.bgra.size());
    for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4)
    {
        rgba[i + 0] = frame.bgra[i + 2];
        rgba[i + 1] = frame.bgra[i + 1];
        rgba[i + 2] = frame.bgra[i + 0];
        rgba[i + 3] = frame.bgra[i + 3];
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void DrawTextureRect(GLuint texture, float x, float y, float w, float h)
{
    if (texture == 0)
    {
        return;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(x + w, y);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(x, y + h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

void DrawPreviewPane(const AppState &app, float x, float y, float w, float h,
                     GLuint texture, int frameW, int frameH,
                     const std::string &fallbackText)
{
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.12f, 1.0f});
    DrawRect(x, y, w, h, Color{0.24f, 0.31f, 0.41f, 1.0f});

    if (texture == 0 || frameW <= 0 || frameH <= 0)
    {
        DrawText(app, x + 14.0f, y + 14.0f, fallbackText,
                 Color{0.71f, 0.78f, 0.86f, 1.0f});
        return;
    }

    const float maxW = w - 20.0f;
    const float maxH = h - 20.0f;
    const float aspect =
        static_cast<float>(frameW) / static_cast<float>(frameH);
    float drawW = maxW;
    float drawH = drawW / aspect;
    if (drawH > maxH)
    {
        drawH = maxH;
        drawW = drawH * aspect;
    }

    const float drawX = x + (w - drawW) * 0.5f;
    const float drawY = y + (h - drawH) * 0.5f;
    DrawTextureRect(texture, drawX, drawY, drawW, drawH);
}

float MeasureTextWidth(const AppState &app, const std::string &text)
{
    if (text.empty() || !app.hdc)
    {
        return 0.0f;
    }

    SIZE textSize{};
    if (!GetTextExtentPoint32A(app.hdc, text.c_str(),
                               static_cast<int>(text.size()), &textSize))
    {
        return static_cast<float>(text.size()) * 8.0f;
    }
    return static_cast<float>(textSize.cx);
}

size_t *GetCaretForField(AppState &app, int fieldId)
{
    if (fieldId == FIELD_ID)
    {
        return &app.idCaret;
    }
    if (fieldId == FIELD_COMMAND)
    {
        return &app.commandCaret;
    }
    if (fieldId == FIELD_STREAM_TARGET)
    {
        return &app.streamTargetCaret;
    }
    return nullptr;
}

bool GetActiveFieldData(AppState &app, std::string *&value, size_t *&caret)
{
    value = nullptr;
    caret = nullptr;
    if (app.activeField == FIELD_ID)
    {
        value = &app.idInput;
        caret = &app.idCaret;
        return true;
    }
    if (app.activeField == FIELD_COMMAND)
    {
        value = &app.commandInput;
        caret = &app.commandCaret;
        return true;
    }
    if (app.activeField == FIELD_STREAM_TARGET)
    {
        value = &app.streamTargetInput;
        caret = &app.streamTargetCaret;
        return true;
    }
    return false;
}

void ClampCaret(std::string &value, size_t &caret)
{
    if (caret > value.size())
    {
        caret = value.size();
    }
}

float MeasureTextSlice(const AppState &app, const std::string &text,
                       size_t from, size_t to)
{
    if (from >= to || from >= text.size())
    {
        return 0.0f;
    }
    to = std::min(to, text.size());
    return MeasureTextWidth(app, text.substr(from, to - from));
}

struct TextVisibleRange
{
    size_t start = 0;
    size_t end = 0;
};

TextVisibleRange ComputeVisibleRange(const AppState &app,
                                     const std::string &value, size_t caret,
                                     float maxWidth)
{
    TextVisibleRange range{};
    range.start = 0;
    range.end = value.size();
    if (value.empty() || maxWidth <= 0.0f)
    {
        return range;
    }

    caret = std::min(caret, value.size());
    while (range.start < caret &&
           MeasureTextSlice(app, value, range.start, caret) > maxWidth)
    {
        ++range.start;
    }

    while (range.end > caret &&
           MeasureTextSlice(app, value, range.start, range.end) > maxWidth)
    {
        --range.end;
    }

    while (MeasureTextSlice(app, value, range.start, range.end) > maxWidth &&
           range.start < range.end)
    {
        if (range.start < caret)
        {
            ++range.start;
        }
        else
        {
            --range.end;
        }
    }

    return range;
}

size_t FindCaretByPixel(const AppState &app, const std::string &value,
                        const TextVisibleRange &range, float localX)
{
    if (value.empty())
    {
        return 0;
    }

    localX = std::max(0.0f, localX);
    size_t bestIndex = range.start;
    float bestDistance = 1e9f;
    for (size_t i = range.start; i <= range.end; ++i)
    {
        const float width = MeasureTextSlice(app, value, range.start, i);
        const float distance = std::abs(width - localX);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    return bestIndex;
}

bool Button(const AppState &app, float x, float y, float w, float h,
            const std::string &label)
{
    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    DrawFilledRect(x, y, w, h,
                   hot ? Color{0.26f, 0.34f, 0.46f, 1.0f}
                       : Color{0.19f, 0.23f, 0.30f, 1.0f});
    DrawRect(x, y, w, h, Color{0.45f, 0.55f, 0.70f, 1.0f}, 1.5f);
    DrawText(app, x + 12.0f, y + 10.0f, label,
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    return hot && app.mouseClicked;
}

bool TabButton(const AppState &app, float x, float y, float w, float h,
               const std::string &label, bool active)
{
    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    const Color fill = active ? Color{0.20f, 0.29f, 0.41f, 1.0f}
                              : (hot ? Color{0.15f, 0.21f, 0.29f, 1.0f}
                                     : Color{0.11f, 0.15f, 0.21f, 1.0f});
    DrawFilledRect(x, y, w, h, fill);
    DrawRect(x, y, w, h,
             active ? Color{0.58f, 0.74f, 0.96f, 1.0f}
                    : Color{0.29f, 0.37f, 0.47f, 1.0f},
             active ? 2.0f : 1.0f);
    DrawText(app, x + 12.0f, y + 8.0f, label,
             active ? Color{0.95f, 0.97f, 1.0f, 1.0f}
                    : Color{0.78f, 0.85f, 0.93f, 1.0f});
    return hot && app.mouseClicked;
}

bool Slider(AppState &app, float x, float y, float w, float h, float minValue,
            float maxValue, float &value)
{
    if (maxValue <= minValue || w <= 0.0f || h <= 0.0f)
    {
        return false;
    }

    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    DrawFilledRect(x, y, w, h, Color{0.10f, 0.14f, 0.20f, 1.0f});
    DrawRect(x, y, w, h, Color{0.30f, 0.38f, 0.48f, 1.0f});

    float ratio =
        std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
    DrawFilledRect(x, y, w * ratio, h, Color{0.39f, 0.62f, 0.92f, 1.0f});

    const float knobX = x + w * ratio;
    DrawFilledRect(knobX - 3.0f, y - 3.0f, 6.0f, h + 6.0f,
                   hot ? Color{0.90f, 0.95f, 1.0f, 1.0f}
                       : Color{0.80f, 0.88f, 0.97f, 1.0f});

    if (hot && (app.mouseClicked || app.mouseDown))
    {
        const float clickRatio = std::clamp(
            (static_cast<float>(app.mousePos.x) - x) / w, 0.0f, 1.0f);
        value = minValue + (maxValue - minValue) * clickRatio;
        return true;
    }
    return false;
}

bool TextBox(AppState &app, int fieldId, float x, float y, float w, float h,
             std::string &value, const std::string &placeholder)
{
    const bool hot = PointInRect(app.mousePos, x, y, w, h);
    const bool active = (app.activeField == fieldId);
    size_t *caret = GetCaretForField(app, fieldId);
    if (caret)
    {
        ClampCaret(value, *caret);
    }

    constexpr float textPaddingX = 12.0f;
    const float maxTextWidth = std::max(1.0f, w - textPaddingX * 2.0f - 2.0f);
    const size_t caretValue = caret ? *caret : value.size();
    TextVisibleRange range =
        ComputeVisibleRange(app, value, caretValue, maxTextWidth);

    if (hot && app.mouseClicked)
    {
        app.activeField = fieldId;
        if (caret)
        {
            const float localX =
                static_cast<float>(app.mousePos.x) - (x + textPaddingX);
            *caret = FindCaretByPixel(app, value, range, localX);
        }
        return true;
    }

    DrawFilledRect(x, y, w, h,
                   active ? Color{0.12f, 0.16f, 0.21f, 1.0f}
                          : Color{0.09f, 0.12f, 0.16f, 1.0f});
    DrawRect(x, y, w, h,
             active ? Color{0.52f, 0.70f, 0.94f, 1.0f}
                    : Color{0.28f, 0.34f, 0.42f, 1.0f},
             active ? 2.0f : 1.0f);

    std::string display;
    Color textColor{};
    if (value.empty() && !active)
    {
        display = placeholder;
        while (!display.empty() &&
               MeasureTextWidth(app, display) > maxTextWidth)
        {
            display.pop_back();
        }
        textColor = Color{0.50f, 0.56f, 0.64f, 1.0f};
    }
    else
    {
        display = value.substr(range.start, range.end - range.start);
        textColor = Color{0.93f, 0.96f, 0.99f, 1.0f};
    }
    DrawText(app, x + textPaddingX, y + 10.0f, display, textColor);

    if (active && caret && (GetTickCount64() / 500ULL) % 2ULL == 0ULL)
    {
        const float caretOffset =
            MeasureTextSlice(app, value, range.start, *caret);
        float caretX = x + textPaddingX + caretOffset;
        caretX = std::min(caretX, x + w - 8.0f);
        DrawFilledRect(caretX, y + 8.0f, 2.0f, h - 16.0f,
                       Color{0.88f, 0.92f, 0.96f, 1.0f});
    }

    return false;
}

void InsertAsciiAtCaret(std::string &value, size_t &caret, WPARAM ch)
{
    if (ch >= 32 && ch <= 126)
    {
        ClampCaret(value, caret);
        value.insert(value.begin() + static_cast<std::ptrdiff_t>(caret),
                     static_cast<char>(ch));
        ++caret;
    }
}

void BackspaceAtCaret(std::string &value, size_t &caret)
{
    ClampCaret(value, caret);
    if (caret > 0 && !value.empty())
    {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(caret - 1));
        --caret;
    }
}

void DeleteAtCaret(std::string &value, size_t &caret)
{
    ClampCaret(value, caret);
    if (caret < value.size())
    {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(caret));
    }
}

void SetupProjection(const AppState &app)
{
    glViewport(0, 0, app.width, app.height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(app.width),
            static_cast<double>(app.height), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void ConnectClient(AppState &app)
{
    if (app.connected || app.idInput.empty())
    {
        return;
    }

    app.client = std::make_unique<P2PClient>();
    app.client->id = app.idInput;
    app.client->Start("SERVER IP!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", 27015);
    app.client->PrintHelp();
    app.connected = true;
    app.activeField = FIELD_COMMAND;
    app.commandCaret = app.commandInput.size();
    app.activeTab = TAB_CONNECTION;
    app.streamTargetCaret = app.streamTargetInput.size();
}

void SendCommand(AppState &app)
{
    if (!app.connected || !app.client || app.commandInput.empty())
    {
        return;
    }

    app.client->ExecuteCommand(app.commandInput);
    app.commandInput.clear();
    app.commandCaret = 0;
}

void DrawHelpPanel(const AppState &app, float x, float y, float w, float h)
{
    DrawFilledRect(x, y, w, h, Color{0.08f, 0.11f, 0.15f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});

    DrawText(app, x + 18.0f, y + 16.0f, "Quick Actions",
             Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 54.0f,
             "Type commands below:", Color{0.68f, 0.75f, 0.84f, 1.0f});
    DrawText(app, x + 18.0f, y + 78.0f, "offer alice",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 100.0f, "msg alice hello",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 122.0f, "voice on alice",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 144.0f, "reconnect alice",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 166.0f, "volume alice 70",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 188.0f, "register neko",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 210.0f, "stream on alice",
             Color{0.92f, 0.95f, 0.99f, 1.0f});
}

void DrawLinePanel(const AppState &app, float x, float y, float w, float h,
                   const std::string &title,
                   const std::vector<std::string> &lines)
{
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, title,
             Color{0.94f, 0.96f, 0.99f, 1.0f});

    constexpr float lineHeight = 18.0f;
    const int visibleLines =
        std::max(1, static_cast<int>((h - 40.0f) / lineHeight));
    const int startIndex =
        std::max(0, static_cast<int>(lines.size()) - visibleLines);
    float lineY = y + 42.0f;
    for (int i = startIndex; i < static_cast<int>(lines.size()); ++i)
    {
        DrawText(app, x + 12.0f, lineY, lines[static_cast<size_t>(i)],
                 Color{0.80f, 0.86f, 0.93f, 1.0f});
        lineY += lineHeight;
    }
}

std::string VolumeLabel(float volume)
{
    const int percent = static_cast<int>(volume * 100.0f + 0.5f);
    return std::to_string(percent) + "%";
}

void ResetRemotePreview(AppState &app)
{
    app.remoteScreenSeq = 0;
    app.remoteScreenW = 0;
    app.remoteScreenH = 0;
    app.remoteScreenBgra.clear();
    if (app.remoteScreenTexture != 0)
    {
        glDeleteTextures(1, &app.remoteScreenTexture);
        app.remoteScreenTexture = 0;
    }
}

void UpdateScreenPreviewTextures(AppState &app)
{
    if (!app.client)
    {
        return;
    }

    ScreenShareEngine::PreviewFrame localPreview;
    if (app.client->GetLocalScreenPreview(localPreview) &&
        localPreview.sequence != app.localScreenSeq)
    {
        UpdateTextureFromPreview(app.localScreenTexture, localPreview);
        app.localScreenSeq = localPreview.sequence;
        app.localScreenW = localPreview.width;
        app.localScreenH = localPreview.height;
    }

    if (!app.selectedRemotePeer.empty())
    {
        ScreenShareEngine::PreviewFrame remotePreview;
        if (app.client->GetRemoteScreenPreviewByPeer(app.selectedRemotePeer,
                                                     remotePreview) &&
            remotePreview.sequence != app.remoteScreenSeq)
        {
            UpdateTextureFromPreview(app.remoteScreenTexture, remotePreview);
            app.remoteScreenSeq = remotePreview.sequence;
            app.remoteScreenW = remotePreview.width;
            app.remoteScreenH = remotePreview.height;
            app.remoteScreenBgra = remotePreview.bgra;
        }
    }
}

void PaintViewerContent(AppState &app, HDC hdc, const RECT &clientRect)
{
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

    if (app.remoteScreenBgra.empty() || app.remoteScreenW <= 0 ||
        app.remoteScreenH <= 0)
    {
        std::string label =
            app.selectedRemotePeer.empty()
                ? "No remote stream selected."
                : ("Waiting for stream from " + app.selectedRemotePeer + "...");
        TextOutA(backDc, 16, 16, label.c_str(), static_cast<int>(label.size()));
    }
    else
    {
        const float srcAspect = static_cast<float>(app.remoteScreenW) /
                                static_cast<float>(app.remoteScreenH);
        float dstW = static_cast<float>(panelW);
        float dstH = dstW / srcAspect;
        if (dstH > static_cast<float>(panelH))
        {
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
        StretchDIBits(backDc, drawX, drawY, drawW, drawH, 0, 0,
                      app.remoteScreenW, app.remoteScreenH,
                      app.remoteScreenBgra.data(), &bmi, DIB_RGB_COLORS,
                      SRCCOPY);

        const std::string streamLabel =
            app.selectedRemotePeer.empty()
                ? "Viewing stream: auto"
                : ("Viewing stream: " + app.selectedRemotePeer);
        TextOutA(backDc, 16, 16, streamLabel.c_str(),
                 static_cast<int>(streamLabel.size()));
    }

    BitBlt(hdc, 0, 0, panelW, panelH, backDc, 0, 0, SRCCOPY);
    SelectObject(backDc, oldBmp);
    DeleteObject(backBmp);
    DeleteDC(backDc);
}

void RefreshViewerWindow(AppState &app)
{
    if (!app.viewerHwnd)
    {
        return;
    }
    if (!IsWindow(app.viewerHwnd))
    {
        app.viewerHwnd = nullptr;
        return;
    }
    InvalidateRect(app.viewerHwnd, nullptr, FALSE);
}

void OpenViewerWindow(AppState &app)
{
    if (app.viewerHwnd && IsWindow(app.viewerHwnd))
    {
        ShowWindow(app.viewerHwnd, SW_SHOW);
        SetForegroundWindow(app.viewerHwnd);
        return;
    }

    app.viewerHwnd = CreateWindowExA(
        0, VIEWER_WINDOW_CLASS, "NekoChat Stream Viewer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 960,
        540, app.hwnd, nullptr, GetModuleHandle(nullptr), &app);
    if (app.viewerHwnd)
    {
        ShowWindow(app.viewerHwnd, SW_SHOW);
        InvalidateRect(app.viewerHwnd, nullptr, FALSE);
    }
}

void CloseViewerWindow(AppState &app)
{
    if (app.viewerHwnd && IsWindow(app.viewerHwnd))
    {
        DestroyWindow(app.viewerHwnd);
    }
    app.viewerHwnd = nullptr;
}

void DrawVoicePanel(AppState &app, float x, float y, float w, float h)
{
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, "Voice Mix",
             Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 36.0f, "Adjust playback level per participant",
             Color{0.68f, 0.75f, 0.84f, 1.0f});

    if (!app.client)
    {
        DrawText(app, x + 18.0f, y + 64.0f, "Client is not ready.",
                 Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float settingsX = x + 10.0f;
    const float settingsY = y + 58.0f;
    const float settingsW = w - 20.0f;
    const float settingsH = 106.0f;
    DrawFilledRect(settingsX, settingsY, settingsW, settingsH,
                   Color{0.09f, 0.12f, 0.17f, 1.0f});
    DrawRect(settingsX, settingsY, settingsW, settingsH,
             Color{0.23f, 0.31f, 0.41f, 1.0f});
    DrawText(app, settingsX + 12.0f, settingsY + 12.0f, "Echo Suppression",
             Color{0.90f, 0.95f, 0.99f, 1.0f});

    bool echoEnabled = app.client->IsEchoSuppressionEnabled();
    if (Button(app, settingsX + 180.0f, settingsY + 6.0f, 112.0f, 28.0f,
               echoEnabled ? "AEC: ON" : "AEC: OFF"))
    {
        app.client->SetEchoSuppressionEnabled(!echoEnabled);
        echoEnabled = !echoEnabled;
    }

    const float sliderX = settingsX + 154.0f;
    const float sliderW = std::max(120.0f, settingsW - 220.0f);
    const float valueX = sliderX + sliderW + 8.0f;

    float detectPercent = app.client->GetEchoCorrelationThreshold() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 34.0f, "Detect",
             Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 42.0f, sliderW, 8.0f, 30.0f, 95.0f,
               detectPercent))
    {
        app.client->SetEchoCorrelationThreshold(detectPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 34.0f,
             std::to_string(static_cast<int>(detectPercent + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    float subtractPercent = app.client->GetEchoSubtractionMaxGain() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 56.0f, "Subtract",
             Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 64.0f, sliderW, 8.0f, 50.0f, 200.0f,
               subtractPercent))
    {
        app.client->SetEchoSubtractionMaxGain(subtractPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 56.0f,
             std::to_string(static_cast<int>(subtractPercent + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    float residualPercent = app.client->GetEchoResidualAttenuation() * 100.0f;
    DrawText(app, sliderX - 126.0f, settingsY + 78.0f, "Residual",
             Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, settingsY + 86.0f, sliderW, 8.0f, 10.0f, 100.0f,
               residualPercent))
    {
        app.client->SetEchoResidualAttenuation(residualPercent / 100.0f);
    }
    DrawText(app, valueX, settingsY + 78.0f,
             std::to_string(static_cast<int>(residualPercent + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    auto peers = app.client->GetPeerIds();
    std::sort(peers.begin(), peers.end());
    if (peers.empty())
    {
        DrawText(app, x + 18.0f, settingsY + settingsH + 22.0f,
                 "No participants yet.", Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float rowHeight = 52.0f;
    float rowY = settingsY + settingsH + 12.0f;
    size_t shown = 0;

    for (const auto &peerId : peers)
    {
        if (rowY + rowHeight > y + h - 10.0f)
        {
            break;
        }

        DrawFilledRect(x + 10.0f, rowY, w - 20.0f, rowHeight - 6.0f,
                       Color{0.09f, 0.12f, 0.17f, 1.0f});
        DrawRect(x + 10.0f, rowY, w - 20.0f, rowHeight - 6.0f,
                 Color{0.23f, 0.31f, 0.41f, 1.0f});
        DrawText(app, x + 20.0f, rowY + 14.0f, peerId,
                 Color{0.92f, 0.95f, 0.99f, 1.0f});

        const float buttonW = 56.0f;
        const float buttonGap = 8.0f;
        const float buttonY = rowY + 9.0f;
        const float rightPad = 14.0f;
        const float fullBtnX = x + w - rightPad - buttonW;
        const float muteBtnX = fullBtnX - buttonGap - buttonW;

        const float sliderX = x + 170.0f;
        const float sliderY = rowY + 19.0f;
        const float sliderH = 10.0f;
        const float percentTextW = 44.0f;
        const float sliderW =
            std::max(120.0f, muteBtnX - sliderX - percentTextW - 8.0f);

        float volume = app.client->GetPeerVolume(peerId);
        float adjusted = volume;
        if (Slider(app, sliderX, sliderY, sliderW, sliderH, 0.0f, 2.0f,
                   adjusted))
        {
            app.client->SetPeerVolume(peerId, adjusted);
            volume = adjusted;
        }

        DrawText(app, sliderX + sliderW + 10.0f, rowY + 12.0f,
                 VolumeLabel(volume), Color{0.85f, 0.91f, 0.98f, 1.0f});

        if (Button(app, muteBtnX, buttonY, buttonW, 28.0f, "Mute"))
        {
            app.client->SetPeerVolume(peerId, 0.0f);
        }
        if (Button(app, fullBtnX, buttonY, buttonW, 28.0f, "100%"))
        {
            app.client->SetPeerVolume(peerId, 1.0f);
        }

        rowY += rowHeight;
        ++shown;
    }

    if (shown < peers.size())
    {
        DrawText(app, x + 18.0f, y + h - 24.0f,
                 "+" + std::to_string(peers.size() - shown) + " more peers...",
                 Color{0.66f, 0.73f, 0.81f, 1.0f});
    }
}

void DrawScreenPanel(AppState &app, float x, float y, float w, float h)
{
    DrawFilledRect(x, y, w, h, Color{0.06f, 0.08f, 0.11f, 1.0f});
    DrawRect(x, y, w, h, Color{0.22f, 0.28f, 0.36f, 1.0f});
    DrawText(app, x + 18.0f, y + 16.0f, "Screen Stream",
             Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, x + 18.0f, y + 36.0f,
             "Native Windows capture + UDP streaming",
             Color{0.68f, 0.75f, 0.84f, 1.0f});

    if (!app.client)
    {
        DrawText(app, x + 18.0f, y + 64.0f, "Client is not ready.",
                 Color{0.76f, 0.82f, 0.90f, 1.0f});
        return;
    }

    const float controlsY = y + 54.0f;
    DrawText(app, x + 12.0f, controlsY + 6.0f,
             "Target:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    TextBox(app, FIELD_STREAM_TARGET, x + 72.0f, controlsY, 200.0f, 34.0f,
            app.streamTargetInput, "alice / *");

    const float startBtnX = x + 286.0f;
    const float startBtnW = 112.0f;
    const float broadcastBtnX = startBtnX + startBtnW + 8.0f;
    const float broadcastBtnW = 128.0f;
    const float viewerBtnX = broadcastBtnX + broadcastBtnW + 8.0f;
    const float viewerBtnW = 122.0f;

    const bool streaming = app.client->IsScreenShareRunning();
    if (Button(app, startBtnX, controlsY, startBtnW, 34.0f,
               streaming ? "Stop Stream" : "Start Stream"))
    {
        if (streaming)
        {
            app.client->StopScreenShare();
        }
        else
        {
            const std::string target =
                app.streamTargetInput.empty() ? "*" : app.streamTargetInput;
            app.client->StartScreenShare(target);
        }
    }

    if (Button(app, broadcastBtnX, controlsY, broadcastBtnW, 34.0f,
               "Broadcast (*)"))
    {
        app.streamTargetInput = "*";
        app.streamTargetCaret = app.streamTargetInput.size();
        app.client->StartScreenShare("*");
    }

    bool viewerOpen = app.viewerHwnd && IsWindow(app.viewerHwnd);
    if (!viewerOpen)
    {
        app.viewerHwnd = nullptr;
    }
    if (Button(app, viewerBtnX, controlsY, viewerBtnW, 34.0f,
               viewerOpen ? "Close Viewer" : "Open Viewer"))
    {
        if (viewerOpen)
        {
            CloseViewerWindow(app);
        }
        else
        {
            OpenViewerWindow(app);
        }
        viewerOpen = app.viewerHwnd && IsWindow(app.viewerHwnd);
    }

    const std::string activeTarget = app.client->GetScreenShareTarget();
    DrawText(app, x + 12.0f, controlsY + 40.0f,
             "Tx: " + activeTarget + (streaming ? " (LIVE)" : " (idle)"),
             streaming ? Color{0.53f, 0.89f, 0.57f, 1.0f}
                       : Color{0.75f, 0.79f, 0.85f, 1.0f});

    auto streamIds = app.client->GetRemoteScreenStreamIds();
    std::sort(streamIds.begin(), streamIds.end());

    if (!app.selectedRemotePeer.empty())
    {
        auto it = std::find(streamIds.begin(), streamIds.end(),
                            app.selectedRemotePeer);
        if (it == streamIds.end())
        {
            app.selectedRemotePeer.clear();
            app.remoteScreenPeer.clear();
            ResetRemotePreview(app);
        }
    }
    if (app.selectedRemotePeer.empty() && !streamIds.empty())
    {
        app.selectedRemotePeer = streamIds.front();
    }
    if (app.remoteScreenPeer != app.selectedRemotePeer)
    {
        app.remoteScreenPeer = app.selectedRemotePeer;
        ResetRemotePreview(app);
    }

    float watchY = controlsY + 62.0f;
    DrawText(app, x + 12.0f, watchY + 4.0f,
             "Watch:", Color{0.82f, 0.89f, 0.96f, 1.0f});
    float watchBtnX = x + 72.0f;
    float watchBtnY = watchY;
    float watchRowBottom = watchY + 28.0f;
    if (streamIds.empty())
    {
        DrawText(app, x + 72.0f, watchY + 4.0f, "No incoming streams yet",
                 Color{0.61f, 0.69f, 0.78f, 1.0f});
    }
    else
    {
        for (const auto &peerId : streamIds)
        {
            const float buttonW = std::clamp(
                MeasureTextWidth(app, peerId) + 26.0f, 86.0f, 180.0f);
            if (watchBtnX + buttonW > x + w - 14.0f)
            {
                watchBtnX = x + 72.0f;
                watchBtnY += 32.0f;
            }
            if (TabButton(app, watchBtnX, watchBtnY, buttonW, 26.0f, peerId,
                          app.selectedRemotePeer == peerId))
            {
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
    DrawText(app, x + 12.0f, sliderY + 2.0f, "FPS",
             Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, sliderY + 8.0f, sliderW, 8.0f, 1.0f, 60.0f,
               fpsValue))
    {
        app.client->SetScreenShareFps(static_cast<int>(fpsValue + 0.5f));
    }
    DrawText(app, sliderX + sliderW + 10.0f, sliderY + 2.0f,
             std::to_string(static_cast<int>(fpsValue + 0.5f)),
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    float qualityValue =
        static_cast<float>(app.client->GetScreenShareQuality());
    DrawText(app, x + 12.0f, sliderY + 24.0f, "Quality",
             Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, sliderY + 30.0f, sliderW, 8.0f, 20.0f, 95.0f,
               qualityValue))
    {
        app.client->SetScreenShareQuality(
            static_cast<int>(qualityValue + 0.5f));
    }
    DrawText(app, sliderX + sliderW + 10.0f, sliderY + 24.0f,
             std::to_string(static_cast<int>(qualityValue + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    float scaleValue =
        static_cast<float>(app.client->GetScreenShareScalePercent());
    DrawText(app, x + 12.0f, sliderY + 46.0f, "Scale",
             Color{0.78f, 0.85f, 0.93f, 1.0f});
    if (Slider(app, sliderX, sliderY + 52.0f, sliderW, 8.0f, 20.0f, 100.0f,
               scaleValue))
    {
        app.client->SetScreenShareScalePercent(
            static_cast<int>(scaleValue + 0.5f));
    }
    DrawText(app, sliderX + sliderW + 10.0f, sliderY + 46.0f,
             std::to_string(static_cast<int>(scaleValue + 0.5f)) + "%",
             Color{0.84f, 0.90f, 0.97f, 1.0f});

    UpdateScreenPreviewTextures(app);

    const float previewY = sliderY + 78.0f;
    const float previewH = h - (previewY - y) - 10.0f;
    const float previewW = (w - 30.0f) * 0.5f;

    DrawText(app, x + 12.0f, previewY - 22.0f, "Local Capture Preview",
             Color{0.86f, 0.92f, 0.99f, 1.0f});
    DrawPreviewPane(app, x + 10.0f, previewY, previewW, previewH,
                    app.localScreenTexture, app.localScreenW, app.localScreenH,
                    "No local frame yet");

    DrawText(app, x + 20.0f + previewW, previewY - 22.0f,
             app.selectedRemotePeer.empty()
                 ? "Incoming Stream Preview"
                 : ("Incoming: " + app.selectedRemotePeer),
             Color{0.86f, 0.92f, 0.99f, 1.0f});
    DrawPreviewPane(
        app, x + 20.0f + previewW, previewY, previewW, previewH,
        app.remoteScreenTexture, app.remoteScreenW, app.remoteScreenH,
        app.selectedRemotePeer.empty() ? "No remote stream yet"
                                       : "Waiting for selected stream");
}

void RenderConnectScreen(AppState &app)
{
    DrawText(app, 120.0f, 120.0f, "NekoChat OpenGL Client",
             Color{0.94f, 0.96f, 0.99f, 1.0f});
    DrawText(app, 120.0f, 150.0f, "Pure OpenGL application for the P2P client.",
             Color{0.68f, 0.75f, 0.84f, 1.0f});

    const float panelX = 120.0f;
    const float panelY = 220.0f;
    const float panelW = 480.0f;
    const float panelH = 170.0f;

    DrawFilledRect(panelX, panelY, panelW, panelH,
                   Color{0.08f, 0.11f, 0.15f, 1.0f});
    DrawRect(panelX, panelY, panelW, panelH, Color{0.22f, 0.28f, 0.36f, 1.0f});

    DrawText(app, panelX + 20.0f, panelY + 20.0f, "Enter your ID",
             Color{0.90f, 0.94f, 0.99f, 1.0f});
    TextBox(app, FIELD_ID, panelX + 20.0f, panelY + 54.0f, panelW - 40.0f,
            46.0f, app.idInput, "alice");

    if (Button(app, panelX + 20.0f, panelY + 118.0f, 180.0f, 34.0f, "Connect"))
    {
        ConnectClient(app);
    }
}

void RenderConnectedScreen(AppState &app)
{
    DrawText(app, 24.0f, 20.0f, "NekoChat OpenGL",
             Color{0.96f, 0.97f, 0.99f, 1.0f});
    DrawText(app, 24.0f, 44.0f, "ID: " + app.idInput,
             Color{0.68f, 0.75f, 0.84f, 1.0f});

    const float leftX = 24.0f;
    const float leftY = 84.0f;
    const float leftW = 260.0f;
    const float leftH = static_cast<float>(app.height) - 108.0f;
    DrawHelpPanel(app, leftX, leftY, leftW, leftH);

    float buttonY = leftY + 236.0f;
    if (Button(app, leftX + 18.0f, buttonY, leftW - 36.0f, 34.0f, "List Users"))
    {
        app.client->ExecuteCommand("list");
    }
    buttonY += 46.0f;
    if (Button(app, leftX + 18.0f, buttonY, leftW - 36.0f, 34.0f, "Voice Off"))
    {
        app.client->ExecuteCommand("voice off");
    }
    buttonY += 46.0f;
    if (Button(app, leftX + 18.0f, buttonY, leftW - 36.0f, 34.0f,
               "Broadcast Voice"))
    {
        app.client->ExecuteCommand("brdvoice on");
    }

    const float logX = leftX + leftW + 20.0f;
    const float logY = 24.0f;
    const float logW = static_cast<float>(app.width) - logX - 24.0f;
    const float logH = static_cast<float>(app.height) - 136.0f;

    const float tabY = logY;
    const float tabH = 34.0f;
    const float tabW = 130.0f;
    const float tabGap = 10.0f;

    if (TabButton(app, logX, tabY, tabW, tabH, "Connection",
                  app.activeTab == TAB_CONNECTION))
    {
        app.activeTab = TAB_CONNECTION;
    }
    if (TabButton(app, logX + tabW + tabGap, tabY, tabW, tabH, "Logs",
                  app.activeTab == TAB_LOGS))
    {
        app.activeTab = TAB_LOGS;
    }
    if (TabButton(app, logX + (tabW + tabGap) * 2.0f, tabY, tabW, tabH, "Voice",
                  app.activeTab == TAB_VOICE))
    {
        app.activeTab = TAB_VOICE;
    }
    if (TabButton(app, logX + (tabW + tabGap) * 3.0f, tabY, tabW, tabH,
                  "Screen", app.activeTab == TAB_SCREEN))
    {
        app.activeTab = TAB_SCREEN;
    }

    const float panelY = tabY + tabH + 10.0f;
    const float panelH = logH - tabH - 10.0f;
    if (app.activeTab == TAB_CONNECTION)
    {
        DrawLinePanel(app, logX, panelY, logW, panelH, "Connection Events",
                      app.logBuffer.SnapshotConnection());
    }
    else if (app.activeTab == TAB_LOGS)
    {
        DrawLinePanel(app, logX, panelY, logW, panelH, "All Logs",
                      app.logBuffer.SnapshotAll());
    }
    else if (app.activeTab == TAB_VOICE)
    {
        DrawVoicePanel(app, logX, panelY, logW, panelH);
    }
    else
    {
        DrawScreenPanel(app, logX, panelY, logW, panelH);
    }

    const float inputY = static_cast<float>(app.height) - 88.0f;
    TextBox(app, FIELD_COMMAND, logX, inputY, logW - 132.0f, 44.0f,
            app.commandInput, "stream on alice / register neko / offer alice");
    if (Button(app, logX + logW - 108.0f, inputY, 108.0f, 44.0f, "Send"))
    {
        SendCommand(app);
    }
}

void Render(AppState &app)
{
    SetupProjection(app);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    DrawFilledRect(0.0f, 0.0f, static_cast<float>(app.width),
                   static_cast<float>(app.height),
                   Color{0.05f, 0.06f, 0.08f, 1.0f});

    if (app.connected)
    {
        UpdateScreenPreviewTextures(app);
        RenderConnectedScreen(app);
    }
    else
    {
        RenderConnectScreen(app);
    }

    RefreshViewerWindow(app);
    app.mouseClicked = false;
    SwapBuffers(app.hdc);
}

bool InitOpenGL(AppState &app)
{
    app.hdc = GetDC(app.hwnd);
    if (!app.hdc)
    {
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
    if (pixelFormat == 0 || !SetPixelFormat(app.hdc, pixelFormat, &pfd))
    {
        return false;
    }

    app.glrc = wglCreateContext(app.hdc);
    if (!app.glrc || !wglMakeCurrent(app.hdc, app.glrc))
    {
        return false;
    }

    app.font =
        CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    FF_DONTCARE, "Consolas");
    SelectObject(app.hdc, app.font);

    app.fontBase = glGenLists(96);
    if (!wglUseFontBitmapsA(app.hdc, 32, 96, app.fontBase))
    {
        return false;
    }

    return true;
}

void Shutdown(AppState &app)
{
    CloseViewerWindow(app);

    if (app.client)
    {
        app.client->Stop();
        app.client.reset();
    }

    std::cout.flush();
    std::cerr.flush();

    if (app.oldCout)
    {
        std::cout.rdbuf(app.oldCout);
    }
    if (app.oldCerr)
    {
        std::cerr.rdbuf(app.oldCerr);
    }

    if (app.fontBase != 0)
    {
        glDeleteLists(app.fontBase, 96);
        app.fontBase = 0;
    }
    if (app.localScreenTexture != 0)
    {
        glDeleteTextures(1, &app.localScreenTexture);
        app.localScreenTexture = 0;
    }
    if (app.remoteScreenTexture != 0)
    {
        glDeleteTextures(1, &app.remoteScreenTexture);
        app.remoteScreenTexture = 0;
    }
    if (app.font)
    {
        DeleteObject(app.font);
        app.font = nullptr;
    }
    if (app.glrc)
    {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(app.glrc);
        app.glrc = nullptr;
    }
    if (app.hdc && app.hwnd)
    {
        ReleaseDC(app.hwnd, app.hdc);
        app.hdc = nullptr;
    }
}

LRESULT CALLBACK ViewerWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam)
{
    auto *app =
        reinterpret_cast<AppState *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto *create = reinterpret_cast<CREATESTRUCT *>(lParam);
        auto *state = reinterpret_cast<AppState *>(create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(state));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (app)
        {
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
        if (app && app->viewerHwnd == hwnd)
        {
            app->viewerHwnd = nullptr;
        }
        return 0;
    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto *app =
        reinterpret_cast<AppState *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto *create = reinterpret_cast<CREATESTRUCT *>(lParam);
        auto *state = reinterpret_cast<AppState *>(create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(state));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_SIZE:
        if (app)
        {
            app->width = LOWORD(lParam);
            app->height = HIWORD(lParam);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app)
        {
            app->mousePos.x = GET_X_LPARAM(lParam);
            app->mousePos.y = GET_Y_LPARAM(lParam);
        }
        return 0;
    case WM_LBUTTONUP:
        if (app)
        {
            app->mousePos.x = GET_X_LPARAM(lParam);
            app->mousePos.y = GET_Y_LPARAM(lParam);
            app->mouseDown = false;
            app->mouseClicked = true;
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (app)
        {
            app->mousePos.x = GET_X_LPARAM(lParam);
            app->mousePos.y = GET_Y_LPARAM(lParam);
            app->mouseDown = true;
        }
        return 0;
    case WM_CHAR:
    {
        if (!app)
        {
            return 0;
        }
        if (wParam == '\r')
        {
            if (app->activeField == FIELD_ID && !app->connected)
            {
                ConnectClient(*app);
            }
            else if (app->activeField == FIELD_COMMAND && app->connected)
            {
                SendCommand(*app);
            }
            else if (app->activeField == FIELD_STREAM_TARGET &&
                     app->connected && app->client)
            {
                const std::string target = app->streamTargetInput.empty()
                                               ? "*"
                                               : app->streamTargetInput;
                app->client->StartScreenShare(target);
            }
            return 0;
        }
        if (wParam == '\b' || wParam == '\t')
        {
            return 0;
        }

        std::string *value = nullptr;
        size_t *caret = nullptr;
        if (GetActiveFieldData(*app, value, caret) && value && caret)
        {
            InsertAsciiAtCaret(*value, *caret, wParam);
        }
        return 0;
    }
    case WM_KEYDOWN:
    {
        if (!app)
        {
            return 0;
        }
        if (wParam == VK_TAB)
        {
            if (!app->connected)
            {
                app->activeField = FIELD_ID;
            }
            else
            {
                const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (reverse)
                {
                    app->activeField = (app->activeField == FIELD_STREAM_TARGET)
                                           ? FIELD_COMMAND
                                           : FIELD_STREAM_TARGET;
                }
                else
                {
                    app->activeField = (app->activeField == FIELD_COMMAND)
                                           ? FIELD_STREAM_TARGET
                                           : FIELD_COMMAND;
                }
            }
            return 0;
        }

        std::string *value = nullptr;
        size_t *caret = nullptr;
        if (!GetActiveFieldData(*app, value, caret) || !value || !caret)
        {
            return 0;
        }

        switch (wParam)
        {
        case VK_BACK:
            BackspaceAtCaret(*value, *caret);
            return 0;
        case VK_DELETE:
            DeleteAtCaret(*value, *caret);
            return 0;
        case VK_LEFT:
            if (*caret > 0)
            {
                --(*caret);
            }
            return 0;
        case VK_RIGHT:
            if (*caret < value->size())
            {
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

int main()
{
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
    app.activeField = FIELD_ID;
    app.idCaret = app.idInput.size();
    app.streamTargetCaret = app.streamTargetInput.size();

    app.hwnd = CreateWindowA(wc.lpszClassName, "NekoChat OpenGL",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, app.width, app.height, nullptr,
                             nullptr, instance, &app);

    if (!app.hwnd || !InitOpenGL(app))
    {
        Shutdown(app);
        return 1;
    }

    ShowWindow(app.hwnd, SW_SHOW);
    UpdateWindow(app.hwnd);

    MSG msg{};
    while (true)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
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
