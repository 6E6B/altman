#include "system_info.h"

#include <array>
#include <cstring>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <fcntl.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

namespace SystemInfo {

    Platform GetCurrentPlatform() noexcept {
#ifdef _WIN32
        return Platform::Windows;
#elif __APPLE__
        return Platform::macOS;
#else
        return Platform::Unknown;
#endif
    }

    std::string_view GetPlatformString() noexcept {
        switch (GetCurrentPlatform()) {
            case Platform::Windows:
                return "Windows";
            case Platform::macOS:
                return "macOS";
            default:
                return "Unknown";
        }
    }

    std::string GetHardwareArchitecture() {
#ifdef _WIN32
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;

        if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
            return "unknown";
        }

        switch (nativeMachine) {
            case IMAGE_FILE_MACHINE_ARM64:
                return "arm64";
            case IMAGE_FILE_MACHINE_AMD64:
                return "x86_64";
            case IMAGE_FILE_MACHINE_I386:
                return "x86";
            default:
                return "unknown";
        }
#elif __APPLE__
        int isArm = 0;
        size_t size = sizeof(isArm);
        if (sysctlbyname("hw.optional.arm64", &isArm, &size, nullptr, 0) == 0 && isArm) {
            return "arm64";
        }
        return "x86_64";
#else
        return "unknown";
#endif
    }

    bool IsRunningUnderEmulation() {
#ifdef _WIN32
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;

        if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
            return false;
        }

#if defined(_M_X64)
        // x64 binary on ARM64 → emulated
        return nativeMachine == IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_IX86)
        // x86 binary on ARM64 or x64 → emulated
        return nativeMachine == IMAGE_FILE_MACHINE_ARM64 || nativeMachine == IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_ARM64)
        // ARM64 / ARM64EC is never emulated
        return false;
#else
        return false;
#endif
#elif __APPLE__
        return IsRunningUnderRosetta();
#else
        return false;
#endif
    }

#ifdef __APPLE__
    bool IsRunningUnderRosetta() {
        int translated = 0;
        size_t size = sizeof(translated);
        if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0) {
            return translated == 1;
        }
        return false;
    }
#endif

    CpuArchitecture GetCpuArchitecture() {
#ifdef _WIN32
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;

        if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
            return CpuArchitecture::Unknown;
        }

#if defined(_M_X64)
        if (nativeMachine == IMAGE_FILE_MACHINE_ARM64) {
            return CpuArchitecture::x86_64_Emulated;
        }
        return CpuArchitecture::x86_64;
#elif defined(_M_ARM64)
        return CpuArchitecture::Arm64;
#elif defined(_M_IX86)
        return CpuArchitecture::x86_64_Emulated;
#else
        return CpuArchitecture::Unknown;
#endif
#elif __APPLE__
        if (IsRunningUnderRosetta()) {
            return CpuArchitecture::x86_64_Emulated;
        }

        if (GetHardwareArchitecture() == "arm64") {
            return CpuArchitecture::Arm64;
        }

        return CpuArchitecture::x86_64;
#else
        return CpuArchitecture::Unknown;
#endif
    }

    std::string_view GetArchitectureString() {
        switch (GetCpuArchitecture()) {
            case CpuArchitecture::Arm64:
                return "arm64";
            case CpuArchitecture::x86_64:
                return "x86_64";
            case CpuArchitecture::x86_64_Emulated:
                return "x86_64_emulated";
            default:
                return "unknown";
        }
    }

#ifdef __APPLE__
    bool ExecuteCommand(const std::string &command, std::string &output) {
        std::array<char, 128> buffer;
        output.clear();

        FILE *pipe = popen((command + " 2>&1").c_str(), "r");
        if (!pipe) {
            return false;
        }

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }

        const int exitCode = pclose(pipe);
        return WEXITSTATUS(exitCode) == 0;
    }

    bool SpawnProcessWithEnv(const char *program, const std::vector<const char *> &args, const SpawnOptions &opts) {

        // Build environment vector
        std::vector<std::string> envStrings;
        envStrings.reserve(opts.env.size() + 32);

        // Copy existing environment, excluding overridden keys
        for (char **env = environ; *env; ++env) {
            std::string entry(*env);
            auto pos = entry.find('=');
            if (pos == std::string::npos) {
                continue;
            }

            std::string key = entry.substr(0, pos);
            if (!opts.env.contains(key)) {
                envStrings.emplace_back(std::move(entry));
            }
        }

        // Add custom environment variables
        for (const auto &[k, v]: opts.env) {
            envStrings.emplace_back(k + "=" + v);
        }

        // Convert to C-style array
        std::vector<std::unique_ptr<char[]>> envBuffers;
        std::vector<char *> envp;
        envBuffers.reserve(envStrings.size());
        envp.reserve(envStrings.size() + 1);

        for (const auto &s: envStrings) {
            auto buf = std::make_unique<char[]>(s.size() + 1);
            std::memcpy(buf.get(), s.c_str(), s.size() + 1);
            envp.push_back(buf.get());
            envBuffers.push_back(std::move(buf));
        }
        envp.push_back(nullptr);

        // Build argument vector
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);

        for (const char *arg: args) {
            if (arg != nullptr) {
                argv.push_back(const_cast<char *>(arg));
            }
        }
        argv.push_back(nullptr);

        // Setup file actions for stdout/stderr redirection
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_t *actionsPtr = nullptr;
        std::vector<int> openedFds;

        if (opts.stdoutPath || opts.stderrPath) {
            posix_spawn_file_actions_init(&actions);
            actionsPtr = &actions;

            if (opts.stdoutPath) {
                int fd = open(opts.stdoutPath->c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
                if (fd != -1) {
                    posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
                    posix_spawn_file_actions_addclose(&actions, fd);
                    openedFds.push_back(fd);
                }
            }

            if (opts.stderrPath) {
                int fd = open(opts.stderrPath->c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
                if (fd != -1) {
                    posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
                    posix_spawn_file_actions_addclose(&actions, fd);
                    openedFds.push_back(fd);
                }
            }
        }

        // Spawn the process
        pid_t pid {};
        int rc = posix_spawn(&pid, program, actionsPtr, nullptr, argv.data(), envp.data());

        if (actionsPtr) {
            posix_spawn_file_actions_destroy(&actions);
        }

        if (rc != 0) {
            for (int fd: openedFds) {
                close(fd);
            }
            return false;
        }

        if (opts.waitForCompletion) {
            int status {};
            if (waitpid(pid, &status, 0) == -1) {
                return false;
            }

            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                return false;
            }
        }

        return true;
    }

    bool SpawnWithCustomHome(
        const char *program,
        const std::vector<const char *> &args,
        const std::string &customHome
    ) {

        SpawnOptions opts;
        opts.env = {
            {"HOME",              customHome},
            {"CFFIXED_USER_HOME", customHome}
        };
        opts.waitForCompletion = true;
        return SpawnProcessWithEnv(program, args, opts);
    }
#endif

} // namespace SystemInfo
