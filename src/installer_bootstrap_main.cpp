#include "hydra/installer_bootstrap.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace {

using hydra::installer::BootstrapInstalledState;
using hydra::installer::BootstrapOperation;
using hydra::installer::BootstrapPackageAssessment;
using hydra::installer::BootstrapPackageLayout;
using hydra::installer::BootstrapPowerShellInvocation;
using hydra::installer::BootstrapProcessResult;
using hydra::installer::BootstrapReleaseIdentity;

constexpr wchar_t kWindowClass[] = L"HydraSeatSetupWindowV1";
constexpr int kInstallButton = 1001;
constexpr int kRepairButton = 1002;
constexpr int kUninstallButton = 1003;
constexpr int kCloseButton = 1004;

struct SetupUiState {
    HINSTANCE instance{nullptr};
    HWND window{nullptr};
    HWND stateLabel{nullptr};
    HWND packageLabel{nullptr};
    HWND destinationLabel{nullptr};
    HWND detailLabel{nullptr};
    HWND installButton{nullptr};
    HWND repairButton{nullptr};
    HWND uninstallButton{nullptr};

    std::filesystem::path setupExecutable;
    std::filesystem::path powershellExecutable;
    BootstrapPackageLayout package;
    BootstrapPackageAssessment packageAssessment;
    bool packageValidated{false};
    std::optional<BootstrapReleaseIdentity> releaseIdentity;
    hydra::installer::BootstrapInstalledInfo installed;
};

std::filesystem::path currentExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool supportedNativeArchitecture() noexcept {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

bool currentProcessIsElevated() noexcept {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return true;
    }
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool queried = GetTokenInformation(
        rawToken, TokenElevation, &elevation, sizeof(elevation), &bytes) != FALSE;
    CloseHandle(rawToken);
    return !queried || elevation.TokenIsElevated != 0;
}

std::wstring packageText(const SetupUiState& state) {
    if (!state.packageValidated) {
        switch (state.packageAssessment.status) {
        case hydra::installer::BootstrapPackageStatus::NotReleaseLayout:
            return L"Package: not a distributable release package";
        case hydra::installer::BootstrapPackageStatus::MissingRequiredPath:
            return L"Package: incomplete or unsigned";
        case hydra::installer::BootstrapPackageStatus::ReparsePointRejected:
            return L"Package: blocked unsafe path";
        case hydra::installer::BootstrapPackageStatus::SignatureRejected:
            return L"Package: signature is not trusted";
        case hydra::installer::BootstrapPackageStatus::UnexpectedLayout:
            return L"Package: invalid release layout";
        case hydra::installer::BootstrapPackageStatus::Valid:
            break;
        }
        return L"Package: unavailable";
    }
    if (!state.releaseIdentity) {
        return L"Package: signature validated";
    }
    return L"Package: HydraSeat " + state.releaseIdentity->version + L" (rev " +
           std::to_wstring(state.releaseIdentity->revision) + L")";
}

std::wstring installedText(const SetupUiState& state) {
    switch (state.installed.state) {
    case BootstrapInstalledState::NotInstalled:
        return L"Current state: Not installed";
    case BootstrapInstalledState::Installed:
        return state.installed.version.empty()
            ? L"Current state: Installed"
            : L"Current state: Installed " + state.installed.version;
    case BootstrapInstalledState::Inconsistent:
        return L"Current state: Needs repair or manual recovery";
    }
    return L"Current state: Unknown";
}

void refreshInstalledState(SetupUiState& state) {
    state.installed = hydra::installer::queryBootstrapInstalledInfo();
    SetWindowTextW(state.stateLabel, installedText(state).c_str());
    const std::wstring destination = L"Destination: " + state.installed.installRoot.wstring();
    SetWindowTextW(state.destinationLabel, destination.c_str());

    const bool packageReady = state.packageValidated;
    const bool installed = state.installed.state == BootstrapInstalledState::Installed;
    const bool inconsistent = state.installed.state == BootstrapInstalledState::Inconsistent;
    EnableWindow(state.installButton, packageReady && !installed && !inconsistent);
    EnableWindow(state.repairButton, packageReady && (installed || inconsistent));
    EnableWindow(state.uninstallButton, packageReady && installed);
}

