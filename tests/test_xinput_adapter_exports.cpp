#include <iostream>

#if defined(_WIN32)
#include <windows.h>

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

    if (argc < 2) {
        std::cerr << "adapter DLL path missing\n";
        return 2;
    }

    HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::cerr << "adapter DLL failed to load\n";
        return 4;
    }

    const char* exports[] = {
        "XInputGetState",
        "XInputSetState",
        "XInputGetCapabilities",
    };
    for (const char* name : exports) {
        if (GetProcAddress(module, name) == nullptr) {
            std::cerr << "missing export: " << name << '\n';
            FreeLibrary(module);
            return 5;
        }
    }

    FreeLibrary(module);
    std::cout << "XInput adapter exports verified\n";
    return 0;
}
#else
int main() {
    return 0;
}
#endif
