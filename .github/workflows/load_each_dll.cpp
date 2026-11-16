// CI diagnostic (Windows): try to load each DLL given on the command line, and report the ones that fail.
// "The specified procedure could not be found" (ERROR_PROC_NOT_FOUND, 127) means that the DLL imports a
// function that one of its dependencies does not export (version mismatch between DLLs).
#include <windows.h>
#include <cstdio>

int
main(int argc, char* argv[])
{
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        HMODULE h = LoadLibraryExA(argv[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) {
            DWORD err = GetLastError();
            char msg[512] = { 0 };
            FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, msg, sizeof(msg), NULL);
            std::printf("FAILED to load %s: error %lu: %s", argv[i], (unsigned long)err, msg);
            ++failures;
        } else {
            FreeLibrary(h);
        }
    }
    std::printf("%d of %d DLLs failed to load\n", failures, argc - 1);
    return 0;
}