std::wstring processFailureText(BootstrapOperation operation,
                                std::uint32_t exitCode,
                                std::uint32_t systemError) {
    std::wstring text = L"HydraSeat ";
    text += hydra::installer::bootstrapPowerShellMode(operation);
    text += L" did not complete.";
    if (systemError != 0) {
        text += L" Windows error: ";
        text += std::to_wstring(systemError);
        text += L".";
    } else {
        text += L" Installer exit code: ";
        text += std::to_wstring(exitCode);
        text += L".";
    }
    return text;
}

bool confirmOperation(HWND owner, BootstrapOperation operation) {
    std::wstring message;
    switch (operation) {
    case BootstrapOperation::Install:
        message = L"Install HydraSeat to Program Files?\n\nAdministrator permission will be requested only after you continue.";
        break;
    case BootstrapOperation::Repair:
        message = L"Repair the exact installed HydraSeat release using this validated package?\n\nAdministrator permission will be requested only after you continue.";
        break;
    case BootstrapOperation::Uninstall:
        message = L"Uninstall HydraSeat and return its owned machine state to ordinary Windows?\n\nAdministrator permission will be requested only after you continue.";
        break;
    case BootstrapOperation::Validate:
        return true;
    }
    return MessageBoxW(owner, message.c_str(), L"HydraSeat Setup",
                       MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK;
}

std::optional<bool> askRemoveUserData(HWND owner) {
    const auto choice = MessageBoxW(
        owner,
        L"Remove HydraSeat Player profiles and local per-user settings too?\n\n"
        L"Yes = remove HydraSeat per-user data\nNo = keep it\nCancel = do not uninstall",
        L"HydraSeat Setup", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (choice == IDCANCEL) {
        return std::nullopt;
    }
    return choice == IDYES;
}

void setBusy(SetupUiState& state, bool busy) {
    EnableWindow(state.installButton, !busy);
    EnableWindow(state.repairButton, !busy);
    EnableWindow(state.uninstallButton, !busy);
    EnableWindow(GetDlgItem(state.window, kCloseButton), !busy);
    if (!busy) {
        refreshInstalledState(state);
    }
}

void performOperation(SetupUiState& state, BootstrapOperation operation) {
    if (!state.packageValidated) {
        MessageBoxW(state.window,
                    L"This setup package has not passed the signed package validation contract.",
                    L"HydraSeat Setup", MB_OK | MB_ICONERROR);
        return;
    }
    if (!confirmOperation(state.window, operation)) {
        SetWindowTextW(state.detailLabel, L"No changes were requested.");
        return;
    }

    bool removeUserData = false;
    if (operation == BootstrapOperation::Uninstall) {
        const auto choice = askRemoveUserData(state.window);
        if (!choice) {
            SetWindowTextW(state.detailLabel, L"Uninstall cancelled before elevation.");
            return;
        }
        removeUserData = *choice;
    }

    BootstrapPowerShellInvocation invocation;
    std::wstring invocationError;
    const std::optional<std::filesystem::path> packageRoot =
        operation == BootstrapOperation::Uninstall
            ? std::optional<std::filesystem::path>{}
            : std::optional<std::filesystem::path>{state.package.packageRoot};
    const std::optional<BootstrapReleaseIdentity> expectedRelease =
        operation == BootstrapOperation::Install || operation == BootstrapOperation::Repair
            ? state.releaseIdentity
            : std::optional<BootstrapReleaseIdentity>{};
    if (!hydra::installer::makeBootstrapPowerShellInvocation(
            operation, state.powershellExecutable, state.package.installerScript,
            packageRoot, expectedRelease, removeUserData, invocation, &invocationError)) {
        MessageBoxW(state.window, invocationError.c_str(), L"HydraSeat Setup",
                    MB_OK | MB_ICONERROR);
        return;
    }

    setBusy(state, true);
    SetWindowTextW(state.detailLabel, L"Waiting for the reviewed HydraSeat installer operation...");
    std::uint32_t exitCode = 0;
    std::uint32_t systemError = 0;
    const auto result = hydra::installer::runBootstrapPowerShell(
        invocation, &exitCode, &systemError);
    setBusy(state, false);

    if (result == BootstrapProcessResult::Cancelled) {
        SetWindowTextW(state.detailLabel, L"Administrator permission was cancelled. No setup success is being reported.");
        return;
    }
    if (result == BootstrapProcessResult::Failed) {
        const auto failure = processFailureText(operation, exitCode, systemError);
        SetWindowTextW(state.detailLabel, failure.c_str());
        MessageBoxW(state.window, failure.c_str(), L"HydraSeat Setup",
                    MB_OK | MB_ICONERROR);
        return;
    }

    refreshInstalledState(state);
    switch (operation) {
    case BootstrapOperation::Install: {
        SetWindowTextW(state.detailLabel, L"Install completed successfully.");
        const auto launch = MessageBoxW(
            state.window,
            L"HydraSeat installed successfully. Launch HydraSeat now?\n\n"
            L"The application will be launched by this non-elevated setup process.",
            L"HydraSeat Setup", MB_YESNO | MB_ICONINFORMATION);
        if (launch == IDYES) {
            std::uint32_t launchError = 0;
            if (!hydra::installer::launchInstalledHydraSeatNormally(
                    state.installed, &launchError)) {
                const auto message = L"HydraSeat installed, but normal launch failed. Windows error: " +
                                     std::to_wstring(launchError) + L".";
                MessageBoxW(state.window, message.c_str(), L"HydraSeat Setup",
                            MB_OK | MB_ICONWARNING);
            }
        }
        break;
    }
    case BootstrapOperation::Repair:
        SetWindowTextW(state.detailLabel, L"Repair completed successfully.");
        MessageBoxW(state.window, L"HydraSeat repair completed successfully.",
                    L"HydraSeat Setup", MB_OK | MB_ICONINFORMATION);
        break;
    case BootstrapOperation::Uninstall:
        SetWindowTextW(state.detailLabel, L"Uninstall completed successfully.");
        MessageBoxW(state.window, L"HydraSeat uninstall completed successfully.",
                    L"HydraSeat Setup", MB_OK | MB_ICONINFORMATION);
        break;
    case BootstrapOperation::Validate:
        break;
    }
}

HWND createLabel(HWND parent, int x, int y, int width, int height,
                 const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, width, height, parent, nullptr, nullptr, nullptr);
}

HWND createButton(HWND parent, int id, int x, int y, int width,
                  const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                           x, y, width, 34, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           nullptr, nullptr);
}

