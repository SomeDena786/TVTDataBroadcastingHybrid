#pragma once
#include <optional>
#include <functional>

class InputDialog
{
    HWND hDlg = nullptr;
    std::wstring characterType;
    std::optional<std::wstring> allowedCharacters;
    int maxLength;
    std::wstring value;
    std::function<void(std::unique_ptr<WCHAR[]>)> callback;
    std::wstring inputMode;
public:
    InputDialog(std::wstring characterType, std::optional<std::wstring> allowedCharacters, int maxLength, std::wstring value, std::function<void(std::unique_ptr<WCHAR[]>)> callback, std::wstring inputMode);
    ~InputDialog();
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    InputDialog(const InputDialog&) = delete;
    InputDialog& operator=(const InputDialog&) = delete;
};
