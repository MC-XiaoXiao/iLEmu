// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "host/firmware_storage.hpp"
#include <algorithm>
#include <jni.h>
#include <stdexcept>
#include <string>

namespace {
class String {
public:
    String(JNIEnv* env, jstring text)
        : env_ { env }
        , text_ { text }
        , bytes_ { env->GetStringUTFChars(text, nullptr) }
    {
        if (!bytes_)
            throw std::runtime_error("cannot read firmware path");
    }
    ~String() { env_->ReleaseStringUTFChars(text_, bytes_); }
    const char* get() const { return bytes_; }

private:
    JNIEnv* env_;
    jstring text_;
    const char* bytes_;
};

template <class Function> void invoke(JNIEnv* env, Function&& function)
{
    try {
        function();
    } catch (const std::exception& error) {
        // Keep Java I/O and cancellation exceptions intact through libarchive.
        if (!env->ExceptionCheck())
            env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    }
}

class Stream {
public:
    Stream(JNIEnv* env, jobject stream, bool read)
        : env_ { env }
        , stream_ { stream }
    {
        auto type = env->GetObjectClass(stream);
        method_ = env->GetMethodID(
            type, read ? "read" : "write", read ? "([BII)I" : "([BII)V");
        env->DeleteLocalRef(type);
        bytes_ = env->NewByteArray(capacity);
        if (env->ExceptionCheck() || !method_ || !bytes_)
            throw std::runtime_error("cannot open firmware stream");
    }
    ~Stream() { env_->DeleteLocalRef(bytes_); }
    std::size_t read(void* data, std::size_t size)
    {
        const auto count = env_->CallIntMethod(stream_, method_, bytes_, 0,
            static_cast<jint>(
                std::min(size, static_cast<std::size_t>(capacity))));
        if (env_->ExceptionCheck())
            throw std::runtime_error("firmware stream read failed");
        if (count == 0)
            throw std::runtime_error("firmware input stream returned no data");
        if (count < 0)
            return 0;
        env_->GetByteArrayRegion(bytes_, 0, count, static_cast<jbyte*>(data));
        if (env_->ExceptionCheck())
            throw std::runtime_error("firmware stream copy failed");
        return static_cast<std::size_t>(count);
    }
    void write(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const jbyte*>(data);
        while (size != 0) {
            const auto count = static_cast<jint>(
                std::min(size, static_cast<std::size_t>(capacity)));
            env_->SetByteArrayRegion(bytes_, 0, count, bytes);
            if (!env_->ExceptionCheck())
                env_->CallVoidMethod(stream_, method_, bytes_, 0, count);
            if (env_->ExceptionCheck())
                throw std::runtime_error("firmware stream write failed");
            bytes += count;
            size -= static_cast<std::size_t>(count);
        }
    }

private:
    static constexpr jint capacity = 64 * 1024;
    JNIEnv* env_;
    jobject stream_;
    jmethodID method_ { };
    jbyteArray bytes_ { };
};
}

extern "C" JNIEXPORT void JNICALL
Java_com_xxiao_ilemu_containers_FirmwareFiles_prepare(
    JNIEnv* env, jobject, jstring root)
{
    invoke(env,
        [&] { ilemu::FirmwareStorage::prepare(String { env, root }.get()); });
}
extern "C" JNIEXPORT void JNICALL
Java_com_xxiao_ilemu_containers_FirmwareFiles_remove(
    JNIEnv* env, jobject, jstring root)
{
    invoke(env,
        [&] { ilemu::FirmwareStorage::remove(String { env, root }.get()); });
}
extern "C" JNIEXPORT void JNICALL
Java_com_xxiao_ilemu_containers_FirmwareFiles_copy(
    JNIEnv* env, jobject, jstring source, jstring destination)
{
    invoke(env, [&] {
        ilemu::FirmwareStorage::copy(
            String { env, source }.get(), String { env, destination }.get());
    });
}
extern "C" JNIEXPORT void JNICALL
Java_com_xxiao_ilemu_containers_FirmwareFiles_unpack(
    JNIEnv* env, jobject, jobject stream, jstring root)
{
    invoke(env, [&] {
        Stream input { env, stream, true };
        ilemu::FirmwareArchive::unpack(
            [&](void* data, std::size_t size) {
                return input.read(data, size);
            },
            String { env, root }.get());
    });
}
extern "C" JNIEXPORT void JNICALL
Java_com_xxiao_ilemu_containers_FirmwareFiles_pack(
    JNIEnv* env, jobject, jstring root, jobject stream)
{
    invoke(env, [&] {
        Stream output { env, stream, false };
        ilemu::FirmwareArchive::pack(String { env, root }.get(),
            [&](const void* data, std::size_t size) {
                output.write(data, size);
            });
    });
}
