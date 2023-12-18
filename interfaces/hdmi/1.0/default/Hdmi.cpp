// FIXME: your file license if you have one

#include "Hdmi.h"
#include "log/log.h"
#include <sys/inotify.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <math.h>
#include "HdmiCallback.h"
#include "HdmiAudioCallback.h"
#include <condition_variable>
#include <cutils/properties.h>


#include <RockchipRga.h>
#include <im2d_api/im2d.h>
#include "im2d_api/im2d.hpp"
#include "im2d_api/im2d_common.h"

#include <ui/Fence.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>

#define BASE_VIDIOC_PRIVATE 192     /* 192-255 are private */
#define RKMODULE_GET_HDMI_MODE       \
        _IOR('V', BASE_VIDIOC_PRIVATE + 34, __u32)


namespace rockchip::hardware::hdmi::implementation {

sp<::rockchip::hardware::hdmi::V1_0::IHdmiCallback> mCb = nullptr;
sp<::rockchip::hardware::hdmi::V1_0::IHdmiAudioCallback> mAudioCb = nullptr;
sp<::rockchip::hardware::hdmi::V1_0::IHdmiRxStatusCallback> mStatusCb = nullptr;
sp<::rockchip::hardware::hdmi::V1_0::IFrameWarpper> mFrameWarpper = nullptr;
enum CBType{
    HDMI= 100,
    AUDIO,
    STATUS,
    FRAME
};

std::mutex mLock;
std::mutex mLockAudio;
std::mutex mLockStatusCb;
std::mutex mLockFrameWarpper;

hidl_string mDeviceId;
#ifdef VIRTUAL_ENABLE
#define YUV_PATH "/vendor/etc/camera/%dx%d.yuv"
#define YUV_W 1920
#define YUV_H 1080
sp<GraphicBuffer> sYUVBuffer;
#endif
#define RGA_VIRTUAL_W (4096)
#define RGA_VIRTUAL_H (4096)

const int kMaxDevicePathLen = 256;
const char* kDevicePath = "/dev/";
const char kPrefix[] = "v4l-subdev";
const int kPrefixLen = sizeof(kPrefix) - 1;
const int kDevicePrefixLen = sizeof(kDevicePath) + kPrefixLen + 1;

char kV4l2DevicePath[kMaxDevicePathLen];
int mMipiHdmi = 0;

sp<V4L2DeviceEvent> mV4l2Event;
char* getMipiID(){
    char value[PROPERTY_VALUE_MAX]={0};
    property_get("persist.vendor.camera.mipi", value, "");
    ALOGD("%s %s",__FUNCTION__,value);
    return value;
}
int findMipiHdmi()
{
    DIR* devdir = opendir(kDevicePath);
    if(devdir == 0) {
        ALOGE("%s: cannot open %s! ", __FUNCTION__, kDevicePath);
        return -1;
    }
    struct dirent* de;
    int videofd,ret;
    while ((de = readdir(devdir)) != 0) {
        // Find external v4l devices that's existing before we start watching and add them
        if (!strncmp(kPrefix, de->d_name, kPrefixLen)) {
            std::string deviceId(de->d_name + kPrefixLen);
            ALOGD("found %s", de->d_name);
            char v4l2DeviceDriver[16];
            snprintf(kV4l2DevicePath, kMaxDevicePathLen,"%s%s", kDevicePath, de->d_name);
            videofd = open(kV4l2DevicePath, O_RDWR);
            if (videofd < 0){
                ALOGE("[%s %d] open device failed:%x [%s]", __FUNCTION__, __LINE__, videofd,strerror(errno));
                continue;
            } else {
                uint32_t ishdmi;
                ret = ::ioctl(videofd, RKMODULE_GET_HDMI_MODE, (void*)&ishdmi);
                if (ret < 0) {
                    ALOGE("RKMODULE_GET_HDMI_MODE Failed, error: %s", strerror(errno));
                    close(videofd);
                    continue;
                }
                ALOGD("%s RKMODULE_GET_HDMI_MODE:%d",kV4l2DevicePath,ishdmi);
                if (ishdmi)
                {
                    mMipiHdmi = videofd;
                    ALOGD("MipiHdmi fd:%d",mMipiHdmi);
                    if (mMipiHdmi < 0)
                    {
                        return ret;
                    }
                    mV4l2Event->initialize(mMipiHdmi);
                }
            }
        }
    }
    closedir(devdir);
    return ret;
}
sp<GraphicBuffer> GraphicBuffer_Init(int width, int height,int format) {
    sp<GraphicBuffer> gb(new GraphicBuffer(width,height,format,
                                           GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN));
    if (gb->initCheck()) {
        printf("GraphicBuffer check error : %s\n",strerror(errno));
        return NULL;
    } else
        printf("GraphicBuffer check %s \n","ok");

    return gb;
}
int rga_scale_crop_dstfd(
		int src_width, int src_height,
		sp<GraphicBuffer> src_buf, int src_format,buffer_handle_t dst_buf_handle,
		int dst_width, int dst_height,
		int zoom_val, bool mirror, bool isNeedCrop,
		bool isDstNV21, bool is16Align, bool isYuyvFormat)
{
    struct timespec last_tm;
    struct timespec curr_tm;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &last_tm);
    int ret = 0;
    rga_info_t src,dst;
    int zoom_cropW,zoom_cropH;
    int ratio = 0;
    int zoom_top_offset=0,zoom_left_offset=0;
    rga_buffer_handle_t src_handle;
    rga_buffer_handle_t dst_handle;

