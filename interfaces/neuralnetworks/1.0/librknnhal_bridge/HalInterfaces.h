#ifndef ROCKCHIP_FRAMEWORKS_NN_HAL_INTERFACES_H
#define ROCKCHIP_FRAMEWORKS_NN_HAL_INTERFACES_H

#include <rockchip/hardware/neuralnetworks/1.0/IRKNeuralnetworks.h>
#include <android/hidl/allocator/1.0/IAllocator.h>
#include <android/hidl/memory/1.0/IMemory.h>
#include <hidlmemory/mapping.h>
#include <sys/cdefs.h>
__BEGIN_DECLS
#define LOG_TAG "neuralnetworks@ndk_bridge"

namespace rockchip::nn::hal {
    using android::sp;
    using ::android::hardware::hidl_memory;
    using ::android::hardware::Return;
    using ::android::hardware::Void;
    using ::android::hidl::memory::V1_0::IMemory;
    using ::android::hidl::allocator::V1_0::IAllocator;

    namespace V1_0 = rockchip::hardware::neuralnetworks::V1_0;
}
__END_DECLS
#endif
