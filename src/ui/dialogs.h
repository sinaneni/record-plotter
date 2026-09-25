// Themed modal dialogs: messages, About, progress, text input, and shell file choosers.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ui {

enum class MsgIcon { None, Info, Warning, Error, Question };

// Options for a message dialog. Buttons are {result, label}; Escape and the close box
// return `cancel_result`.
struct MessageOptions {
    std::wstring title;
    std::wstring text;          // primary line, emphasised
    std::wstring detail;        // optional secondary paragraph, wrapped
    MsgIcon      icon = MsgIcon::Info;
    std::vector<std::pair<int, std::wstring>> buttons;  // {result, label}
    int          default_button = 0;   // index into `buttons`, activated by Enter
    int          cancel_result = 0;
    // Minimum client width in 96-dpi units; 0 uses the default.
    int          min_width96 = 0;
    // Size of the app icon shown above the text, in 96-dpi units; 0 for none.
    // Replaces the small `icon` glyph when set.
    int          hero_icon96 = 0;
};

int ShowMessage(HWND owner, const MessageOptions& opts);

// Whether any modal dialog (including the shell choosers) is open.
bool AnyDialogOpen();

// ------------------------------------------------------------------- About
// The About box's content, laid out in sections by the dialog.
struct AboutInfo {
    std::wstring icon_caption;   // reserved for a no-icon fallback
    std::wstring name;
    std::wstring version;
    std::wstring tagline;
    std::wstring blurb;          // one wrapped paragraph
    std::wstring section;        // heading of the capability table
    // {label, value} rows; values drawn in monospace.
    std::vector<std::pair<std::wstring, std::wstring>> capabilities;
    std::wstring caveat;         // small print under the table
    // {label, value} footer rows.
    std::vector<std::pair<std::wstring, std::wstring>> meta;
    int          icon96 = 96;    // masthead icon size, 96-dpi units
};

// Modal. Returns the result code of the pressed button, 0 for Close/Escape.
int ShowAboutDialog(HWND owner, const AboutInfo& info);

// Convenience wrappers with a single OK button.
void ShowInfo(HWND owner, const std::wstring& title, const std::wstring& text,
              const std::wstring& detail = L"");
void ShowWarning(HWND owner, const std::wstring& title, const std::wstring& text,
                 const std::wstring& detail = L"");
void ShowError(HWND owner, const std::wstring& title, const std::wstring& text,
               const std::wstring& detail = L"");
// Yes/No question; defaults to No.
bool AskYesNo(HWND owner, const std::wstring& title, const std::wstring& text,
              const std::wstring& detail = L"");

// ---------------------------------------------------------------- Progress
// State of background work as reported to the progress dialog.
struct ProgressSnapshot {
    double       fraction = -1.0;  // 0..1, or negative when unknown
    std::wstring detail;
    bool         finished = false; // the dialog closes on the next poll
};

struct ProgressOptions {
    std::wstring title;
    std::wstring text;            // emphasised line
    std::wstring cancel_label;    // the dialog's one button
    bool         detail_line = true;  // reserve a row for the snapshot detail
};

// Modal progress dialog. `poll` runs on the GUI thread every 100 ms. Returns 1 when
// finished, 0 when cancelled; stopping the work is the caller's job.
int ShowProgress(HWND owner, const ProgressOptions& opts,
                 const std::function<ProgressSnapshot()>& poll);

// Single-line text prompt. `value` is the initial text in and the entered text out;
// false if cancelled.
bool ShowInput(HWND owner, const std::wstring& title, const std::wstring& prompt,
               std::wstring* value);

// Folder chooser. `dir` is the starting folder in and the chosen one out.
bool PickFolder(HWND owner, const std::wstring& title, std::wstring* dir);

// Open-file chooser. `path` is the starting file or folder in and the chosen file out.
bool OpenFileDialog(HWND owner, const std::wstring& title,
                    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                    std::wstring* path);

// Save-file chooser. `filters` are {label, "*.ext"} pairs; the chosen 0-based index
// comes back in `filter_index`.
bool SaveFileDialog(HWND owner, const std::wstring& title,
                    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
                    const std::wstring& default_name, std::wstring* path,
                    int* filter_index);

// The user's Documents folder, or the profile folder as a fallback.
std::wstring DocumentsFolder();

// Opens a folder in Explorer.
void OpenFolderInShell(const std::wstring& dir);

}  // namespace ui
