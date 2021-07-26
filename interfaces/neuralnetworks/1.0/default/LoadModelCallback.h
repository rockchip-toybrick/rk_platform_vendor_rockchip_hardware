// FIXME: your file license if you have one

#pragma once

#include <rockchip/hardware/neuralnetworks/1.0/ILoadModelCallback.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>

namespace rockchip::hardware::neuralnetworks::implementation {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

struct LoadModelCallback : public V1_0::ILoadModelCallback {
    // Methods from ::rockchip::hardware::neuralnetworks::V1_0::ILoadModelCallback follow.
    Return<void> notify(::rockchip::hardware::neuralnetworks::V1_0::ErrorStatus status, const ::rockchip::hardware::neuralnetworks::V1_0::RKNNDeviceID& pdevs) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.

};

// FIXME: most likely delete, this is only for passthrough implementations
// extern "C" ILoadModelCallback* HIDL_FETCH_ILoadModelCallback(const char* name);

}  // namespace rockchip::hardware::neuralnetworks::implementation