    RockchipRga& rkRga(RockchipRga::get());

    im_handle_param_t param;
    param.width = src_width;
    param.height = src_height;
    param.format = src_format;

    memset(&src, 0, sizeof(rga_info_t));
    int src_fd,dst_fd;
    ret = rkRga.RkRgaGetBufferFd(src_buf->handle, &src_fd);
    if (ret){
        ALOGE("%s: get buffer fd fail: %s, buffer_handle_t=%p",__FUNCTION__, strerror(errno), (void*)(src_buf->handle));
        return ret;
    }

    src.fd = src_fd;
    src_handle = importbuffer_fd(src_fd, &param);
    src.mmuFlag = ((2 & 0x3) << 4) | 1 | (1 << 8) | (1 << 10);
    memset(&dst, 0, sizeof(rga_info_t));

    ret = rkRga.RkRgaGetBufferFd(dst_buf_handle, &dst_fd);
    if (ret){
        ALOGE("%s: get buffer fd fail: %s, buffer_handle_t=%p",__FUNCTION__, strerror(errno), (void*)(src_buf->handle));
        return ret;
    }

    dst.fd = dst_fd;
    param.width = dst_width;
    param.height = dst_height;
    if (isDstNV21){
        param.format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
    }else{
        param.format = HAL_PIXEL_FORMAT_YCrCb_NV12;
    }

    dst_handle = importbuffer_fd(dst_fd, &param);
    //ALOGD("@%s, dst fd:%d,width:%d,height:%d,isDstNV21:%d",__FUNCTION__,dst.fd,param.width,param.height,isDstNV21);
    dst.mmuFlag = ((2 & 0x3) << 4) | 1 | (1 << 8) | (1 << 10);

    if((dst_width > RGA_VIRTUAL_W) || (dst_height > RGA_VIRTUAL_H)){
        ALOGE("(dst_width > RGA_VIRTUAL_W) || (dst_height > RGA_VIRTUAL_H), switch to arm ");
        ret = -1;
        goto END;
    }

    //need crop ? when cts FOV,don't crop
    if(isNeedCrop && (src_width*100/src_height) != (dst_width*100/dst_height)) {
        ratio = ((src_width*100/dst_width) >= (src_height*100/dst_height))?
                (src_height*100/dst_height):
                (src_width*100/dst_width);
        zoom_cropW = (ratio*dst_width/100) & (~0x01);
        zoom_cropH = (ratio*dst_height/100) & (~0x01);
        zoom_left_offset=((src_width-zoom_cropW)>>1) & (~0x01);
        zoom_top_offset=((src_height-zoom_cropH)>>1) & (~0x01);
    }else{
        zoom_cropW = src_width;
        zoom_cropH = src_height;
        zoom_left_offset=0;
        zoom_top_offset=0;
    }

    if(zoom_val > 100){
        zoom_cropW = zoom_cropW*100/zoom_val & (~0x01);
        zoom_cropH = zoom_cropH*100/zoom_val & (~0x01);
        zoom_left_offset = ((src_width-zoom_cropW)>>1) & (~0x01);
        zoom_top_offset= ((src_height-zoom_cropH)>>1) & (~0x01);
    }