void applyDefaultFont(HWND control) {
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
}

LRESULT CALLBACK setupWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SetupUiState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<SetupUiState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = hwnd;
    }

    switch (message) {
    case WM_COMMAND:
        if (state == nullptr || HIWORD(wParam) != BN_CLICKED) {
            break;
        }
        switch (LOWORD(wParam)) {
        case kInstallButton:
            performOperation(*state, BootstrapOperation::Install);
            return 0;
        case kRepairButton:
            performOperation(*state, BootstrapOperation::Repair);
            return 0;
        case kUninstallButton:
            performOperation(*state, BootstrapOperation::Uninstall);
            return 0;
        case kCloseButton:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool createSetupWindow(SetupUiState& state) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = state.instance;
    windowClass.lpfnWndProc = setupWindowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    state.window = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, L"HydraSeat Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 590, 365,
        nullptr, nullptr, state.instance, &state);
    if (state.window == nullptr) {
        return false;
    }

    const auto title = createLabel(state.window, 28, 24, 520, 34, L"HydraSeat Setup");
    state.stateLabel = createLabel(state.window, 28, 70, 520, 24, L"");
    state.packageLabel = createLabel(state.window, 28, 100, 520, 24, packageText(state).c_str());
    state.destinationLabel = createLabel(state.window, 28, 130, 520, 24, L"");
    state.detailLabel = createLabel(state.window, 28, 162, 520, 62,
        state.packageValidated
            ? L"Requires: 64-bit Windows. Reboot: not requested by this installer.\r\nChoose one operation; UAC appears only after you confirm a change."
            : state.packageAssessment.diagnostic.c_str());
    state.installButton = createButton(state.window, kInstallButton, 28, 232, 120, L"Install");
    state.repairButton = createButton(state.window, kRepairButton, 158, 232, 120, L"Repair");
    state.uninstallButton = createButton(state.window, kUninstallButton, 288, 232, 120, L"Uninstall");
    const auto closeButton = createButton(state.window, kCloseButton, 428, 232, 120, L"Close");

    for (const auto control : {title, state.stateLabel, state.packageLabel,
                               state.destinationLabel, state.detailLabel,
                               state.installButton, state.repairButton,
                               state.uninstallButton, closeButton}) {
        applyDefaultFont(control);
    }

    refreshInstalledState(state);
    ShowWindow(state.window, SW_SHOWNORMAL);
    UpdateWindow(state.window);
    return true;
}

