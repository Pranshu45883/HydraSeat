// Diagnostic: Dump all raw input handles vs tile handles to a log file
// Build and run to see exact mismatch
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

int main() {
    FILE* f = fopen("handle_diagnostic.txt", "w");
    if (!f) return 1;

    // 1. List ALL raw input devices
    UINT numDevices = 0;
    GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> rawList(numDevices);
    GetRawInputDeviceList(rawList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST));

    fprintf(f, "=== ALL RAW INPUT DEVICES ===\n");
    for (UINT i = 0; i < numDevices; i++) {
        const char* typeStr = "UNKNOWN";
        if (rawList[i].dwType == RIM_TYPEKEYBOARD) typeStr = "KEYBOARD";
        else if (rawList[i].dwType == RIM_TYPEMOUSE) typeStr = "MOUSE";
        else if (rawList[i].dwType == RIM_TYPEHID) typeStr = "HID";

        UINT nameSize = 0;
        GetRawInputDeviceInfoW(rawList[i].hDevice, RIDI_DEVICENAME, NULL, &nameSize);
        std::wstring nameBuf(nameSize, L'\0');
        GetRawInputDeviceInfoW(rawList[i].hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize);

        fprintf(f, "[%u] type=%s handle=0x%p path=%ls\n", i, typeStr, rawList[i].hDevice, nameBuf.c_str());
    }

    fprintf(f, "\n=== DEDUPLICATION ANALYSIS ===\n");
    fprintf(f, "The hardware_detector keeps only ONE handle per base device ID.\n");
    fprintf(f, "If raw input arrives on a DIFFERENT sub-collection handle, the handle lookup FAILS!\n");
    fprintf(f, "\nFIX: Store ALL handles for each physical device in m_handleToTileIndex.\n");

    fclose(f);

    // Also print to console
    FILE* f2 = fopen("handle_diagnostic.txt", "r");
    char buf[1024];
    while (fgets(buf, sizeof(buf), f2)) {
        printf("%s", buf);
    }
    fclose(f2);

    return 0;
}