    //usb camera height align to 16,the extra eight rows need to be croped.
    if(!is16Align){
        zoom_top_offset = zoom_top_offset & (~0x07);
    }

    rga_set_rect(&src.rect, zoom_left_offset, zoom_top_offset,
                zoom_cropW, zoom_cropH, src_width,
                src_height, src_format);
    if (isDstNV21)
        rga_set_rect(&dst.rect, 0, 0, dst_width, dst_height,
                    dst_width, dst_height,
                    HAL_PIXEL_FORMAT_YCrCb_420_SP);
    else
        rga_set_rect(&dst.rect, 0,0,dst_width,dst_height,
                    dst_width,dst_height,
                    HAL_PIXEL_FORMAT_YCrCb_NV12);

    if (mirror)
        src.rotation = DRM_RGA_TRANSFORM_FLIP_V;
    //TODO:sina,cosa,scale_mode,render_mode

    src.handle = src_handle;
    src.fd = 0;
    dst.handle = dst_handle;
    dst.fd = 0;
    dst.core = 0x03;
    ret = rkRga.RkRgaBlit(&src, &dst, NULL);
    if (ret) {
        ALOGE("%s:rga blit failed %s", __FUNCTION__, imStrError((IM_STATUS)ret));
        goto END;
    }

    END:
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
    clock_gettime(CLOCK_MONOTONIC_COARSE, &curr_tm);
    //LOGD("%s use %ldms", __FUNCTION__, get_time_diff_ms(&last_tm,&curr_tm));

    return ret;
}

Return<void> Hdmi::foundHdmiDevice(const hidl_string& deviceId, const ::android::sp<::rockchip::hardware::hdmi::V1_0::IHdmiRxStatusCallback>& cb) {

    ALOGD("@%s,deviceId:%s",__FUNCTION__,deviceId.c_str());
    if (cb == nullptr ||cb.get() == nullptr)
    {
       return Void();
    }
    std::unique_lock<std::mutex> lk(mLockStatusCb);
    mDeviceId = deviceId.c_str();
    if (mStatusCb != nullptr)
    {
        mStatusCb->unlinkToDeath(this);
    }
    mStatusCb = cb;
    mStatusCb->linkToDeath(this, STATUS);
    lk.unlock();
    return Void();
}

