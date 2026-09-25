// Screen reader annotations (MSAA dynamic annotation) for the custom-drawn controls.
// Every function is a no-op if the annotation service is unavailable.
#pragma once

#include <windows.h>

#include <string>

namespace ui {
namespace a11y {

// The MSAA roles used by this app, so callers need not include oleacc.h.
enum class Role {
    PushButton,
    CheckBox,
    ComboBox,
    Text,        // read-only live state
    Graphic,     // drawn, non-text surface
    List,
    Grouping,
};

// Sets the name a screen reader reads for the window.
void SetName(HWND h, const std::wstring& name);

// Overrides the role reported for the window.
void SetRole(HWND h, Role role);

// Sets name and role together.
void Annotate(HWND h, const std::wstring& name, Role role);

// Sets the control's current value as text.
void SetValue(HWND h, const std::wstring& value);

// Sets a checkbox's checked state.
void SetChecked(HWND h, bool checked);

// Tell listening clients that the window's state, name or value changed.
void NotifyStateChanged(HWND h);
void NotifyNameChanged(HWND h);
void NotifyValueChanged(HWND h);

// Removes all annotations for a window; call when it is destroyed.
void Clear(HWND h);

// Releases the annotation service at shutdown, before CoUninitialize.
void Shutdown();

}  // namespace a11y
}  // namespace ui
