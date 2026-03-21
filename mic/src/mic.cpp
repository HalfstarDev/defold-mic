#define EXTENSION_NAME Mic
#define LIB_NAME "Mic"
#define MODULE_NAME "mic"

#include <dmsdk/sdk.h>
#include <dmsdk/dlib/mutex.h>

#include <vector>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
#include <cerrno>
#include <cctype>
#include <cstdio>

#if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
#include <CoreFoundation/CoreFoundation.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#endif

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_GENERATION
#include "miniaudio.h"

static const ma_uint32 DEFAULT_SAMPLE_RATE = 16000;
static const ma_uint32 DEFAULT_CHANNELS    = 1;
static const ma_uint32 BIT_DEPTH           = 16;

struct MicOptions
{
    int       device_index;
    ma_uint32 sample_rate;
    ma_uint32 channels;
    double    max_seconds;
    double    noise_gate_threshold;
};

struct MicState
{
    ma_context            context;
    bool                  contextInitialized;
    ma_device             device;
    std::vector<ma_int16> samples;
    ma_uint32             sampleRate;
    ma_uint32             channels;
    bool                  isRecording;
    bool                  deviceInitialized;
    ma_uint64             maxSamples;
    ma_int16              noiseGateS16;
    double                currentRMS;
    int                   currentPeak;
    bool                  loggedNullInputWarning;
    bool                  loggedMaxSamplesWarning;
    bool                  permissionRequestInFlight;
    bool                  hasPermissionStatusOverride;
    int                   permissionStatusOverride;
    dmMutex::HMutex       mutex;
};

static MicState g_Mic;

static const char* SafeDeviceName(const char* name)
{
    return (name && name[0] != '\0') ? name : "<unknown>";
}

static void MiniaudioLogCallback(void* pUserData, ma_uint32 level, const char* pMessage)
{
    (void)pUserData;

    if (pMessage == NULL || pMessage[0] == '\0') {
        return;
    }

    if (level == MA_LOG_LEVEL_ERROR) {
        dmLogError("Mic/miniaudio: %s", pMessage);
    } else if (level == MA_LOG_LEVEL_WARNING) {
        dmLogWarning("Mic/miniaudio: %s", pMessage);
    } else {
        dmLogInfo("Mic/miniaudio: %s", pMessage);
    }
}

static const char* DeviceNotificationTypeToString(ma_device_notification_type type)
{
    switch (type) {
        case ma_device_notification_type_started: return "started";
        case ma_device_notification_type_stopped: return "stopped";
        case ma_device_notification_type_rerouted: return "rerouted";
        case ma_device_notification_type_interruption_began: return "interruption_began";
        case ma_device_notification_type_interruption_ended: return "interruption_ended";
        case ma_device_notification_type_unlocked: return "unlocked";
        default: return "unknown";
    }
}

static void DeviceNotificationCallback(const ma_device_notification* pNotification)
{
    if (pNotification == NULL || pNotification->pDevice == NULL) {
        dmLogWarning("Mic: Received a null device notification");
        return;
    }

    dmLogInfo("Mic: Device notification '%s' for '%s'",
    DeviceNotificationTypeToString(pNotification->type),
    SafeDeviceName(pNotification->pDevice->capture.name));
}

static void LogCaptureDevices(const char* reason)
{
    if (!g_Mic.contextInitialized) {
        dmLogWarning("Mic: Cannot enumerate capture devices during %s because the audio context is not initialised", reason);
        return;
    }

    ma_device_info* pPlaybackInfos;
    ma_device_info* pCaptureInfos;
    ma_uint32 playbackCount;
    ma_uint32 captureCount;

    const ma_result result = ma_context_get_devices(&g_Mic.context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
    if (result != MA_SUCCESS) {
        dmLogWarning("Mic: Failed to enumerate capture devices during %s: %s", reason, ma_result_description(result));
        return;
    }

    dmLogInfo("Mic: %s found %u capture device(s)", reason, captureCount);
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        dmLogInfo("Mic: capture[%u]%s name='%s'", i, pCaptureInfos[i].isDefault ? " default" : "", SafeDeviceName(pCaptureInfos[i].name));
    }

    if (captureCount == 0) {
        dmLogWarning("Mic: No capture devices are currently available");
    }
}