Return<void> Hdmi::addAudioListener(const ::android::sp<::rockchip::hardware::hdmi::V1_0::IHdmiAudioCallback>& cb) {
    ALOGD("@%s",__FUNCTION__);
    if (cb == nullptr ||cb.get() == nullptr)
    {
       return Void();
    }
    std::unique_lock<std::mutex> lk(mLockAudio);
    if (mAudioCb != nullptr)
    {
        mAudioCb->unlinkToDeath(this);
    }
    mAudioCb = cb;
    mAudioCb->linkToDeath(this, AUDIO);
    lk.unlock();
    return Void();
}
Return<void> Hdmi::removeAudioListener(const ::android::sp<::rockchip::hardware::hdmi::V1_0::IHdmiAudioCallback>& cb)
{
    ALOGD("@%s",__FUNCTION__);
    std::unique_lock<std::mutex> lk(mLockAudio);
    mAudioCb = nullptr;
    lk.unlock();
    return Void();
}
Return<void> Hdmi::onAudioChange(const ::rockchip::hardware::hdmi::V1_0::HdmiAudioStatus& status) {
    ALOGD("@%s",__FUNCTION__);
    std::unique_lock<std::mutex> lk(mLockAudio);
    char* mipiid = getMipiID();
    if (mAudioCb.get()!=nullptr && ( strstr(status.deviceId.c_str(),mDeviceId.c_str())
        || (strlen(mipiid) > 0 && (status.deviceId.c_str(),mipiid))))
    {
        ALOGD("@%s,cameraId:%s status:%d",__FUNCTION__,status.deviceId.c_str(),status.status);
        if (status.status)
        {
            mAudioCb->onConnect(status.deviceId);
        }else{
            mAudioCb->onDisconnect(status.deviceId);
        }
    }
    lk.unlock();
    return Void();
}
Return<void> Hdmi::getHdmiDeviceId(getHdmiDeviceId_cb _hidl_cb) {
    ALOGD("@%s,mDeviceId:%s",__FUNCTION__,mDeviceId.c_str());
    _hidl_cb(mDeviceId);
    return Void();
}
Return<void> Hdmi::getMipiStatus(Hdmi::getMipiStatus_cb _hidl_cb){
    ALOGD("@%s",__FUNCTION__);
    V1_0::HdmiStatus status;
    struct v4l2_subdev_format aFormat;
    int err = ioctl(mMipiHdmi, VIDIOC_SUBDEV_G_FMT, &aFormat);
    if (err < 0) {
        ALOGE("VIDIOC_SUBDEV_G_FMT failed: %s", strerror(errno));
        _hidl_cb(status);
        return Void();
    }
    ALOGD("VIDIOC_SUBDEV_G_FMT: pad: %d, which: %d, width: %d, "
    "height: %d, format: 0x%x, field: %d, color space: %d",
    aFormat.pad,
    aFormat.which,
    aFormat.format.width,
    aFormat.format.height,
    aFormat.format.code,
    aFormat.format.field,
    aFormat.format.colorspace);
    status.width = aFormat.format.width;
    status.height = aFormat.format.height;
    struct v4l2_dv_timings timings;
    err = ioctl(mMipiHdmi, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &timings);
    if (err < 0) {
        ALOGD("get VIDIOC_SUBDEV_QUERY_DV_TIMINGS failed ,%d(%s)", errno, strerror(errno));
        _hidl_cb(status);
        return Void();
    }
    const struct v4l2_bt_timings *bt =&timings.bt;
    double tot_width, tot_height;
    tot_height = bt->height +
        bt->vfrontporch + bt->vsync + bt->vbackporch +
        bt->il_vfrontporch + bt->il_vsync + bt->il_vbackporch;
    tot_width = bt->width +
        bt->hfrontporch + bt->hsync + bt->hbackporch;
    ALOGD("%s:%dx%d, pixelclock:%lld Hz, %.2f fps", __func__,
    timings.bt.width, timings.bt.height,
    timings.bt.pixelclock,static_cast<double>(bt->pixelclock) /(tot_width * tot_height));
    status.fps = round(static_cast<double>(bt->pixelclock) /(tot_width * tot_height));
    struct v4l2_control control;
    memset(&control, 0, sizeof(struct v4l2_control));
    control.id = V4L2_CID_DV_RX_POWER_PRESENT;
    err = ioctl(mMipiHdmi, VIDIOC_G_CTRL, &control);
    if (err < 0) {
        ALOGE("V4L2_CID_DV_RX_POWER_PRESENT failed ,%d(%s)", errno, strerror(errno));
    }
    ALOGD("VIDIOC_G_CTRL:%d",control.value);
    status.status = control.value;
    _hidl_cb(status);
    return Void();
}

Return<void> Hdmi::getHdmiRxStatus(Hdmi::getHdmiRxStatus_cb _hidl_cb){
    ALOGD("@%s",__FUNCTION__);
    std::unique_lock<std::mutex> lk(mLockStatusCb);
    V1_0::HdmiStatus status;
    if (mStatusCb)
    {
        mStatusCb->getHdmiRxStatus(_hidl_cb);
        lk.unlock();
        return Void();
    }
    _hidl_cb(status);
    lk.unlock();
    return Void();
}
// Methods from ::rockchip::hardware::hdmi::V1_0::IHdmi follow.
Return<void> Hdmi::onStatusChange(uint32_t status) {
    ALOGD("@%s",__FUNCTION__);
    std::unique_lock<std::mutex> lk(mLock);
    if (mCb.get()!=nullptr)
    {
        ALOGD("@%s,status:%d",__FUNCTION__,status);
        if (status)
        {
            mCb->onConnect(mDeviceId);
        }else{
            mCb->onDisconnect(mDeviceId);
        }
    }
    lk.unlock();
    return Void();
}

Return<void> Hdmi::registerListener(const sp<::rockchip::hardware::hdmi::V1_0::IHdmiCallback>& cb) {
    ALOGD("@%s",__FUNCTION__);
    if (cb == nullptr ||cb.get() == nullptr)
    {
       return Void();
    }
    std::unique_lock<std::mutex> lk(mLock);
    if (mCb != nullptr)
    {
        mCb->unlinkToDeath(this);
    }
    mCb = cb;
    cb->linkToDeath(this, HDMI);
    lk.unlock();
    return Void();
}

