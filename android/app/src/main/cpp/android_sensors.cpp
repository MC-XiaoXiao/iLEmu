// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "android_sensors.hpp"
#include <jni.h>

namespace ilemu {
std::shared_ptr<SensorInput> android_sensors()
{
    static auto input = std::make_shared<SensorInput>();
    return input;
}
}

extern "C" JNIEXPORT void JNICALL Java_com_xxiao_ilemu_HostSensors_available(
    JNIEnv*, jobject, jint kind, jboolean available)
{
    const auto input = ilemu::android_sensors();
    if (kind == 3)
        input->set_location_available(available);
    else if (kind >= 0 && kind < 3)
        input->set_available(static_cast<ilemu::MotionSensor>(kind), available);
}

extern "C" JNIEXPORT void JNICALL Java_com_xxiao_ilemu_HostSensors_motion(
    JNIEnv*, jobject, jint kind, jfloat x, jfloat y, jfloat z, jlong timestamp,
    jint accuracy)
{
    if (kind < 0 || kind >= 3 || timestamp <= 0)
        return;
    ilemu::android_sensors()->publish(static_cast<ilemu::MotionSensor>(kind),
        { { x, y, z }, static_cast<std::uint64_t>(timestamp), accuracy });
}

extern "C" JNIEXPORT void JNICALL Java_com_xxiao_ilemu_HostSensors_location(
    JNIEnv*, jobject, jdouble latitude, jdouble longitude, jdouble accuracy,
    jdouble unix_time, jlong timestamp, jint fields, jdouble altitude,
    jdouble vertical_accuracy, jdouble speed, jdouble bearing)
{
    if (timestamp <= 0)
        return;
    ilemu::LocationSample sample;
    sample.latitude = latitude;
    sample.longitude = longitude;
    sample.horizontal_accuracy = accuracy;
    sample.unix_time_seconds = unix_time;
    sample.timestamp_ns = static_cast<std::uint64_t>(timestamp);
    if (fields & 1)
        sample.altitude = altitude;
    if (fields & 2)
        sample.vertical_accuracy = vertical_accuracy;
    if (fields & 4)
        sample.speed = speed;
    if (fields & 8)
        sample.bearing = bearing;
    ilemu::android_sensors()->publish(sample);
}
