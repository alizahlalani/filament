/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <backend/AcquiredImage.h>
#include <backend/Platform.h>
#include <backend/platforms/PlatformEGL.h>
#include <backend/platforms/PlatformEGLAndroid.h>

#include <private/backend/VirtualMachineEnv.h>

#include "opengl/GLUtils.h"
#include "ExternalStreamManagerAndroid.h"

#include <android/api-level.h>
#include <android/hardware_buffer.h>

#include <utils/android/PerformanceHintManager.h>

#include <utils/compiler.h>
#include <utils/ostream.h>
#include <utils/Log.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <sys/system_properties.h>

#include <jni.h>

#include <chrono>
#include <new>

#include <unistd.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// We require filament to be built with an API 19 toolchain, before that, OpenGLES 3.0 didn't exist
// Actually, OpenGL ES 3.0 was added to API 18, but API 19 is the better target and
// the minimum for Jetpack at the time of this comment.
#if __ANDROID_API__ < 26
#   error "__ANDROID_API__ must be at least 26"
#endif

using namespace utils;

namespace filament::backend {
using namespace backend;

// The Android NDK doesn't expose extensions, fake it with eglGetProcAddress
namespace glext {

extern PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
extern PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
extern PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;
extern PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

UTILS_PRIVATE PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID = {};
UTILS_PRIVATE PFNEGLPRESENTATIONTIMEANDROIDPROC eglPresentationTimeANDROID = {};
UTILS_PRIVATE PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC eglGetCompositorTimingSupportedANDROID = {};
UTILS_PRIVATE PFNEGLGETCOMPOSITORTIMINGANDROIDPROC eglGetCompositorTimingANDROID = {};
UTILS_PRIVATE PFNEGLGETNEXTFRAMEIDANDROIDPROC eglGetNextFrameIdANDROID = {};
UTILS_PRIVATE PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC eglGetFrameTimestampSupportedANDROID = {};
UTILS_PRIVATE PFNEGLGETFRAMETIMESTAMPSANDROIDPROC eglGetFrameTimestampsANDROID = {};
}
using namespace glext;

using EGLStream = Platform::Stream;

// ---------------------------------------------------------------------------------------------

PlatformEGLAndroid::InitializeJvmForPerformanceManagerIfNeeded::InitializeJvmForPerformanceManagerIfNeeded() {
    // PerformanceHintManager() needs the calling thread to be a Java thread; so we need
    // to attach this thread to the JVM before we initialize PerformanceHintManager.
    // This should be done in PerformanceHintManager(), but libutils doesn't have access to
    // VirtualMachineEnv.
    if (PerformanceHintManager::isSupported()) {
        (void)VirtualMachineEnv::get().getEnvironment();
    }
}

// ---------------------------------------------------------------------------------------------

PlatformEGLAndroid::PlatformEGLAndroid() noexcept
        : PlatformEGL(),
          mExternalStreamManager(ExternalStreamManagerAndroid::create()),
          mInitializeJvmForPerformanceManagerIfNeeded(),
          mPerformanceHintManager() {

    char scratch[PROP_VALUE_MAX + 1];
    int length = __system_property_get("ro.build.version.release", scratch);
    int const androidVersion = length >= 0 ? atoi(scratch) : 1;
    if (!androidVersion) {
        mOSVersion = 1000; // if androidVersion is 0, it means "future"
    } else {
        length = __system_property_get("ro.build.version.sdk", scratch);
        mOSVersion = length >= 0 ? atoi(scratch) : 1;
    }

    // This disables an ANGLE optimization on ARM, which turns out to be more costly for us
    // see b/229017581
    // We need to do this before we create the GL context.
    // An alternative solution is use a system property:
    //            __system_property_set(
    //                    "debug.angle.feature_overrides_disabled",
    //                    "preferSubmitAtFBOBoundary");
    // but that would outlive this process, so the environment variable is better.
    // We also make sure to not update the variable if it already exists.
    // There is no harm setting this if we're not on ANGLE or ARM.
    setenv("ANGLE_FEATURE_OVERRIDES_DISABLED", "preferSubmitAtFBOBoundary", false);
}

PlatformEGLAndroid::~PlatformEGLAndroid() noexcept = default;


void PlatformEGLAndroid::terminate() noexcept {
    ExternalStreamManagerAndroid::destroy(&mExternalStreamManager);
    PlatformEGL::terminate();
}

void PlatformEGLAndroid::beginFrame(
        int64_t monotonic_clock_ns,
        int64_t refreshIntervalNs,
        uint32_t frameId) noexcept {
    if (mPerformanceHintSession.isValid()) {
        if (refreshIntervalNs <= 0) {
            // we're not provided with a target time, assume 16.67ms
            refreshIntervalNs = 16'666'667;
        }
        mStartTimeOfActualWork = clock::time_point(std::chrono::nanoseconds(monotonic_clock_ns));
        mPerformanceHintSession.updateTargetWorkDuration(refreshIntervalNs);
    }
    PlatformEGL::beginFrame(monotonic_clock_ns, refreshIntervalNs, frameId);
}

void backend::PlatformEGLAndroid::preCommit() noexcept {
    if (mPerformanceHintSession.isValid()) {
        auto const actualWorkDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - mStartTimeOfActualWork);
        mPerformanceHintSession.reportActualWorkDuration(actualWorkDuration.count());
    }
    PlatformEGL::preCommit();
}

