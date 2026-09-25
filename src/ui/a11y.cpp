#include "ui/a11y.h"

// Must come before oleacc.h so its GUIDs are defined in this file.
#include <initguid.h>

#include <oleacc.h>

#include <mutex>

namespace ui {
namespace a11y {

namespace {

std::mutex          g_mutex;
IAccPropServices*   g_props = nullptr;
bool                g_tried = false;

// Creates the annotation service on first use; a failed attempt is not retried.
IAccPropServices* Props() {
    std::lock_guard<std::mutex> g(g_mutex);
    if (!g_tried) {
        g_tried = true;
        CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&g_props));
    }
    return g_props;
}

MSAAPROPID PropRole()  { return PROPID_ACC_ROLE; }
MSAAPROPID PropName()  { return PROPID_ACC_NAME; }
MSAAPROPID PropValue() { return PROPID_ACC_VALUE; }
MSAAPROPID PropState() { return PROPID_ACC_STATE; }

long RoleValue(Role r) {
    switch (r) {
        case Role::PushButton: return ROLE_SYSTEM_PUSHBUTTON;
        case Role::CheckBox:   return ROLE_SYSTEM_CHECKBUTTON;
        case Role::ComboBox:   return ROLE_SYSTEM_COMBOBOX;
        case Role::Text:       return ROLE_SYSTEM_STATICTEXT;
        case Role::Graphic:    return ROLE_SYSTEM_GRAPHIC;
        case Role::List:       return ROLE_SYSTEM_LIST;
        case Role::Grouping:   return ROLE_SYSTEM_GROUPING;
    }
    return ROLE_SYSTEM_CLIENT;
}

// Annotations target the window's client object.
constexpr DWORD kClient = static_cast<DWORD>(OBJID_CLIENT);

void SetStringProp(HWND h, MSAAPROPID prop, const std::wstring& text) {
    if (!h || !IsWindow(h)) return;
    IAccPropServices* p = Props();
    if (!p) return;
    p->SetHwndPropStr(h, kClient, CHILDID_SELF, prop, text.c_str());
}

void SetVariantProp(HWND h, MSAAPROPID prop, const VARIANT& v) {
    if (!h || !IsWindow(h)) return;
    IAccPropServices* p = Props();
    if (!p) return;
    p->SetHwndProp(h, kClient, CHILDID_SELF, prop, v);
}

}  // namespace

void SetName(HWND h, const std::wstring& name) { SetStringProp(h, PropName(), name); }

void SetValue(HWND h, const std::wstring& value) { SetStringProp(h, PropValue(), value); }

void SetRole(HWND h, Role role) {
    VARIANT v = {};  // same as VariantInit
    v.vt = VT_I4;
    v.lVal = RoleValue(role);
    SetVariantProp(h, PropRole(), v);
}

void Annotate(HWND h, const std::wstring& name, Role role) {
    SetName(h, name);
    SetRole(h, role);
}

void SetChecked(HWND h, bool checked) {
    // The state replaces the default one, so focusable is kept explicitly.
    VARIANT v = {};
    v.vt = VT_I4;
    v.lVal = STATE_SYSTEM_FOCUSABLE | (checked ? STATE_SYSTEM_CHECKED : 0);
    SetVariantProp(h, PropState(), v);
}

void NotifyStateChanged(HWND h) {
    if (h && IsWindow(h)) NotifyWinEvent(EVENT_OBJECT_STATECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}

void NotifyNameChanged(HWND h) {
    if (h && IsWindow(h)) NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}

void NotifyValueChanged(HWND h) {
    if (h && IsWindow(h)) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}

void Clear(HWND h) {
    if (!h) return;
    IAccPropServices* p = Props();
    if (!p) return;
    // No IsWindow check: this runs during WM_NCDESTROY.
    static const MSAAPROPID kAll[] = {PROPID_ACC_NAME, PROPID_ACC_ROLE, PROPID_ACC_VALUE,
                                      PROPID_ACC_STATE};
    p->ClearHwndProps(h, kClient, CHILDID_SELF, kAll,
                      static_cast<int>(sizeof(kAll) / sizeof(kAll[0])));
}

void Shutdown() {
    std::lock_guard<std::mutex> g(g_mutex);
    if (g_props) {
        g_props->Release();
        g_props = nullptr;
    }
    // Keeps later calls as no-ops.
    g_tried = true;
}

}  // namespace a11y
}  // namespace ui
