#include "ford_sync2_gps_section.h"

#include "ford_sync2_gps_source.h"
#include "ford_sync2_ilp_channel.h"
#include "../../core/cerf_emulator.h"
#include "../../host/host_dark_mode.h"
#include "../../host/host_window.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

std::wstring GpsSection::Degrees(int32_t value) {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(7) << value / 10000000.0;
    return text.str();
}

std::wstring GpsSection::Label() const {
    const auto snapshot = emu_.Get<FordSync2GpsSource>().Read();
    if (!snapshot.fix) return L"Location: GPS not reported";
    const auto& f = *snapshot.fix;
    return L"Location: " + Degrees(f.latitude_e7) + L", " + Degrees(f.longitude_e7);
}

std::wstring GpsSection::TooltipText() const {
    const auto snapshot = emu_.Get<FordSync2GpsSource>().Read();
    if (!snapshot.fix) return L"Location: GPS not reported";
    const auto& f = *snapshot.fix;
    wchar_t utc[48];
    swprintf_s(utc, L"%04u-%02u-%02u %02u:%02u:%02u UTC", f.year, f.month, f.day,
               f.hour, f.minute, f.second);
    return Label() + L"\nGPS time: " + utc + L" (system clock)";
}

std::vector<WidgetMenuItem> GpsSection::BuildItems() {
    const auto snapshot = emu_.Get<FordSync2GpsSource>().Read();
    WidgetMenuItem edit;
    edit.label = L"Edit latitude / longitude...";
    edit.on_click = [this] { EditCoordinates(); };
    WidgetMenuItem preset;
    preset.label = L"Reset to Ford Glass House, One American Road";
    preset.checked = snapshot.fix &&
        snapshot.fix->latitude_e7 == FordSync2GpsSource::kDearbornLatitudeE7 &&
        snapshot.fix->longitude_e7 == FordSync2GpsSource::kDearbornLongitudeE7;
    preset.on_click = [this] {
        emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] { emu_.Get<FordSync2GpsSource>().SetDearborn(); });
    };
    WidgetMenuItem clear;
    clear.label = L"Stop reporting GPS";
    clear.checked = !snapshot.fix;
    clear.on_click = [this] {
        emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] { emu_.Get<FordSync2GpsSource>().Clear(); });
    };
    return {std::move(edit), std::move(preset), std::move(clear)};
}

bool GpsSection::PollDirty() {
    auto& gps = emu_.Get<FordSync2GpsSource>();
    gps.SyncClock();
    const auto revision = gps.Read().revision;
    if (revision == last_revision_) return false;
    last_revision_ = revision;
    return true;
}

bool GpsSection::ParseDegrees(HWND dlg, int id, double& value) {
    wchar_t text[80];
    GetDlgItemTextW(dlg, id, text, static_cast<int>(std::size(text)));
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    if (!(input >> value) || !std::isfinite(value)) return false;
    input >> std::ws;
    return input.eof();
}

/* https://learn.microsoft.com/windows/win32/dlgbox/using-dialog-boxes */
INT_PTR CALLBACK GpsSection::CoordinateDialog(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<GpsSection*>(GetWindowLongPtrW(dlg, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<GpsSection*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        const auto snapshot = self->emu_.Get<FordSync2GpsSource>().Read();
        const int32_t lat = snapshot.fix ? snapshot.fix->latitude_e7 : FordSync2GpsSource::kDearbornLatitudeE7;
        const int32_t lon = snapshot.fix ? snapshot.fix->longitude_e7 : FordSync2GpsSource::kDearbornLongitudeE7;
        SetDlgItemTextW(dlg, 1001, Degrees(lat).c_str());
        SetDlgItemTextW(dlg, 1002, Degrees(lon).c_str());
        for (int id : {1001, 1002}) SendDlgItemMessageW(dlg, id, EM_LIMITTEXT, 64, 0);
        self->emu_.Get<HostDarkMode>().ApplyToDialog(dlg);
        SendDlgItemMessageW(dlg, 1001, EM_SETSEL, 0, -1);
        return TRUE;
    }
    if (!self) return FALSE;
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC ||
        msg == WM_CTLCOLORBTN || msg == WM_CTLCOLOREDIT) {
        LRESULT brush;
        if (self->emu_.Get<HostDarkMode>().HandleCtlColor(msg, wp, brush)) return brush;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        double latitude = 0, longitude = 0;
        bool accepted = false;
        if (ParseDegrees(dlg, 1001, latitude) && ParseDegrees(dlg, 1002, longitude)) {
            self->emu_.Get<FordSync2IlpChannel>().ApplyHostChange([&] {
                accepted = self->emu_.Get<FordSync2GpsSource>().SetCoordinates(latitude, longitude);
            });
        }
        if (accepted) EndDialog(dlg, IDOK);
        else MessageBoxW(dlg, L"Enter decimal degrees using a period. Latitude must be greater than -90 and less than 90; longitude greater than -180 and less than 180.",
                         L"Invalid coordinates", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void GpsSection::EditCoordinates() {
    if (DialogBoxParamW(GetModuleHandleW(nullptr), L"SYNC2_LOCATION", emu_.Get<HostWindow>().Hwnd(),
                        CoordinateDialog, reinterpret_cast<LPARAM>(this)) == -1)
        MessageBoxW(emu_.Get<HostWindow>().Hwnd(), L"The Location dialog could not be opened.",
                    L"Location", MB_OK | MB_ICONERROR);
}