bool validatePackageWithoutElevation(SetupUiState& state) {
    if (!supportedNativeArchitecture()) {
        state.packageAssessment.diagnostic = L"HydraSeat v1 setup requires native x64 Windows.";
        return false;
    }
    if (!hydra::installer::inspectBootstrapPackageLayout(
            state.setupExecutable, state.package, state.packageAssessment)) {
        return false;
    }

    std::wstring powershellError;
    state.powershellExecutable = hydra::installer::systemWindowsPowerShellPath(&powershellError);
    if (state.powershellExecutable.empty()) {
        state.packageAssessment.diagnostic = powershellError;
        return false;
    }

    BootstrapPowerShellInvocation validation;
    std::wstring invocationError;
    if (!hydra::installer::makeBootstrapPowerShellInvocation(
            BootstrapOperation::Validate, state.powershellExecutable,
            state.package.installerScript, state.package.packageRoot,
            std::nullopt, false, validation, &invocationError)) {
        state.packageAssessment.diagnostic = invocationError;
        return false;
    }
    std::uint32_t exitCode = 0;
    std::uint32_t systemError = 0;
    const auto result = hydra::installer::runBootstrapPowerShell(
        validation, &exitCode, &systemError);
    if (result != BootstrapProcessResult::Success) {
        state.packageAssessment.diagnostic =
            L"The signed installer package validation failed before elevation. Exit=" +
            std::to_wstring(exitCode) + L", Windows=" +
            std::to_wstring(systemError) + L".";
        return false;
    }

    std::wstring identityError;
    state.releaseIdentity = hydra::installer::readValidatedBootstrapReleaseIdentity(
        state.package.signingProvenance, &identityError);
    if (!state.releaseIdentity) {
        state.packageAssessment.diagnostic = identityError;
        return false;
    }
    state.packageValidated = true;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (currentProcessIsElevated()) {
        MessageBoxW(nullptr,
                    L"HydraSeat Setup must start without administrator rights. Close this copy and double-click HydraSeatSetup.exe normally; setup will request UAC only after you confirm an install change.",
                    L"HydraSeat Setup", MB_OK | MB_ICONWARNING);
        return 2;
    }

    SetupUiState state;
    state.instance = instance;
    state.setupExecutable = currentExecutablePath();
    state.installed = hydra::installer::queryBootstrapInstalledInfo();
    if (state.setupExecutable.empty()) {
        MessageBoxW(nullptr, L"HydraSeat Setup could not resolve its executable path.",
                    L"HydraSeat Setup", MB_OK | MB_ICONERROR);
        return 2;
    }

    (void)validatePackageWithoutElevation(state);
    if (!createSetupWindow(state)) {
        MessageBoxW(nullptr, L"HydraSeat Setup could not create its Windows interface.",
                    L"HydraSeat Setup", MB_OK | MB_ICONERROR);
        return 2;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