Driver* PlatformEGLAndroid::createDriver(void* sharedContext,
        const Platform::DriverConfig& driverConfig) noexcept {

    // the refresh rate default value doesn't matter, we change it later
    int32_t const tid = gettid();
    mPerformanceHintSession = PerformanceHintManager::Session{
            mPerformanceHintManager, &tid, 1, 16'666'667 };

    Driver* driver = PlatformEGL::createDriver(sharedContext, driverConfig);
    auto extensions = GLUtils::split(eglQueryString(mEGLDisplay, EGL_EXTENSIONS));

    eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC) eglGetProcAddress(
            "eglGetNativeClientBufferANDROID");

    if (extensions.has("EGL_ANDROID_presentation_time")) {
        eglPresentationTimeANDROID = (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress(
                "eglPresentationTimeANDROID");
    }

    if (extensions.has("EGL_ANDROID_get_frame_timestamps")) {
        eglGetCompositorTimingSupportedANDROID = (PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC)eglGetProcAddress(
                "eglGetCompositorTimingSupportedANDROID");
        eglGetCompositorTimingANDROID = (PFNEGLGETCOMPOSITORTIMINGANDROIDPROC)eglGetProcAddress(
                "eglGetCompositorTimingANDROID");
        eglGetNextFrameIdANDROID = (PFNEGLGETNEXTFRAMEIDANDROIDPROC)eglGetProcAddress(
                "eglGetNextFrameIdANDROID");
        eglGetFrameTimestampSupportedANDROID = (PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC)eglGetProcAddress(
                "eglGetFrameTimestampSupportedANDROID");
        eglGetFrameTimestampsANDROID = (PFNEGLGETFRAMETIMESTAMPSANDROIDPROC)eglGetProcAddress(
                "eglGetFrameTimestampsANDROID");
    }

    return driver;
}

void PlatformEGLAndroid::setPresentationTime(int64_t presentationTimeInNanosecond) noexcept {
    EGLSurface currentDrawSurface = eglGetCurrentSurface(EGL_DRAW);
    if (currentDrawSurface != EGL_NO_SURFACE) {
        if (eglPresentationTimeANDROID) {
            eglPresentationTimeANDROID(
                    mEGLDisplay,
                    currentDrawSurface,
                    presentationTimeInNanosecond);
        }
    }
}