static double NormalizeSInt16(double x)
{
    const double n = x / 32767.0;
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

// ---------------- Apple devices code START ----------------

#if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
enum AppleMicPermissionStatus
{
    APPLE_MIC_PERMISSION_UNAVAILABLE    = -1,
    APPLE_MIC_PERMISSION_NOT_DETERMINED = 0,
    APPLE_MIC_PERMISSION_RESTRICTED     = 1,
    APPLE_MIC_PERMISSION_DENIED         = 2,
    APPLE_MIC_PERMISSION_GRANTED        = 3,
};

static const char* AppleMicPermissionStatusToString(AppleMicPermissionStatus status)
{
    switch (status) {
        case APPLE_MIC_PERMISSION_NOT_DETERMINED: return "not_determined";
        case APPLE_MIC_PERMISSION_RESTRICTED: return "restricted";
        case APPLE_MIC_PERMISSION_DENIED: return "denied";
        case APPLE_MIC_PERMISSION_GRANTED: return "granted";
        default: return "unavailable";
    }
}

static const char* GetAppleStringUTF8(id value)
{
    if (value == nil) {
        return NULL;
    }

    typedef const char* (*UTF8StringFn)(id, SEL);
    UTF8StringFn utf8String = (UTF8StringFn)objc_msgSend;
    return utf8String(value, sel_registerName("UTF8String"));
}

static void SetAppleMicPermissionStatusOverride(AppleMicPermissionStatus status)
{
    g_Mic.hasPermissionStatusOverride = true;
    g_Mic.permissionStatusOverride = static_cast<int>(status);
}

static void ClearAppleMicPermissionStatusOverride()
{
    g_Mic.hasPermissionStatusOverride = false;
    g_Mic.permissionStatusOverride = static_cast<int>(APPLE_MIC_PERMISSION_UNAVAILABLE);
}

#define APPLE_FOURCC(a, b, c, d) \
((static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24) | \
(static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16) | \
(static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8) | \
static_cast<uint32_t>(static_cast<unsigned char>(d)))

static AppleMicPermissionStatus QueryAppleMicPermissionStatusRaw()
{
    #if defined(DM_PLATFORM_IOS)
    Class audioSessionClass = (Class)objc_getClass("AVAudioSession");
    if (audioSessionClass == NULL) {
        return APPLE_MIC_PERMISSION_UNAVAILABLE;
    }

    typedef id (*SharedInstanceFn)(id, SEL);
    SharedInstanceFn sharedInstance = (SharedInstanceFn)objc_msgSend;
    id session = sharedInstance((id)audioSessionClass, sel_registerName("sharedInstance"));
    if (session == nil) {
        return APPLE_MIC_PERMISSION_UNAVAILABLE;
    }

    typedef uint32_t (*RecordPermissionFn)(id, SEL);
    RecordPermissionFn recordPermission = (RecordPermissionFn)objc_msgSend;
    const uint32_t status = recordPermission(session, sel_registerName("recordPermission"));

    switch (status) {
        case 0:
        case APPLE_FOURCC('u', 'n', 'd', 't'):
        return APPLE_MIC_PERMISSION_NOT_DETERMINED;
        case APPLE_FOURCC('d', 'e', 'n', 'y'):
        return APPLE_MIC_PERMISSION_DENIED;
        case APPLE_FOURCC('g', 'r', 'n', 't'):
        return APPLE_MIC_PERMISSION_GRANTED;
        default:
        return APPLE_MIC_PERMISSION_UNAVAILABLE;
    }
    #else
    Class captureDeviceClass = (Class)objc_getClass("AVCaptureDevice");
    if (captureDeviceClass == NULL) {
        return APPLE_MIC_PERMISSION_UNAVAILABLE;
    }

    SEL authSel = sel_registerName("authorizationStatusForMediaType:");
    typedef BOOL (*RespondsToSelectorFn)(id, SEL, SEL);
    RespondsToSelectorFn respondsToSelector = (RespondsToSelectorFn)objc_msgSend;

    if (!respondsToSelector((id)captureDeviceClass, sel_registerName("respondsToSelector:"), authSel)) {
        return APPLE_MIC_PERMISSION_GRANTED; // macOS < 10.14
    }

    typedef long (*AuthorizationStatusFn)(id, SEL, CFStringRef);
    AuthorizationStatusFn authorizationStatus = (AuthorizationStatusFn)objc_msgSend;
    const long status = authorizationStatus((id)captureDeviceClass, sel_registerName("authorizationStatusForMediaType:"), CFSTR("soun"));

    switch (status) {
        case 0:  return APPLE_MIC_PERMISSION_NOT_DETERMINED;
        case 1:  return APPLE_MIC_PERMISSION_RESTRICTED;
        case 2:  return APPLE_MIC_PERMISSION_DENIED;
        case 3:  return APPLE_MIC_PERMISSION_GRANTED;
        default: return APPLE_MIC_PERMISSION_UNAVAILABLE;
    }
    #endif
}

static AppleMicPermissionStatus GetAppleMicPermissionStatus()
{
    const AppleMicPermissionStatus rawStatus = QueryAppleMicPermissionStatusRaw();
    if (rawStatus == APPLE_MIC_PERMISSION_GRANTED || rawStatus == APPLE_MIC_PERMISSION_DENIED || rawStatus == APPLE_MIC_PERMISSION_RESTRICTED) {
        SetAppleMicPermissionStatusOverride(rawStatus);
        return rawStatus;
    }

    if (g_Mic.hasPermissionStatusOverride) {
        const AppleMicPermissionStatus overrideStatus = static_cast<AppleMicPermissionStatus>(g_Mic.permissionStatusOverride);
        if (overrideStatus == APPLE_MIC_PERMISSION_GRANTED || overrideStatus == APPLE_MIC_PERMISSION_DENIED || overrideStatus == APPLE_MIC_PERMISSION_RESTRICTED) {
            return overrideStatus;
        }
    }

    return rawStatus;
}

static void LogAppleBundleInfo()
{
    Class bundleClass = (Class)objc_getClass("NSBundle");
    if (bundleClass == NULL) {
        dmLogWarning("Mic: NSBundle is unavailable, cannot log app identity");
        return;
    }

    typedef id (*MainBundleFn)(id, SEL);
    MainBundleFn mainBundle = (MainBundleFn)objc_msgSend;
    id bundle = mainBundle((id)bundleClass, sel_registerName("mainBundle"));
    if (bundle == nil) {
        dmLogWarning("Mic: Failed to resolve NSBundle.mainBundle");
        return;
    }

    typedef id (*ObjectGetterFn)(id, SEL);
    ObjectGetterFn getObject = (ObjectGetterFn)objc_msgSend;
    typedef id (*ObjectForInfoKeyFn)(id, SEL, id);
    ObjectForInfoKeyFn objectForInfoKey = (ObjectForInfoKeyFn)objc_msgSend;

    const char* bundleIdentifier = GetAppleStringUTF8(getObject(bundle, sel_registerName("bundleIdentifier")));
    const char* bundlePath = GetAppleStringUTF8(getObject(bundle, sel_registerName("bundlePath")));
    const char* executablePath = GetAppleStringUTF8(getObject(bundle, sel_registerName("executablePath")));
    const char* microphoneUsageDescription = GetAppleStringUTF8(objectForInfoKey(bundle, sel_registerName("objectForInfoDictionaryKey:"), (id)CFSTR("NSMicrophoneUsageDescription")));

    dmLogInfo("Mic: App bundle identifier='%s' bundle_path='%s' executable_path='%s'",
    bundleIdentifier ? bundleIdentifier : "<none>",
    bundlePath ? bundlePath : "<none>",
    executablePath ? executablePath : "<none>");

    if (microphoneUsageDescription && microphoneUsageDescription[0] != '\0') {
        dmLogInfo("Mic: Host app declares NSMicrophoneUsageDescription='%s'", microphoneUsageDescription);
    } else {
        dmLogWarning("Mic: Host app is missing NSMicrophoneUsageDescription, so macOS may not show a microphone permission prompt");
    }

    if (bundleIdentifier && strcmp(bundleIdentifier, "com.defold.editor") == 0) {
        dmLogWarning("Mic: Running inside the Defold editor. The extension's macOS Info.plist only applies to bundled apps, not /Applications/Defold.app");
    }
}

static bool RequestAppleMicPermission()
{
    #if defined(DM_PLATFORM_IOS)
    Class audioSessionClass = (Class)objc_getClass("AVAudioSession");
    if (audioSessionClass == NULL) {
        dmLogWarning("Mic: AVAudioSession is unavailable, cannot request microphone permission");
        return false;
    }

    typedef id (*SharedInstanceFn)(id, SEL);
    SharedInstanceFn sharedInstance = (SharedInstanceFn)objc_msgSend;
    id session = sharedInstance((id)audioSessionClass, sel_registerName("sharedInstance"));
    if (session == nil) {
        dmLogWarning("Mic: AVAudioSession sharedInstance is unavailable, cannot request microphone permission");
        return false;
    }

    if (g_Mic.permissionRequestInFlight) {
        dmLogInfo("Mic: Microphone permission request is already in flight");
        return true;
    }

    g_Mic.permissionRequestInFlight = true;

    typedef void (*RequestRecordPermissionFn)(id, SEL, void (^)(BOOL));
    RequestRecordPermissionFn requestRecordPermission = (RequestRecordPermissionFn)objc_msgSend;
    requestRecordPermission(session, sel_registerName("requestRecordPermission:"), ^(BOOL granted) {
        dmMutex::Lock(g_Mic.mutex);
        g_Mic.permissionRequestInFlight = false;
        SetAppleMicPermissionStatusOverride(granted ? APPLE_MIC_PERMISSION_GRANTED : APPLE_MIC_PERMISSION_DENIED);
        dmMutex::Unlock(g_Mic.mutex);
        dmLogInfo("Mic: Microphone permission callback -> %s", granted ? "granted" : "denied");
    });

    dmLogInfo("Mic: Requested microphone permission from AVAudioSession");
    return true;
    #else
    Class captureDeviceClass = (Class)objc_getClass("AVCaptureDevice");
    if (captureDeviceClass == NULL) {
        dmLogWarning("Mic: AVCaptureDevice is unavailable, cannot request microphone permission");
        return false;
    }

    if (g_Mic.permissionRequestInFlight) {
        dmLogInfo("Mic: Microphone permission request is already in flight");
        return true;
    }

    g_Mic.permissionRequestInFlight = true;

    typedef void (*RequestAccessFn)(id, SEL, CFStringRef, void (^)(BOOL));
    RequestAccessFn requestAccess = (RequestAccessFn)objc_msgSend;
    requestAccess((id)captureDeviceClass, sel_registerName("requestAccessForMediaType:completionHandler:"), CFSTR("soun"), ^(BOOL granted) {
        dmMutex::Lock(g_Mic.mutex);
        g_Mic.permissionRequestInFlight = false;
        SetAppleMicPermissionStatusOverride(granted ? APPLE_MIC_PERMISSION_GRANTED : APPLE_MIC_PERMISSION_DENIED);
        dmMutex::Unlock(g_Mic.mutex);
        dmLogInfo("Mic: Microphone permission callback -> %s", granted ? "granted" : "denied");
    });

    dmLogInfo("Mic: Requested microphone permission from the OS");
    return true;
    #endif
}
#endif

// ---------------- Apple devices code END ----------------

static void UninitDevice()
{
    if (g_Mic.deviceInitialized) {
        dmLogInfo("Mic: Uninitialising capture device '%s'", SafeDeviceName(g_Mic.device.capture.name));
        ma_device_uninit(&g_Mic.device);
        g_Mic.deviceInitialized = false;
    }
}

// mic.get_permission_status()
static int MicGetPermissionStatus(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    #if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
    lua_pushstring(L, AppleMicPermissionStatusToString(GetAppleMicPermissionStatus()));
    #else
    lua_pushstring(L, "unavailable");
    #endif
    return 1;
}

// mic.request_permission()
static int MicRequestPermission(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    #if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
    const AppleMicPermissionStatus status = GetAppleMicPermissionStatus();
    if (status == APPLE_MIC_PERMISSION_GRANTED) {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (status == APPLE_MIC_PERMISSION_DENIED || status == APPLE_MIC_PERMISSION_RESTRICTED) {
        dmLogWarning("Mic: Microphone permission status is %s; enable access in system settings", AppleMicPermissionStatusToString(status));
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, RequestAppleMicPermission() ? 1 : 0);
    #else
    lua_pushboolean(L, 0);
    #endif
    return 1;
}

static void CaptureCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    (void)pOutput;
    MicState* mic = static_cast<MicState*>(pDevice->pUserData);

    dmMutex::Lock(mic->mutex);

    if (!pInput) {
        if (!mic->loggedNullInputWarning) {
            mic->loggedNullInputWarning = true;
            dmLogWarning("Mic: Capture callback received no input buffer from '%s'", SafeDeviceName(pDevice->capture.name));
        }
        dmMutex::Unlock(mic->mutex);
        return;
    }

    if (!mic->isRecording) {
        dmMutex::Unlock(mic->mutex);
        return;
    }

    size_t totalSamplesInCallback = static_cast<size_t>(frameCount) * mic->channels;
    size_t toAdd = totalSamplesInCallback;

    if (mic->maxSamples > 0) {
        const size_t already = mic->samples.size();
        if (already >= static_cast<size_t>(mic->maxSamples)) {
            if (!mic->loggedMaxSamplesWarning) {
                mic->loggedMaxSamplesWarning = true;
                dmLogWarning("Mic: Reached max_seconds limit on '%s'; additional audio will be discarded", SafeDeviceName(pDevice->capture.name));
            }
            dmMutex::Unlock(mic->mutex);
            return;
        }
        const size_t remaining = static_cast<size_t>(mic->maxSamples) - already;
        if (toAdd > remaining) {
            toAdd = remaining;
        }
        if (toAdd == 0) {
            if (!mic->loggedMaxSamplesWarning) {
                mic->loggedMaxSamplesWarning = true;
                dmLogWarning("Mic: Reached max_seconds limit on '%s'; additional audio will be discarded", SafeDeviceName(pDevice->capture.name));
            }
            dmMutex::Unlock(mic->mutex);
            return;
        }
    }

    const ma_int16* src = static_cast<const ma_int16*>(pInput);
    double sumSq = 0.0;
    int blockPeak = 0;
    for (size_t i = 0; i < toAdd; ++i) {
        int a = std::abs(src[i]);
        if (a > blockPeak) blockPeak = a;
        sumSq += static_cast<double>(src[i]) * static_cast<double>(src[i]);
    }

    if (mic->noiseGateS16 > 0 && blockPeak < static_cast<int>(mic->noiseGateS16)) {
        mic->samples.insert(mic->samples.end(), toAdd, 0); 
        mic->currentPeak = 0;
        mic->currentRMS = 0.0;
    } else {
        mic->samples.insert(mic->samples.end(), src, src + toAdd);
        mic->currentPeak = blockPeak;
        mic->currentRMS = std::sqrt(sumSq / static_cast<double>(toAdd));
    }

    dmMutex::Unlock(mic->mutex);
}

// mic.start([options])
static int MicStart(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 0);

    if (g_Mic.isRecording) {
        dmLogWarning("Mic: start() ignored because recording is already active on '%s'", SafeDeviceName(g_Mic.device.capture.name));
        return 0;
    }

    if (!g_Mic.contextInitialized) {
        dmLogError("Mic: start() failed because the audio context is not initialised");
        return luaL_error(L, "Mic: Audio context was not initialized properly");
    }

    #if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
    const AppleMicPermissionStatus permissionStatus = GetAppleMicPermissionStatus();
    dmLogInfo("Mic: start() microphone permission status is %s", AppleMicPermissionStatusToString(permissionStatus));

    if (permissionStatus == APPLE_MIC_PERMISSION_NOT_DETERMINED) {
        RequestAppleMicPermission();
        return luaL_error(L, "Mic: Microphone permission is not granted yet. Approve the OS prompt and try again.");
    }

    if (permissionStatus == APPLE_MIC_PERMISSION_DENIED || permissionStatus == APPLE_MIC_PERMISSION_RESTRICTED) {
        return luaL_error(L, "Mic: Microphone permission is %s. Enable microphone access for this app in system settings.", AppleMicPermissionStatusToString(permissionStatus));
    }
    #endif

    MicOptions opts;
    opts.device_index = -1; // default device
    opts.sample_rate = DEFAULT_SAMPLE_RATE;
    opts.channels = DEFAULT_CHANNELS;
    opts.max_seconds = 60.0;
    opts.noise_gate_threshold = 0.0;

    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);

        lua_getfield(L, 1, "device_index");
        if (!lua_isnil(L, -1)) {
            opts.device_index = static_cast<int>(luaL_checkinteger(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "sample_rate");
        if (!lua_isnil(L, -1)) {
            const int sr = static_cast<int>(luaL_checkinteger(L, -1));
            if (sr <= 0) {
                return luaL_error(L, "Mic: sample_rate must be a positive number");
            }
            opts.sample_rate = static_cast<ma_uint32>(sr);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "channels");
        if (!lua_isnil(L, -1)) {
            const int ch = static_cast<int>(luaL_checkinteger(L, -1));
            if (ch != 1 && ch != 2) {
                return luaL_error(L, "Mic: channels must be 1 or 2");
            }
            opts.channels = static_cast<ma_uint32>(ch);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "max_seconds");
        if (!lua_isnil(L, -1)) {
            opts.max_seconds = luaL_checknumber(L, -1);
            if (opts.max_seconds < 0.0) {
                return luaL_error(L, "Mic: max_seconds must be >= 0");
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "noise_gate_threshold");
        if (!lua_isnil(L, -1)) {
            opts.noise_gate_threshold = luaL_checknumber(L, -1);
            if (opts.noise_gate_threshold < 0.0 || opts.noise_gate_threshold > 1.0) {
                return luaL_error(L, "Mic: noise_gate_threshold must be in [0.0, 1.0]");
            }
        }
        lua_pop(L, 1);
    }

    dmLogInfo("Mic: start() requested device_index=%d sample_rate=%u channels=%u max_seconds=%.3f noise_gate_threshold=%.5f",
    opts.device_index,
    opts.sample_rate,
    opts.channels,
    opts.max_seconds,
    opts.noise_gate_threshold);
    LogCaptureDevices("start()");

    UninitDevice();

    dmMutex::Lock(g_Mic.mutex);
    g_Mic.samples.clear();
    g_Mic.currentRMS = 0.0;
    g_Mic.currentPeak = 0;
    g_Mic.loggedNullInputWarning = false;
    g_Mic.loggedMaxSamplesWarning = false;
    dmMutex::Unlock(g_Mic.mutex);

    ma_device_id selectedDeviceId;
    ma_device_id* pDeviceId = NULL;

    if (opts.device_index >= 0) {
        ma_device_info* pPlaybackInfos;
        ma_device_info* pCaptureInfos;
        ma_uint32 playbackCount;
        ma_uint32 captureCount;

        const ma_result enumResult = ma_context_get_devices(&g_Mic.context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);

        if (enumResult != MA_SUCCESS || static_cast<ma_uint32>(opts.device_index) >= captureCount) {
            if (enumResult != MA_SUCCESS) {
                dmLogError("Mic: Failed to enumerate capture devices for requested index %d: %s", opts.device_index, ma_result_description(enumResult));
                return luaL_error(L, "Mic: Failed to enumerate capture devices: %s", ma_result_description(enumResult));
            }

            dmLogError("Mic: Requested device_index %d is not available (capture device count=%u)", opts.device_index, captureCount);
            return luaL_error(L, "Mic: Device_index %d is not available", opts.device_index);
        }

        selectedDeviceId = pCaptureInfos[opts.device_index].id;
        pDeviceId = &selectedDeviceId;
        dmLogInfo("Mic: start() selected capture[%d] name='%s'%s",
        opts.device_index,
        SafeDeviceName(pCaptureInfos[opts.device_index].name),
        pCaptureInfos[opts.device_index].isDefault ? " (default)" : "");
    } else {
        dmLogInfo("Mic: start() will use the system default capture device");
    }

    ma_device_config cfg     = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID    = pDeviceId;
    cfg.capture.format       = ma_format_s16;
    cfg.capture.channels     = opts.channels;
    cfg.sampleRate           = opts.sample_rate;
    cfg.dataCallback         = CaptureCallback;
    cfg.notificationCallback = DeviceNotificationCallback;
    cfg.pUserData            = &g_Mic;

    const ma_result initResult = ma_device_init(&g_Mic.context, &cfg, &g_Mic.device);
    if (initResult != MA_SUCCESS) {
        dmLogError("Mic: Failed to initialise capture device: %s", ma_result_description(initResult));
        return luaL_error(L, "Mic: Failed to initialise capture device: %s", ma_result_description(initResult));
    }

    // Read back the values actually negotiated with the driver.
    g_Mic.sampleRate        = g_Mic.device.sampleRate;
    g_Mic.channels          = g_Mic.device.capture.channels;
    g_Mic.deviceInitialized = true;

    if (opts.max_seconds > 0.0) {
        const ma_uint64 frames = static_cast<ma_uint64>(opts.max_seconds * g_Mic.sampleRate);
        g_Mic.maxSamples = frames * g_Mic.channels;
    } else {
        g_Mic.maxSamples = 0; // unlimited
    }

    if (opts.noise_gate_threshold > 0.0) {
        g_Mic.noiseGateS16 = static_cast<ma_int16>(opts.noise_gate_threshold * 32767.0);
    } else {
        g_Mic.noiseGateS16 = 0;
    }

    dmLogInfo("Mic: Negotiated capture device='%s' sample_rate=%u channels=%u internal_sample_rate=%u internal_channels=%u",
    SafeDeviceName(g_Mic.device.capture.name),
    g_Mic.device.sampleRate,
    g_Mic.device.capture.channels,
    g_Mic.device.capture.internalSampleRate,
    g_Mic.device.capture.internalChannels);

    const ma_result startResult = ma_device_start(&g_Mic.device);
    if (startResult != MA_SUCCESS) {
        dmLogError("Mic: Failed to start capture device '%s': %s", SafeDeviceName(g_Mic.device.capture.name), ma_result_description(startResult));
        UninitDevice();
        return luaL_error(L, "Mic: Failed to start capture device: %s", ma_result_description(startResult));
    }

    g_Mic.isRecording = true;
    dmLogInfo("Mic: Recording started on '%s'", SafeDeviceName(g_Mic.device.capture.name));
    return 0;
}

// mic.stop()
static int MicStop(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    double normalize_peak = -1;

    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);

        lua_getfield(L, 1, "normalize_peak");
        if (!lua_isnil(L, -1)) {
            normalize_peak = luaL_checknumber(L, -1);
            if (normalize_peak <= 0.0 || normalize_peak > 1.0) {
                return luaL_error(L, "Mic: normalize_peak must be in (0.0, 1.0]");
            }
        }
        lua_pop(L, 1);
    }

    if (!g_Mic.isRecording) {
        dmLogWarning("Mic: stop() called while not recording");
        lua_pushnil(L);
        return 1;
    }

    dmMutex::Lock(g_Mic.mutex);
    g_Mic.isRecording = false;
    dmMutex::Unlock(g_Mic.mutex);

    const ma_result stopResult = ma_device_stop(&g_Mic.device);
    if (stopResult != MA_SUCCESS) {
        dmLogWarning("Mic: Failed to stop capture device '%s' cleanly: %s", SafeDeviceName(g_Mic.device.capture.name), ma_result_description(stopResult));
    }

    dmMutex::Lock(g_Mic.mutex);
    std::vector<ma_int16> samples;
    samples.swap(g_Mic.samples);
    g_Mic.currentRMS = 0.0;
    g_Mic.currentPeak = 0;
    dmMutex::Unlock(g_Mic.mutex);

    if (samples.empty()) {
        dmLogWarning("Mic: stop() produced no audio samples from '%s'", SafeDeviceName(g_Mic.device.capture.name));
        lua_pushnil(L);
        return 1;
    }

    int recordingPeak = 0;
    double sumSq = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const int sample = static_cast<int>(samples[i]);
        const int absSample = std::abs(sample);
        if (absSample > recordingPeak) {
            recordingPeak = absSample;
        }
        sumSq += static_cast<double>(sample) * static_cast<double>(sample);
    }

    const double recordingPeakNorm = NormalizeSInt16(static_cast<double>(recordingPeak));
    const double recordingRMSNorm = NormalizeSInt16(std::sqrt(sumSq / static_cast<double>(samples.size())));

    if (normalize_peak > 0 && recordingPeak > 0) {
        const double targetPeakS16 = normalize_peak * 32767.0;
        if (recordingPeak < static_cast<int>(targetPeakS16)) {
            const double scale = targetPeakS16 / static_cast<double>(recordingPeak);
            for (size_t i = 0; i < samples.size(); ++i) {
                const double scaled = static_cast<double>(samples[i]) * scale;
                if (scaled > 32767.0) {
                    samples[i] = 32767;
                } else if (scaled < -32768.0) {
                    samples[i] = -32768;
                } else {
                    samples[i] = static_cast<ma_int16>(std::lround(scaled));
                }
            }

            dmLogInfo("Mic: stop() normalized recording from peak=%.5f to target=%.5f using gain=%.3f",
            recordingPeakNorm,
            normalize_peak,
            scale);
        } else {
            dmLogInfo("Mic: stop() skipped normalization because recording peak %.5f already meets target %.5f",
            recordingPeakNorm,
            normalize_peak);
        }
    }

    dmLogInfo("Mic: stop() stats peak=%.5f rms=%.5f samples=%zu channels=%u sample_rate=%u",
    recordingPeakNorm,
    recordingRMSNorm,
    samples.size(),
    g_Mic.channels,
    g_Mic.sampleRate);

    ma_dr_wav_data_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.container     = ma_dr_wav_container_riff;
    fmt.format        = MA_DR_WAVE_FORMAT_PCM;
    fmt.channels      = g_Mic.channels;
    fmt.sampleRate    = g_Mic.sampleRate;
    fmt.bitsPerSample = BIT_DEPTH;

    void* pWavData = NULL;
    size_t wavDataSize = 0;
    const ma_uint64 frameCount = static_cast<ma_uint64>(samples.size()) / g_Mic.channels;

    ma_dr_wav wav;
    if (!ma_dr_wav_init_memory_write(&wav, &pWavData, &wavDataSize, &fmt, NULL)) {
        return luaL_error(L, "Mic: failed to initialise dr_wav memory writer");
    }

    ma_dr_wav_write_pcm_frames(&wav, frameCount, samples.data());
    ma_dr_wav_uninit(&wav);

    lua_pushlstring(L, static_cast<const char*>(pWavData), wavDataSize);
    ma_dr_wav_free(pWavData, NULL);

    dmLogInfo("Mic: stop() returning %zu bytes of WAV data from '%s'", wavDataSize, SafeDeviceName(g_Mic.device.capture.name));
    return 1;
}

