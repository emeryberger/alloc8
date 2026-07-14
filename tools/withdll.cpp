//
// withdll.exe -- run a process with DLLs injected (Detours).
//
// This replaces the withdll sample that ships with Detours, for ONE reason:
// the sample launches the target with a zeroed STARTUPINFO, so it never sets
// STARTF_USESTDHANDLES. The target then does NOT pick up redirected standard
// handles, and everything it prints is lost:
//
//     withdll.exe /d:alloc.dll bench.exe > out.txt
//     ...out.txt contains withdll's own banner and NOTHING from bench.exe.
//
// That made it impossible to capture benchmark output on Windows -- exit codes
// were the only signal, so Windows could be crash-tested but never
// performance-tested. Here we pass the parent's standard handles through
// explicitly, so a redirected stdout/stderr reaches the injected process.
//
// The command line is deliberately compatible with the Detours sample:
//
//     withdll.exe [/d:dll_to_inject]... command [args...]
//
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <detours.h>

static void usage()
{
    fprintf(stderr,
            "Usage:\n"
            "    withdll.exe [options] command [args...]\n"
            "Options:\n"
            "    /d:file.dll   Inject file.dll (repeatable).\n"
            "    /?            This help.\n");
}

int main(int argc, char ** argv)
{
    LPCSTR dlls[64];
    DWORD  nDlls = 0;

    int arg = 1;
    for (; arg < argc; arg++) {
        if (argv[arg][0] != '/' && argv[arg][0] != '-') {
            break;                       // start of the command
        }
        char opt = argv[arg][1];
        if ((opt == 'd' || opt == 'D') && argv[arg][2] == ':') {
            if (nDlls < ARRAYSIZE(dlls)) {
                dlls[nDlls++] = argv[arg] + 3;
            }
        }
        else if (opt == '?') {
            usage();
            return 0;
        }
        else {
            fprintf(stderr, "withdll.exe: unknown option `%s'.\n", argv[arg]);
            usage();
            return 9001;
        }
    }

    if (nDlls == 0 || arg >= argc) {
        usage();
        return 9001;
    }

    // Rebuild the command line, quoting any argument containing whitespace.
    char cmd[8192];
    cmd[0] = '\0';
    for (int i = arg; i < argc; i++) {
        bool needsQuotes = (strchr(argv[i], ' ') != NULL ||
                            strchr(argv[i], '\t') != NULL);
        if (needsQuotes) { strcat_s(cmd, sizeof(cmd), "\""); }
        strcat_s(cmd, sizeof(cmd), argv[i]);
        if (needsQuotes) { strcat_s(cmd, sizeof(cmd), "\""); }
        if (i + 1 < argc) { strcat_s(cmd, sizeof(cmd), " "); }
    }

    printf("withdll.exe: Starting: `%s'\n", cmd);
    for (DWORD n = 0; n < nDlls; n++) {
        printf("withdll.exe:   with `%s'\n", dlls[n]);
    }
    fflush(stdout);   // our banner must not sit in a buffer behind the child's output

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    // THE POINT OF THIS PROGRAM. Hand our standard handles to the child
    // explicitly. Without STARTF_USESTDHANDLES the child does not inherit a
    // REDIRECTED stdout/stderr, and its output is lost -- which is exactly what
    // the Detours sample does.
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    // The handles must be inheritable for the child to actually receive them;
    // a handle that came from shell redirection normally already is, but say so.
    for (HANDLE h : { si.hStdInput, si.hStdOutput, si.hStdError }) {
        if (h != NULL && h != INVALID_HANDLE_VALUE) {
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }

    char fullExe[1024] = "\0";
    char * fileExe = NULL;
    SearchPathA(NULL, argv[arg], ".exe", ARRAYSIZE(fullExe), fullExe, &fileExe);

    DWORD dwFlags = CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED;

    if (!DetourCreateProcessWithDllsA(fullExe[0] ? fullExe : NULL, cmd,
                                      NULL, NULL,
                                      TRUE,        // bInheritHandles
                                      dwFlags, NULL, NULL,
                                      &si, &pi, nDlls, dlls, NULL)) {
        DWORD err = GetLastError();
        fprintf(stderr, "withdll.exe: DetourCreateProcessWithDlls failed: %lu\n", err);
        if (err == ERROR_INVALID_HANDLE) {
            fprintf(stderr, "withdll.exe: bitness mismatch between this process "
                            "and the target.\n");
        }
        return 9009;
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD result = 0;
    if (!GetExitCodeProcess(pi.hProcess, &result)) {
        fprintf(stderr, "withdll.exe: GetExitCodeProcess failed: %lu\n",
                GetLastError());
        result = 9010;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int) result;
}
