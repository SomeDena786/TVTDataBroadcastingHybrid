#include "pch.h"
#include "InputDialog.h"
#include "resource.h"

static const auto inputCharactersDescription = std::map<std::wstring_view, std::wstring_view>
{
    { L"number", L"半角数字" },
    { L"alphabet", L"半角英字と半角記号" },
    { L"hankaku", L"半角英数と半角記号" },
    { L"zenkaku", L"全角ひらがなと全角カタカナと全角英数と全角記号" },
    { L"katakana", L"全角カタカナと全角記号" },
    { L"hiragana", L"全角ひたがなと全角記号" },
};

static std::optional<WCHAR> ValidateString(std::wstring_view allowedCharacters, std::wstring_view input)
{
    for (auto c : input)
    {
        if (allowedCharacters.find(c) == std::wstring::npos)
        {
            return c;
        }
    }
    return std::nullopt;
}

InputDialog::InputDialog(std::wstring characterType, std::optional<std::wstring> allowedCharacters, int maxLength, std::wstring value, std::function<void(std::unique_ptr<WCHAR[]> callback)> callback, std::wstring inputMode) : characterType(std::move(characterType)), allowedCharacters(std::move(allowedCharacters)), maxLength(maxLength), value(std::move(value)), callback(std::move(callback)), inputMode(std::move(inputMode))
{

}

INT_PTR CALLBACK InputDialog::DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    InputDialog* pThis = static_cast<InputDialog*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        pThis->hDlg = hDlg;
        auto input = GetDlgItem(hDlg, IDC_INPUT);
        SetFocus(input);
        if (pThis->characterType == L"number")
        {
            SetWindowLongW(input, GWL_STYLE, GetWindowLongW(input, GWL_STYLE) | ES_NUMBER);
        }
        if (pThis->inputMode == L"password")
        {
            SendMessageW(input, EM_SETPASSWORDCHAR, L'*', 0);
        }
        SetDlgItemTextW(hDlg, IDC_INPUT, pThis->value.c_str());
        SendMessageW(input, EM_SETSEL, pThis->value.length(), pThis->value.length());
        SendDlgItemMessageW(hDlg, IDC_INPUT, EM_LIMITTEXT, pThis->maxLength, 0);
        break;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK)
            {
                std::unique_ptr<WCHAR[]> text(new WCHAR[pThis->maxLength + 1]);
                text[0] = 0;
                GetDlgItemTextW(hDlg, IDC_INPUT, text.get(), pThis->maxLength + 1);
                auto invalidChar = pThis->allowedCharacters ? ValidateString(pThis->allowedCharacters.value(), text.get()) : std::nullopt;
                if (invalidChar)
                {
                    auto desc = inputCharactersDescription.find(pThis->characterType);
                    if (desc != inputCharactersDescription.end())
                    {
                        MessageBoxW(hDlg, (std::wstring(L"不正な文字「") + invalidChar.value() + L"」が含まれています。\n" + desc->second.data() + L"のみ入力できます。").c_str(), nullptr, MB_OK | MB_ICONERROR);
                    }
                    else
                    {
                        MessageBoxW(hDlg, (std::wstring(L"不正な文字「") + invalidChar.value() + L"」が含まれています。").c_str(), nullptr, MB_OK | MB_ICONERROR);
                    }
                    return 0;
                }
                else
                {
                    pThis->callback(std::move(text));
                }
            }
            else
            {
                pThis->callback({});
            }
            EndDialog(hDlg, wParam);
        }
        break;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    case WM_DESTROY:
    {
        pThis->hDlg = nullptr;
        return 1;
    }
    }
    return 0;
}

InputDialog::~InputDialog()
{
    DestroyWindow(this->hDlg);
}