// mic.is_recording()
static int MicIsRecording(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);
    lua_pushboolean(L, g_Mic.isRecording ? 1 : 0);
    return 1;
}

// mic.is_connected()
static int MicIsConnected(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    if (!g_Mic.contextInitialized) {
        lua_pushboolean(L, 0);
        return 1;
    }

    ma_device_info* pPlaybackInfos;
    ma_device_info* pCaptureInfos;
    ma_uint32 playbackCount;
    ma_uint32 captureCount;

    const ma_result result = ma_context_get_devices(&g_Mic.context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
    if (result != MA_SUCCESS) {
        dmLogWarning("Mic: is_connected() failed to enumerate capture devices: %s", ma_result_description(result));
    }

    lua_pushboolean(L, (result == MA_SUCCESS && captureCount > 0) ? 1 : 0);
    return 1;
}

// mic.get_devices()
static int MicGetDevices(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);
    lua_newtable(L);

    if (!g_Mic.contextInitialized) return 1;

    ma_device_info* pPlaybackInfos;
    ma_device_info* pCaptureInfos;
    ma_uint32 playbackCount;
    ma_uint32 captureCount;

    const ma_result result = ma_context_get_devices(&g_Mic.context, &pPlaybackInfos, &playbackCount, &pCaptureInfos,  &captureCount);
    if (result == MA_SUCCESS) {
        dmLogInfo("Mic: get_devices() returning %u capture device(s)", captureCount);
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            lua_newtable(L);

            lua_pushstring(L, "index");
            lua_pushinteger(L, static_cast<lua_Integer>(i));
            lua_settable(L, -3);

            lua_pushstring(L, "name");
            lua_pushstring(L, pCaptureInfos[i].name);
            lua_settable(L, -3);

            lua_pushstring(L, "is_default");
            lua_pushboolean(L, pCaptureInfos[i].isDefault ? 1 : 0);
            lua_settable(L, -3);

            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    } else {
        dmLogWarning("Mic: get_devices() failed to enumerate capture devices: %s", ma_result_description(result));
    }
    return 1;
}

