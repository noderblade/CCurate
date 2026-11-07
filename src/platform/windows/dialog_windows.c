#define CCURATE_WINDOWS_NATIVE
#define COBJMACROS
#include "platform/windows/dialog_windows.h"
#include "curator.h"
#include "platform/windows/native_windows.h"
#include <objbase.h>
#include <shobjidl.h>

static const GUID file_open_class = {
    0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const GUID file_open_interface = {
    0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const GUID shell_item_interface = {
    0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};

char *cc_win_choose_folder(Rid parent, const char *title, const char *initial)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *dialog = NULL;
    IShellItem *item = NULL;
    char *result = NULL;
    RNativeWindow native = {0};
    Rwindow_get_native_handle(parent, &native);
    if (FAILED(CoCreateInstance(&file_open_class, NULL, CLSCTX_INPROC_SERVER, &file_open_interface,
                                (void **)&dialog)))
        goto done;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(dialog, &options);
    IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                           FOS_NOCHANGEDIR | FOS_FORCESHOWHIDDEN |
                                           FOS_DONTADDTORECENT | FOS_PATHMUSTEXIST);
    wchar_t *wide_title = cc_win_wide(title);
    wchar_t *wide_initial = initial ? cc_win_wide(initial) : NULL;
    if (wide_initial)
        for (wchar_t *p = wide_initial; *p; p++)
            if (*p == '/')
                *p = '\\';
    if (wide_title)
        IFileOpenDialog_SetTitle(dialog, wide_title);
    if (wide_initial && SUCCEEDED(SHCreateItemFromParsingName(
                            wide_initial, NULL, &shell_item_interface, (void **)&item))) {
        IFileOpenDialog_SetFolder(dialog, item);
        IShellItem_Release(item);
        item = NULL;
    }
    free(wide_title);
    free(wide_initial);
    if (SUCCEEDED(IFileOpenDialog_Show(dialog, (HWND)native.window)) &&
        SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item))) {
        PWSTR path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            result = cc_win_utf8(path);
            CoTaskMemFree(path);
        }
    }
done:
    if (item)
        IShellItem_Release(item);
    if (dialog)
        IFileOpenDialog_Release(dialog);
    if (SUCCEEDED(init))
        CoUninitialize();
    return result;
}
