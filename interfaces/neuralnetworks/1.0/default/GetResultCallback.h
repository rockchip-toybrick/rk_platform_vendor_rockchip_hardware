// FIXME: your file license if you have one

#pragma once

#include <rockchip/hardware/neuralnetworks/1.0/IGetResultCallback.h>
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

struct GetResultCallback : public V1_0::IGetResultCallback {
    // Methods from ::rockchip::hardware::neuralnetworks::V1_0::IGetResultCallback follow.
    Return<void> notify(::rockchip::hardware::neuralnetworks::V1_0::ErrorStatus status, const ::rockchip::hardware::neuralnetworks::V1_0::Response& response) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.

};

// FIXME: most likely delete, this is only for passthrough implementations
// extern "C" IGetResultCallback* HIDL_FETCH_IGetResultCallback(const char* name);

}  // namespace rockchip::hardware::neuralnetworks::implementation