// mic.get_current_device()
static int MicGetCurrentDevice(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    if (!g_Mic.deviceInitialized || g_Mic.device.capture.name[0] == '\0') {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, g_Mic.device.capture.name);
    return 1;
}

// mic.get_peak()
static int MicGetPeak(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    dmMutex::Lock(g_Mic.mutex);
    int peak = g_Mic.currentPeak;
    dmMutex::Unlock(g_Mic.mutex);

    lua_pushnumber(L, NormalizeSInt16(static_cast<double>(peak)));
    return 1;
}

// mic.get_rms()
static int MicGetRMS(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    dmMutex::Lock(g_Mic.mutex);
    double rms = g_Mic.currentRMS;
    dmMutex::Unlock(g_Mic.mutex);

    lua_pushnumber(L, NormalizeSInt16(rms));
    return 1;
}

// mic.get_duration()
static int MicGetDuration(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    dmMutex::Lock(g_Mic.mutex);
    const double sampleCount = g_Mic.samples.size();
    const double channels = g_Mic.channels;
    const double sampleRate = g_Mic.sampleRate;
    dmMutex::Unlock(g_Mic.mutex);

    if (sampleCount == 0.0 || channels == 0.0 || sampleRate == 0.0) {
        lua_pushnumber(L, 0.0);
    } else {
        lua_pushnumber(L, sampleCount / channels / sampleRate);
    }
    return 1;
}

