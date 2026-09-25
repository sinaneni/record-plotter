// Themed controls: button, text input, checkbox, dropdown.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "d2d.h"
#include "theme.h"

namespace ui {

// Registers every window class in this header. Safe to call more than once.
void RegisterControlClasses();

// ------------------------------------------------------------------- Button
class Button {
public:
    enum class Variant { Accent, Success, Danger, Neutral };

    // Optional drawn icon shown instead of the caption.
    enum class Glyph { None, Gear };

    // `id` is reported to the parent as WM_COMMAND(id, BN_CLICKED).
    bool Create(HWND parent, int id, const std::wstring& text,
                Variant v = Variant::Accent);

    HWND hwnd() const { return hwnd_; }
    void SetText(const std::wstring& text);
    void SetVariant(Variant v);
    void SetEnabled(bool on);
    bool enabled() const;
    void SetToolTip(const std::wstring& text);
    // Square icon-button style: no wide padding, larger glyph.
    void SetIconStyle(bool on);
    // Draws `g` centred instead of the caption.
    void SetGlyph(Glyph g);
    // Colour the control clears to (default BgPanel).
    void SetBackdrop(theme::Color c);
    void Move(int x, int y, int w, int h);

private:
    HWND hwnd_ = nullptr;
};

// -------------------------------------------------------------------- Input
// Single-line text field. Numeric fields are clamped to [lo,hi] when read or settled.
class Input {
public:
    bool Create(HWND parent, int id, bool numeric = false, int lo = 0, int hi = 0);

    HWND hwnd() const { return hwnd_; }
    void Move(int x, int y, int w, int h);
    void SetEnabled(bool on);

    std::wstring Text() const;
    void         SetText(const std::wstring& s);
    // Numeric value clamped into range; does not modify the field.
    int  Value() const;
    void SetValue(int v);
    // Rewrites a numeric field to its clamped plain value.
    void Settle();

    bool numeric() const { return numeric_; }
    int  lo() const { return lo_; }
    int  hi() const { return hi_; }
    // Changes the numeric range and clamps the current text.
    void SetRange(int lo, int hi);

    // True if `hwnd` is the EDIT of an Input field.
    static bool IsInputEdit(HWND hwnd);

private:
    HWND hwnd_ = nullptr;
    bool numeric_ = false;
    int  lo_ = 0;
    int  hi_ = 0;
};

// ----------------------------------------------------------------- Checkbox
// Custom-drawn checkbox.
class Checkbox {
public:
    bool Create(HWND parent, int id, const std::wstring& label, bool checked = false);

    HWND hwnd() const { return hwnd_; }
    void Move(int x, int y, int w, int h);
    void SetEnabled(bool on);
    bool checked() const;
    void SetChecked(bool on);   // does not notify the parent

    // Replaces the caption.
    void SetLabel(const std::wstring& label);
    // Colour the control clears to (default BgPanel).
    void SetBackdrop(theme::Color c);

private:
    HWND hwnd_ = nullptr;
};

// Sent to the parent as WM_COMMAND(id, kCheckboxToggled) when the user clicks.
constexpr WORD kCheckboxToggled = 0x7A03;

// ----------------------------------------------------------------- Dropdown
class Dropdown {
public:
    // A label with an int payload.
    struct Item {
        std::wstring label;
        int          data;
    };

    bool Create(HWND parent, int id, std::vector<Item> items);

    HWND hwnd() const { return hwnd_; }
    void Move(int x, int y, int w, int h);
    void SetEnabled(bool on);

    int  SelectedData() const;
    // Selects the item with this payload; no-op if none matches.
    void SelectData(int data);

    // Colour the control clears to (default BgPanel).
    void SetBackdrop(theme::Color c);

    // Replaces the items, keeping the selection by payload.
    void SetItems(std::vector<Item> items);

    // Closes the open list without choosing anything.
    void CloseList();

private:
    HWND hwnd_ = nullptr;
};

// Notification codes sent to the parent via WM_COMMAND.
constexpr WORD kDropdownChanged = 0x7A01;
// Enter pressed in an Input.
constexpr WORD kInputAccept = 0x7A02;

// Sent to a top-level window when Ctrl+C is pressed outside a text field.
constexpr UINT WM_UI_COPY = WM_APP + 3;

// Puts UTF-16 text on the clipboard. Returns false for empty text or on failure.
bool SetClipboardText(HWND owner, const std::wstring& text);

// The caption a control draws: button text, checkbox label or selected dropdown item.
std::wstring ShownCaption(HWND hwnd);

// --------------------------------------------------------- shared paint bits
// Draws the rounded input frame, with an accent border when focused.
void DrawInputFrame(ID2D1RenderTarget* rt, d2d::Brushes& brushes,
                    const D2D1_RECT_F& r, bool focused, bool enabled);

// Height of an input or dropdown row at 96 DPI, and the frame's corner radius.
constexpr int   kFieldH96 = 28;
constexpr float kFieldRadius = 6.0f;

// ------------------------------------------------------------------ popups
// Shared metrics for the popup surfaces (context menu and dropdown list).
constexpr int   kPopupItemH96   = 28;   // one row
constexpr int   kPopupSepH96    = 9;    // a separator row
constexpr int   kPopupPadX96    = 12;   // label inset, both sides
constexpr float kPopupRadius    = 8.0f; // the panel's corners
constexpr float kPopupHotRadius = 5.0f; // the highlight's corners

// Horizontal inset of the row highlight.
constexpr float kPopupHotInsetX = 4.0f;
// Vertical inset of the row highlight within its row.
constexpr float kPopupHotInsetY = 1.0f;

// Vertical panel padding, so the highlight gap is equal on all sides.
constexpr int   kPopupPadY96 =
    static_cast<int>(kPopupHotInsetX - kPopupHotInsetY);

// Gap between an input's frame and the EDIT inside it, at 96 DPI.
constexpr int kFieldInset96 = 5;

}  // namespace ui
