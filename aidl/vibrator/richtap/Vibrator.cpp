/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Vibrator.h"

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <cutils/properties.h>
#include <inttypes.h>
#include <log/log.h>

#include <algorithm>
#include <thread>

#include "aac_vibra_function.h"

#define RICHTAP_LIGHT_STRENGTH 69
#define RICHTAP_MEDIUM_STRENGTH 89
#define RICHTAP_STRONG_STRENGTH 99

/*
 * The AAC library plays each prebaked effect's one waveform whatever strength it is
 * given, and sets the driver's gain to full just before. The driver drives the motor
 * at richtap_fifo_vmax times that gain, so scale richtap_fifo_vmax with the strength
 * instead: LIGHT at half, as the QTI HAL's magnitudes have it, and MEDIUM, the default
 * for ring, notification, alarm and media, close to STRONG, since salami's short clicks
 * felt weak at 75%.
 */
#define RICHTAP_FIFO_VMAX_PATH "/sys/class/qcom-haptics/richtap_fifo_vmax"
#define FIFO_VMAX_PATH "/sys/class/qcom-haptics/fifo_vmax"
#define RICHTAP_LIGHT_VMAX_PERCENT 50
#define RICHTAP_MEDIUM_VMAX_PERCENT 90
#define RICHTAP_FULL_VMAX_PERCENT 100

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static uint32_t readVmaxMv(const char* path) {
    std::string value;
    uint32_t vmaxMv;

    if (!::android::base::ReadFileToString(path, &value) ||
        !::android::base::ParseUint(::android::base::Trim(value), &vmaxMv))
        return 0;

    return vmaxMv;
}

Vibrator::Vibrator() {
    uint32_t deviceType = 0;

    int32_t ret = aac_vibra_init(&deviceType);
    if (ret) {
        ALOGE("AAC init failed: %d\n", ret);
        return;
    }

    aac_vibra_looper_start();

    // richtap_fifo_vmax starts at the device tree's RichTap vmax, or else at its FIFO
    // vmax, which nothing changes. Take the larger: if this service restarts while a
    // lighter strength is set, that must not become full.
    mFullVmaxMv = std::max(readVmaxMv(RICHTAP_FIFO_VMAX_PATH), readVmaxMv(FIFO_VMAX_PATH));
    if (!mFullVmaxMv) ALOGE("Can't read the RichTap vmax: effects play at one strength");
    setVmaxPercent(RICHTAP_FULL_VMAX_PERCENT);

    ALOGI("AAC init success: %u\n", deviceType);
}

void Vibrator::setVmaxPercent(uint32_t percent) {
    uint32_t vmaxMv = mFullVmaxMv * percent / 100;

    if (!vmaxMv || vmaxMv == mVmaxMv) return;

    if (!::android::base::WriteStringToFile(std::to_string(vmaxMv), RICHTAP_FIFO_VMAX_PATH)) {
        ALOGE("Failed to set the RichTap vmax to %u mV", vmaxMv);
        return;
    }

    mVmaxMv = vmaxMv;
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    int32_t ret = aac_vibra_off();
    if (ret) {
        ALOGE("AAC off failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    // The framework scales these through setAmplitude(); don't let the vmax of a
    // lighter prebaked effect shrink them as well.
    setVmaxPercent(RICHTAP_FULL_VMAX_PERCENT);
    int32_t ret = aac_vibra_looper_on(timeoutMs);
    if (ret < 0) {
        ALOGE("AAC on failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([=] {
            usleep(ret * 1000);
            callback->onComplete();
        }).detach();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength es,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    int32_t strength;
    uint32_t vmaxPercent;

    if (effect < Effect::CLICK || effect > Effect::HEAVY_CLICK)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    switch (es) {
        case EffectStrength::LIGHT:
            strength = RICHTAP_LIGHT_STRENGTH;
            vmaxPercent = RICHTAP_LIGHT_VMAX_PERCENT;
            break;
        case EffectStrength::MEDIUM:
            strength = RICHTAP_MEDIUM_STRENGTH;
            vmaxPercent = RICHTAP_MEDIUM_VMAX_PERCENT;
            break;
        case EffectStrength::STRONG:
            strength = RICHTAP_STRONG_STRENGTH;
            vmaxPercent = RICHTAP_FULL_VMAX_PERCENT;
            break;
        default:
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    setVmaxPercent(vmaxPercent);
    int32_t ret = aac_vibra_looper_prebaked_effect(static_cast<uint32_t>(effect), strength);
    if (ret < 0) {
        ALOGE("AAC perform failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    if (callback != nullptr) {
        std::thread([=] {
            usleep(ret * 1000);
            callback->onComplete();
        }).detach();
    }

    *_aidl_return = ret;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK,
                     Effect::THUD,  Effect::POP,          Effect::HEAVY_CLICK};

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    uint8_t tmp = (uint8_t)(amplitude * 0xff);

    setVmaxPercent(RICHTAP_FULL_VMAX_PERCENT);

    int32_t ret = aac_vibra_setAmplitude(tmp);
    if (ret) {
        ALOGE("AAC set amplitude failed: %d\n", ret);
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(
        std::vector<CompositePrimitive>* supported __unused) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive __unused,
                                                  int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite __unused,
                                     const std::shared_ptr<IVibratorCallback>& callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(
        std::vector<Effect>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t id __unused, Effect effect __unused,
                                            EffectStrength strength __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t id __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* freqResolutionHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* freqMinimumHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& composite __unused,
                                         const std::shared_ptr<IVibratorCallback>& callback
                                                 __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