static const luaL_reg Module_methods[] =
{
    {"start",                 MicStart},
    {"stop",                  MicStop},
    {"is_recording",          MicIsRecording},
    {"is_connected",          MicIsConnected},
    {"get_devices",           MicGetDevices},
    {"get_current_device",    MicGetCurrentDevice},
    {"get_peak",              MicGetPeak},
    {"get_rms",               MicGetRMS},
    {"get_duration",          MicGetDuration},
    {"request_permission",    MicRequestPermission},
    {"get_permission_status", MicGetPermissionStatus},
    {0, 0}
};

static void LuaInit(lua_State* L)
{
    int top = lua_gettop(L);
    luaL_register(L, MODULE_NAME, Module_methods);
    lua_pop(L, 1);
    assert(top == lua_gettop(L));
}

static dmExtension::Result AppInitializeMic(dmExtension::AppParams* params)
{
    g_Mic.mutex = dmMutex::New();
    
    #if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
    ClearAppleMicPermissionStatusOverride();
    #endif
    
    dmLogInfo("Mic: AppInitialize");
    if (ma_context_init(NULL, 0, NULL, &g_Mic.context) == MA_SUCCESS) {
        g_Mic.contextInitialized = true;
        dmLogInfo("Mic: Audio context initialised successfully");
        ma_log* log = ma_context_get_log(&g_Mic.context);
        const ma_result logResult = ma_log_register_callback(log, ma_log_callback_init(MiniaudioLogCallback, NULL));
        if (logResult == MA_SUCCESS) {
            dmLogInfo("Mic: Registered miniaudio log callback");
        } else {
            dmLogWarning("Mic: Failed to register miniaudio log callback: %s", ma_result_description(logResult));
        }
        
        #if defined(DM_PLATFORM_OSX) || defined(DM_PLATFORM_IOS)
        LogAppleBundleInfo();
        dmLogInfo("Mic: Initial microphone permission status is %s", AppleMicPermissionStatusToString(GetAppleMicPermissionStatus()));
        #endif
        
        LogCaptureDevices("AppInitialize");
    } else {
        g_Mic.contextInitialized = false;
        dmLogError("Mic: Failed to initialize miniaudio context");
    }
    return dmExtension::RESULT_OK;
}