Return<void> Hdmi::unregisterListener(const sp<::rockchip::hardware::hdmi::V1_0::IHdmiCallback>& cb) {
    ALOGD("@%s",__FUNCTION__);
    std::unique_lock<std::mutex> lk(mLock);
    mCb = nullptr;
    lk.unlock();
    return Void();
}

V4L2EventCallBack Hdmi::eventCallback(void* sender,int event_type,struct v4l2_event *event){
    ALOGD("@%s,event_type:%d",__FUNCTION__,event_type);
    std::unique_lock<std::mutex> lk(mLock);
    if (event_type == V4L2_EVENT_CTRL)
    {
        struct v4l2_event_ctrl* ctrl =(struct v4l2_event_ctrl*) &(event->u);
        if (mCb != nullptr)
        {
            if (!ctrl->value)
            {
                mCb->onDisconnect(getMipiID());
            }else{
                mCb->onConnect(getMipiID());
            }
        }
        ALOGD("V4L2_EVENT_CTRL event %d\n", ctrl->value);
    }else if (event_type == V4L2_EVENT_SOURCE_CHANGE)
    {
        if (sender!=nullptr)
        {
            V4L2DeviceEvent::V4L2EventThread* eventThread = (V4L2DeviceEvent::V4L2EventThread*)sender;
            sp<V4L2DeviceEvent::FormartSize> format = eventThread->getFormat();
            if (format!=nullptr)
            {
                ALOGD("getFormatWeight:%d,getFormatHeight:%d",format->getFormatWeight(),format->getFormatHeight());
                if (mCb != nullptr)
                {
                    mCb->onFormatChange(getMipiID(),format->getFormatWeight(),format->getFormatHeight());
                    mCb->onConnect(getMipiID());
                }
            }
        }
    }
    lk.unlock();
    return 0;
}

Hdmi::Hdmi(){
    ALOGD("@%s.",__FUNCTION__);
#ifdef VIRTUAL_ENABLE
    if(sYUVBuffer == nullptr){
        int width,height;
        width = YUV_W;
        height = YUV_H;
        sYUVBuffer = GraphicBuffer_Init(width, height, HAL_PIXEL_FORMAT_YCrCb_420_SP);
        char* outbuf = NULL;
        if (sYUVBuffer != NULL) {
            int ret = sYUVBuffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN, (void**)&outbuf);
            {
                FILE* fp =NULL;
                char filename[128];
                filename[0] = 0x00;
                sprintf(filename, YUV_PATH,
                    width, height);
                fp = fopen(filename, "r");
                if (fp != NULL) {
                    int size = fread((char*)outbuf,1,width*height*1.5,fp);
                    fclose(fp);
                    ALOGD("read success yuv data to %s size:%d",filename, size);
                } else {
                    ALOGE("Create %s failed(%d, %s)",filename,fp, strerror(errno));
                }
            }
            ret = sYUVBuffer->unlock();
        }
    }
#endif
    mCb = new HdmiCallback();
    mV4l2Event = new V4L2DeviceEvent();
    mV4l2Event->RegisterEventvCallBack((V4L2EventCallBack)Hdmi::eventCallback);
    findMipiHdmi();
}
Hdmi::~Hdmi(){
    ALOGD("@%s",__FUNCTION__);
    if (mV4l2Event)
        mV4l2Event->closePipe();
    if (mV4l2Event)
        mV4l2Event->closeEventThread();
}
V1_0::IHdmi* HIDL_FETCH_IHdmi(const char* /* name */) {
    ALOGD("@%s",__FUNCTION__);
    return new Hdmi();
}

Return<void> Hdmi::setFrameDecorator(const sp<::rockchip::hardware::hdmi::V1_0::IFrameWarpper>& frameWarpper) {
    ALOGD("@%s",__FUNCTION__);
    if (frameWarpper == nullptr ||frameWarpper.get() == nullptr)
    {
       return Void();
    }
    std::unique_lock<std::mutex> lk(mLockFrameWarpper);
    if (mFrameWarpper != nullptr)
    {
        mFrameWarpper->unlinkToDeath(this);
    }
    mFrameWarpper = frameWarpper;
    mFrameWarpper->linkToDeath(this, STATUS);
    lk.unlock();
    return Void();
}

