// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "android_host.hpp"
#include "android_sensors.hpp"
#include "app/command_line.hpp"

#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <jni.h>
#include <string>
#include <unistd.h>

namespace {
std::atomic<bool> stop_requested { false };
std::atomic<bool> running { false };

bool send_command(const char* path, const char* command)
{
    const int fd = ::open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::string line { command };
    line += '\n';
    const auto count = ::write(fd, line.data(), line.size());
    ::close(fd);
    return count == static_cast<ssize_t>(line.size());
}
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_xxiao_ilemu_NativeBridge_prepare(
    JNIEnv*, jobject)
{
    if (running.load())
        return JNI_FALSE;
    stop_requested.store(false);
    ilemu::android_sensors()->reset();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_xxiao_ilemu_NativeBridge_command(
    JNIEnv* env, jobject, jstring path, jstring command, jboolean stop)
{
    if (stop)
        stop_requested.store(true);
    const char* file = env->GetStringUTFChars(path, nullptr);
    if (!file)
        return JNI_FALSE;
    const char* text = env->GetStringUTFChars(command, nullptr);
    const bool sent = text && send_command(file, text);
    if (text)
        env->ReleaseStringUTFChars(command, text);
    env->ReleaseStringUTFChars(path, file);
    return sent ? JNI_TRUE : JNI_FALSE;
}

extern "C" int SDL_main(int argc, char** argv)
{
    if (running.exchange(true))
        return 1;
    struct RunningGuard {
        ~RunningGuard() { running.store(false); }
    } guard;
    try {
        const auto* storage = SDL_AndroidGetInternalStoragePath();
        if (!storage)
            throw std::runtime_error("Android internal storage is unavailable");
        const std::filesystem::path files { storage };
        std::freopen((files / "native.log").c_str(), "w", stderr);
        std::freopen((files / "native.log").c_str(), "a", stdout);
        setvbuf(stderr, nullptr, _IOLBF, 0);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        // A background transition must allow the command loop to consume quit.
        SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        ilemu::AndroidHost host { files / "control.fifo", stop_requested };
        if (stop_requested.load())
            return 0;
        __android_log_print(ANDROID_LOG_INFO, "iLEmu",
            "Native command starting; host page size=%ld",
            sysconf(_SC_PAGESIZE));
        const auto result = ilemu::run_command_line(argc, argv, host);
        __android_log_print(
            ANDROID_LOG_INFO, "iLEmu", "Native command finished: %d", result);
        return result;
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_ERROR, "iLEmu", "%s", error.what());
        std::fprintf(stderr, "ilemu: %s\n", error.what());
        return 1;
    }
}