OpenGLPlatform::ExternalTexture* UTILS_NULLABLE PlatformEGLAndroid::createExternalImageTexture(void* _Nullable hardware_buffer) noexcept {
  slog.i << "mExternalBufferPlatformEGLAndroid" << io::endl;
  ExternalTexture* outTexture = new(std::nothrow) ExternalTexture{};
  if (!outTexture) {
    slog.e << "mExternalBufferPlatformEGLAndroid Failed to allocate ExternalTexture" << io::endl;
    return nullptr;
  }
  AHardwareBuffer* hardwareBuffer = static_cast<AHardwareBuffer*>(hardware_buffer);
  AHardwareBuffer_Desc hardware_buffer_description = {};
  AHardwareBuffer_describe(hardwareBuffer, &hardware_buffer_description);
  // If the texture is in YUV, we will sample it as an external image and let
  // GL_TEXTURE_EXTERNAL_OES help us convert it into RGB.
  // we may get YUV pixel format that's undocumented in Android
  // (e.g. YCbCr_420_SP_VENUS_UBWC from https://jbit.net/Android_Colors/), so we
  // are currently assuming every non-RGB texture is in YUV. This is not 100%
  // safe as the pixel format can be neither RGB nor YUV.
  slog.i << "mExternalBufferPlatformEGLAndroid hb format " << hardware_buffer_description.format << io::endl;
  bool isExternalFormat = true;
  switch (hardware_buffer_description.format) {
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
    case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
    case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
    case AHARDWAREBUFFER_FORMAT_D16_UNORM:
    case AHARDWAREBUFFER_FORMAT_D24_UNORM:
    case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT:
    case AHARDWAREBUFFER_FORMAT_D32_FLOAT:
    case AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT:
    case AHARDWAREBUFFER_FORMAT_S8_UINT:
      isExternalFormat = false;
      break;
  }
  // Log the determined target type
  slog.i << "mExternalBufferPlatformEGLAndroid Texture target is " << (isExternalFormat ? "GL_TEXTURE_EXTERNAL_OES" : "GL_TEXTURE_2D") << io::endl;
  // Get the EGL client buffer from AHardwareBuffer
  EGLClientBuffer clientBuffer = eglGetNativeClientBufferANDROID(hardwareBuffer);
  // Questions around attributes with isSrgbTransfer and protected content
  EGLint imageAttrs[] = {EGL_IMAGE_PRESERVED_KHR,
                      EGL_TRUE,
                      EGL_NONE,
                      EGL_NONE,
                      EGL_NONE,
                      EGL_NONE,
                      EGL_NONE};
  int attrIndex = 2;
  bool isSrgbTransfer = false;
  if (isSrgbTransfer) {
    imageAttrs[attrIndex++] = EGL_GL_COLORSPACE;
    imageAttrs[attrIndex++] = EGL_GL_COLORSPACE_SRGB;
  }

//  if (hardware_buffer_description.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) {
//    imageAttrs[attrIndex++] = EGL_PROTECTED_CONTENT_EXT;
//    imageAttrs[attrIndex++] = EGL_TRUE;
//  }
  // Create an EGLImage from the client buffer
  EGLImageKHR eglImage = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, imageAttrs);
  if (eglImage == EGL_NO_IMAGE_KHR) {
    // Handle error
    slog.e << "mExternalBufferPlatformEGLAndroid Failed to create EGL image" << io::endl;
    delete outTexture;
    return nullptr;
  }
  // Create and bind the OpenGL texture
  glGenTextures(1, &outTexture->id);
  glActiveTexture(GL_TEXTURE0);
  auto target = isExternalFormat ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
  glBindTexture(target, outTexture->id);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    slog.e << "mExternalBufferPlatformEGLAndroid Error after glBindTexture: " << error << io::endl;
    glDeleteTextures(1, &outTexture->id);
    eglDestroyImageKHR(eglGetCurrentDisplay(), eglImage);
    delete outTexture;
    return nullptr;
  }
  glEGLImageTargetTexture2DOES(target, static_cast<GLeglImageOES>(eglImage));
  error = glGetError();
  if (error != GL_NO_ERROR) {
    slog.e << "mExternalBufferPlatformEGLAndroid Error after glEGLImageTargetTexture2DOES: " << error << io::endl;
  }

  if (!isExternalFormat) {
    // Set up mipmap generation for GL_TEXTURE_2D only
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    error = glGetError();
    if (error != GL_NO_ERROR) {
      slog.e << "mExternalBufferPlatformEGLAndroid Error after mipmap generation: " << error << io::endl;
    }
  }
  outTexture->target = target;
  slog.i << "mExternalBufferPlatformEGLAndroid Successfully created external image texture with ID: " << outTexture->id << io::endl;

  // Create and return ExternalTexture object
  return outTexture;
}

