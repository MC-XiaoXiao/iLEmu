#include "foundation/device_model.hpp"
#include <jni.h>
#include <plist/plist.h>
#include <cstdlib>
#include <vector>

namespace {
jstring json(JNIEnv* env, plist_t value)
{
    char* text = nullptr;
    uint32_t size = 0;
    const auto result = plist_to_json(value, &text, &size, 0);
    jstring output = result == PLIST_ERR_SUCCESS && text
        ? env->NewStringUTF(text) : nullptr;
    std::free(text);
    plist_free(value);
    return output;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_xxiao_ilemu_firmware_FirmwareMetadata_readPlist(JNIEnv* env, jobject, jbyteArray data)
{
    const auto size = env->GetArrayLength(data);
    if (size <= 0 || size > 4 * 1024 * 1024)
        return nullptr;
    std::vector<char> bytes(static_cast<size_t>(size));
    env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    if (env->ExceptionCheck())
        return nullptr;
    plist_t value = nullptr;
    if (plist_from_memory(bytes.data(), size, &value, nullptr) != PLIST_ERR_SUCCESS)
        return nullptr;
    // Only expose firmware identity fields, not arbitrary binary plist data.
    auto identity = plist_new_dict();
    for (const auto* key : {"ProductVersion", "ProductBuildVersion", "ProductType", "SupportedProductTypes"}) {
        const auto node = plist_dict_get_item(value, key);
        if (node)
            plist_dict_set_item(identity, key, plist_copy(node));
    }
    auto files = plist_new_array();
    const auto builds = plist_dict_get_item(value, "BuildIdentities");
    for (uint32_t i = 0; i < plist_array_get_size(builds); ++i) {
        const auto build = plist_array_get_item(builds, i);
        const auto info = plist_dict_get_item(build, "Info");
        const auto manifest = plist_dict_get_item(build, "Manifest");
        const auto os = plist_dict_get_item(manifest, "OS");
        const auto os_info = plist_dict_get_item(os, "Info");
        const auto path = plist_dict_get_item(os_info, "Path");
        if (plist_get_node_type(path) == PLIST_STRING) {
            auto entry = plist_new_dict();
            plist_dict_set_item(entry, "path", plist_copy(path));
            const auto board = plist_dict_get_item(info, "DeviceClass");
            if (board) plist_dict_set_item(entry, "board", plist_copy(board));
            plist_array_append_item(files, entry);
        }
    }
    const auto restore = plist_dict_get_item(value, "SystemRestoreImages");
    const auto user = plist_dict_get_item(restore, "User");
    if (plist_get_node_type(user) == PLIST_STRING) {
        auto entry = plist_new_dict();
        plist_dict_set_item(entry, "path", plist_copy(user));
        plist_array_append_item(files, entry);
    }
    plist_dict_set_item(identity, "RootFilesystems", files);
    plist_free(value);
    return json(env, identity);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_xxiao_ilemu_firmware_FirmwareMetadata_deviceModels(JNIEnv* env, jobject)
{
    auto list = plist_new_array();
    for (const auto& model : ilemu::DeviceModel::available_models()) {
        auto entry = plist_new_dict();
        plist_dict_set_item(entry, "identifier", plist_new_string(model.identity.product_type.data()));
        plist_dict_set_item(entry, "board", plist_new_string(model.identity.board_config.data()));
        plist_dict_set_item(entry, "name", plist_new_string(model.screen.graphics_services.marketing_name.data()));
        plist_array_append_item(list, entry);
    }
    return json(env, list);
}