static dmExtension::Result AppFinalizeMic(dmExtension::AppParams* params)
{
    if (g_Mic.isRecording) {
        dmLogWarning("Mic: AppFinalize while recording; forcing capture stop");
        g_Mic.isRecording = false;
        const ma_result stopResult = ma_device_stop(&g_Mic.device);
        if (stopResult != MA_SUCCESS) {
            dmLogWarning("Mic: Forced stop during AppFinalize failed: %s", ma_result_description(stopResult));
        }
    }
    UninitDevice();
    if (g_Mic.contextInitialized) {
        ma_context_uninit(&g_Mic.context);
        g_Mic.contextInitialized = false;
    }
    if (g_Mic.mutex) {
        dmMutex::Delete(g_Mic.mutex);
        g_Mic.mutex = 0;
    }
    dmLogInfo("Mic: AppFinalize");
    return dmExtension::RESULT_OK;
}

static dmExtension::Result InitializeMic(dmExtension::Params* params)
{
    LuaInit(params->m_L);
    dmLogInfo("Mic: registered Lua module '%s'", MODULE_NAME);
    return dmExtension::RESULT_OK;
}

static dmExtension::Result FinalizeMic(dmExtension::Params* params)
{
    dmLogInfo("Mic: Finalize");
    return dmExtension::RESULT_OK;
}

DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitializeMic, AppFinalizeMic, InitializeMic, 0, 0, FinalizeMic)