Return<void> Hdmi::decoratorFrame(const ::rockchip::hardware::hdmi::V1_0::FrameInfo& frameInfo, decoratorFrame_cb _hidl_cb) {
    ALOGV("@%s",__FUNCTION__);
    std::unique_lock<std::mutex> lk(mLockFrameWarpper);

    rockchip::hardware::hdmi::V1_0::FrameInfo _frameInfo;
    if (mFrameWarpper.get()!=nullptr)
    {
        V1_0::IFrameWarpper::onFrame_cb _onFrame_cb;
        mFrameWarpper->onFrame(frameInfo,[&]( ::rockchip::hardware::hdmi::V1_0::FrameInfo frameInfo){
            ALOGV("[%s] Receive wrapped frame(%d,%d)",__FUNCTION__,frameInfo.width,frameInfo.height);
            _frameInfo = frameInfo;
        });
        ALOGV("[%s] Receive wrapped frame(%d,%d)",__FUNCTION__,_frameInfo.width,_frameInfo.height);
        _hidl_cb(_frameInfo);
        lk.unlock();
        return Void();
    }else{
#ifdef VIRTUAL_ENABLE
        android::GraphicBufferMapper &mapper = android::GraphicBufferMapper::get();
        buffer_handle_t handle = nullptr;
        android::Rect bounds(android::ui::Size(frameInfo.width, frameInfo.height));
        android::status_t err = mapper.importBuffer(
                           frameInfo.buffer.getNativeHandle(), frameInfo.width, frameInfo.height, 1u,
                           HAL_PIXEL_FORMAT_YCrCb_NV12,
                            static_cast<uint64_t>(frameInfo.usage), frameInfo.stride, &handle);
        if(err != android::NO_ERROR){
            ALOGE("@%s(%d) importBuffer error",__FUNCTION__,__LINE__);
        }
        int deviceId = atoi(frameInfo.deviceId.c_str());
        ALOGV("@%s(%d,%d) usage:%" PRIx64 " frameId:%d deviceId:%d",__FUNCTION__,(int)frameInfo.width,(int)frameInfo.height,static_cast<uint64_t>(frameInfo.usage)
        , (int)frameInfo.frameId,deviceId);


        if(sYUVBuffer != nullptr && handle !=nullptr){
            bool mirror = false;
            bool isNeedCrop = true;
            bool isDstNV21 = false;
            bool is16Align = true;
            bool isYuyvFormat = true;

            rga_scale_crop_dstfd(YUV_W,YUV_H,sYUVBuffer,HAL_PIXEL_FORMAT_YCrCb_NV12,
                handle,frameInfo.width,frameInfo.height,100,mirror,isNeedCrop,isDstNV21,is16Align,isYuyvFormat);
            mapper.freeBuffer(handle);
        }else{
            ALOGE("sYUVBuffer:%x",(void*)sYUVBuffer.get());
            ALOGE("handle:%x",(void*)handle);
        }
#endif
    }
    _hidl_cb(frameInfo);
    lk.unlock();
    return Void();
}
void Hdmi::serviceDied(uint64_t cookie,
                     const android::wp<::android::hidl::base::V1_0::IBase>&  who){
    ALOGD("%s cookie:%d",__FUNCTION__,cookie);
    switch (cookie)
    {
        case HDMI:
            {
                std::lock_guard<std::mutex> lk(mLock);
                mCb= nullptr;
            }
            break;
        case AUDIO:
            {
                std::lock_guard<std::mutex> lk(mLock);
                mAudioCb= nullptr;
            }
            break;
        case STATUS:
            {
                std::lock_guard<std::mutex> lk(mLock);
                mStatusCb= nullptr;
            }
            break;
        case FRAME:
            {
                std::lock_guard<std::mutex> flk(mLockFrameWarpper);
                mFrameWarpper= nullptr;
            }
            break;
        default:
                ALOGE("%s invalid cookie:%d",__FUNCTION__,cookie);
            break;
    }
}

}  // namespace rockchip::hardware::hdmi::implementation