Platform::Stream* PlatformEGLAndroid::createStream(void* nativeStream) noexcept {
    return mExternalStreamManager.acquire(static_cast<jobject>(nativeStream));
}

void PlatformEGLAndroid::destroyStream(Platform::Stream* stream) noexcept {
    mExternalStreamManager.release(stream);
}

void PlatformEGLAndroid::attach(Stream* stream, intptr_t tname) noexcept {
    mExternalStreamManager.attach(stream, tname);
}

void PlatformEGLAndroid::detach(Stream* stream) noexcept {
    mExternalStreamManager.detach(stream);
}

void PlatformEGLAndroid::updateTexImage(Stream* stream, int64_t* timestamp) noexcept {
    mExternalStreamManager.updateTexImage(stream, timestamp);
}

int PlatformEGLAndroid::getOSVersion() const noexcept {
    return mOSVersion;
}

AcquiredImage PlatformEGLAndroid::transformAcquiredImage(AcquiredImage source) noexcept {
    // Convert the AHardwareBuffer to EGLImage.
    AHardwareBuffer const* const pHardwareBuffer = (const AHardwareBuffer*)source.image;

    EGLClientBuffer clientBuffer = eglGetNativeClientBufferANDROID(pHardwareBuffer);
    if (!clientBuffer) {
        slog.e << "Unable to get EGLClientBuffer from AHardwareBuffer." << io::endl;
        return {};
    }

    PlatformEGL::Config attributes;

    if (__builtin_available(android 26, *)) {
        AHardwareBuffer_Desc desc;
        AHardwareBuffer_describe(pHardwareBuffer, &desc);
        bool const isProtectedContent =
                desc.usage & AHardwareBuffer_UsageFlags::AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;
        if (isProtectedContent) {
            attributes[EGL_PROTECTED_CONTENT_EXT] = EGL_TRUE;
        }
    }

    EGLImageKHR eglImage = eglCreateImageKHR(mEGLDisplay,
            EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attributes.data());
    if (eglImage == EGL_NO_IMAGE_KHR) {
        slog.e << "eglCreateImageKHR returned no image." << io::endl;
        return {};
    }

    // Destroy the EGLImage before invoking the user's callback.
    struct Closure {
        Closure(AcquiredImage const& acquiredImage, EGLDisplay display)
                : acquiredImage(acquiredImage), display(display) {}
        AcquiredImage acquiredImage;
        EGLDisplay display;
    };
    Closure* closure = new(std::nothrow) Closure(source, mEGLDisplay);
    auto patchedCallback = [](void* image, void* userdata) {
        Closure* closure = (Closure*)userdata;
        if (eglDestroyImageKHR(closure->display, (EGLImageKHR) image) == EGL_FALSE) {
            slog.e << "eglDestroyImageKHR failed." << io::endl;
        }
        closure->acquiredImage.callback(closure->acquiredImage.image, closure->acquiredImage.userData);
        delete closure;
    };

    return { eglImage, patchedCallback, closure, source.handler };
}

} // namespace filament::backend

// ---------------------------------------------------------------------------------------------
