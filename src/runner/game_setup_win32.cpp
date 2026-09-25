// Win32 leaf of the first-run flow: IFileOpenDialog for folder selection and
// TaskDialogIndirect for the explain/recovery prompts, with a MessageBox
// fallback when comctl32 v6 is not in the activation context (the manifest
// carries it, but a repacked binary may lose it — degrade, never die).

#if defined(_WIN32)

#include "runner/game_setup.hpp"

#include "moderngekko/utf8_path.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <shobjidl.h>

#include <string>
#include <utility>
#include <vector>

namespace moderngekko::runner {
namespace {

// Balances exactly the CoInitializeEx calls that succeeded (S_OK or S_FALSE);
// RPC_E_CHANGED_MODE means someone else owns this thread's apartment.
struct ComGuard
{
  HRESULT status;
  ComGuard()
      : status(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                          COINIT_DISABLE_OLE1DDE))
  {
  }
  ~ComGuard()
  {
    if (SUCCEEDED(status))
      CoUninitialize();
  }
  bool ok() const { return SUCCEEDED(status); }
};

template <typename T>
struct ComPtr
{
  T* ptr = nullptr;
  ~ComPtr()
  {
    if (ptr)
      ptr->Release();
  }
  T* operator->() const { return ptr; }
  T** put()
  {
    if (ptr)
      ptr->Release();
    ptr = nullptr;
    return &ptr;
  }
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
};

struct CoTaskMemPtr
{
  PWSTR value = nullptr;
  ~CoTaskMemPtr()
  {
    if (value)
      CoTaskMemFree(value);
  }
};

// The console window owns our dialogs when there is one (the runner is a
// console-subsystem exe); nullptr still shows them, just unowned.
HWND DialogOwner() { return GetConsoleWindow(); }

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

TaskDialogIndirectFn ResolveTaskDialogIndirect()
{
  // TaskDialogIndirect is exported only by comctl32 v6. The runner manifest
  // declares the v6 dependency, but if it was stripped the loaded module is
  // v5.82 and the lookup fails — the MessageBox path below then covers us.
  HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
  if (!comctl)
    comctl = LoadLibraryW(L"comctl32.dll");
  if (!comctl)
    return nullptr;
  return reinterpret_cast<TaskDialogIndirectFn>(
      GetProcAddress(comctl, "TaskDialogIndirect"));
}

constexpr int kIdPrimary = 100;
constexpr int kIdSecondary = 101;

}  // namespace

SetupChoice ShowSetupDialog(const SetupDialog& dialog)
{
  const std::wstring title = Utf8ToWide(dialog.title);
  const std::wstring heading = Utf8ToWide(dialog.heading);
  const std::wstring details = Utf8ToWide(dialog.details);
  const std::wstring primary = Utf8ToWide(dialog.primary_label);
  const std::wstring secondary =
      dialog.secondary_label ? Utf8ToWide(*dialog.secondary_label) : L"";

  if (const TaskDialogIndirectFn task_dialog = ResolveTaskDialogIndirect())
  {
    const TASKDIALOG_BUTTON buttons[] = {
        {kIdPrimary, primary.c_str()},
        {kIdSecondary, secondary.c_str()},
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = DialogOwner();
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszMainInstruction = heading.c_str();
    config.pszContent = details.c_str();
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.cButtons = dialog.secondary_label ? 2u : 1u;
    config.pButtons = buttons;
    config.nDefaultButton = kIdPrimary;
    int pressed = 0;
    if (SUCCEEDED(task_dialog(&config, &pressed, nullptr, nullptr)))
    {
      if (pressed == kIdPrimary)
        return SetupChoice::Primary;
      if (pressed == kIdSecondary)
        return SetupChoice::Secondary;
      return SetupChoice::Cancel;
    }
    // Fall through to MessageBox on any TaskDialogIndirect failure.
  }

  std::wstring text = heading + L"\r\n\r\n" + details + L"\r\n";
  if (dialog.secondary_label)
  {
    text += L"\r\nYes: " + primary + L"\r\nNo: " + secondary + L"\r\nCancel: quit";
    const int answer = MessageBoxW(DialogOwner(), text.c_str(), title.c_str(),
                                   MB_YESNOCANCEL | MB_ICONWARNING | MB_SETFOREGROUND);
    if (answer == IDYES)
      return SetupChoice::Primary;
    if (answer == IDNO)
      return SetupChoice::Secondary;
    return SetupChoice::Cancel;
  }
  text += L"\r\nOK: " + primary + L"\r\nCancel: quit";
  const int answer =
      MessageBoxW(DialogOwner(), text.c_str(), title.c_str(),
                MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND);
  return answer == IDOK ? SetupChoice::Primary : SetupChoice::Cancel;
}

FolderPick PickGameFolder(std::string_view title)
{
  const ComGuard com;
  if (!com.ok())
    return {FolderPick::Outcome::Unavailable, {}};

  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                              IID_IFileOpenDialog,
                              reinterpret_cast<void**>(dialog.put()))))
    return {FolderPick::Outcome::Unavailable, {}};

  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  const std::wstring wide_title = Utf8ToWide(title);
  if (!wide_title.empty())
    dialog->SetTitle(wide_title.c_str());

  const HRESULT shown = dialog->Show(DialogOwner());
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    return {FolderPick::Outcome::Cancelled, {}};
  if (FAILED(shown))
    return {FolderPick::Outcome::Unavailable, {}};

  ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(item.put())) || !item.ptr)
    return {FolderPick::Outcome::Unavailable, {}};
  CoTaskMemPtr display_name;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &display_name.value)) ||
      !display_name.value)
    return {FolderPick::Outcome::Unavailable, {}};
  const std::filesystem::path picked = Utf8ToPath(WideToUtf8(display_name.value));
  if (picked.empty())
    return {FolderPick::Outcome::Unavailable, {}};
  return {FolderPick::Outcome::Picked, picked};
}

}  // namespace moderngekko::runner

#endif  // _WIN32
