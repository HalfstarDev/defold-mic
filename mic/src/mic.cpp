#define EXTENSION_NAME Mic
#define LIB_NAME "Mic"
#define MODULE_NAME "mic"

#include <dmsdk/sdk.h>
#include <dmsdk/dlib/mutex.h>

#include <vector>
#include <cstring>
#include <cmath>
#include <cstdlib>

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
    dmMutex::HMutex       mutex;
};

static MicState g_Mic;

static double NormalizeSInt16(double x)
{
    const double n = x / 32767.0;
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

static void UninitDevice()
{
    if (g_Mic.deviceInitialized) {
        ma_device_uninit(&g_Mic.device);
        g_Mic.deviceInitialized = false;
    }
}

static void CaptureCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    (void)pOutput;
    if (!pInput) return;

    MicState* mic = static_cast<MicState*>(pDevice->pUserData);

    dmMutex::Lock(mic->mutex);

    if (!mic->isRecording) {
        dmMutex::Unlock(mic->mutex);
        return;
    }

    size_t totalSamplesInCallback = static_cast<size_t>(frameCount) * mic->channels;
    size_t toAdd = totalSamplesInCallback;

    if (mic->maxSamples > 0) {
        const size_t already = mic->samples.size();
        if (already >= static_cast<size_t>(mic->maxSamples)) {
            dmMutex::Unlock(mic->mutex);
            return;
        }
        const size_t remaining = static_cast<size_t>(mic->maxSamples) - already;
        if (toAdd > remaining) {
            toAdd = remaining;
        }
        if (toAdd == 0) {
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

    if (g_Mic.isRecording) return 0;

    if (!g_Mic.contextInitialized) {
        return luaL_error(L, "Mic: Audio context was not initialized properly");
    }

    MicOptions opts;
    opts.device_index = -1; // default device
    opts.sample_rate = DEFAULT_SAMPLE_RATE;
    opts.channels = DEFAULT_CHANNELS;
    opts.max_seconds = 0.0;
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

    UninitDevice();

    dmMutex::Lock(g_Mic.mutex);
    g_Mic.samples.clear();
    g_Mic.currentRMS = 0.0;
    g_Mic.currentPeak = 0;
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
            return luaL_error(L, "Mic: Device_index %d is not available", opts.device_index);
        }

        selectedDeviceId = pCaptureInfos[opts.device_index].id;
        pDeviceId = &selectedDeviceId;
    }

    ma_device_config cfg  = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = pDeviceId;
    cfg.capture.format    = ma_format_s16;
    cfg.capture.channels  = opts.channels;
    cfg.sampleRate        = opts.sample_rate;
    cfg.dataCallback      = CaptureCallback;
    cfg.pUserData         = &g_Mic;

    if (ma_device_init(&g_Mic.context, &cfg, &g_Mic.device) != MA_SUCCESS) {
        return luaL_error(L, "Mic: Failed to initialise capture device");
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

    if (ma_device_start(&g_Mic.device) != MA_SUCCESS) {
        UninitDevice();
        return luaL_error(L, "Mic: Failed to start capture device");
    }

    g_Mic.isRecording = true;
    return 0;
}

// mic.stop()
static int MicStop(lua_State* L)
{
    DM_LUA_STACK_CHECK(L, 1);

    if (!g_Mic.isRecording) {
        lua_pushnil(L);
        return 1;
    }

    dmMutex::Lock(g_Mic.mutex);
    g_Mic.isRecording = false;
    dmMutex::Unlock(g_Mic.mutex);
    
    ma_device_stop(&g_Mic.device);

    dmMutex::Lock(g_Mic.mutex);
    std::vector<ma_int16> samples;
    samples.swap(g_Mic.samples);
    g_Mic.currentRMS = 0.0;
    g_Mic.currentPeak = 0;
    dmMutex::Unlock(g_Mic.mutex);

    if (samples.empty()) {
        lua_pushnil(L);
        return 1;
    }

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
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            lua_newtable(L);

            lua_pushstring(L, "index");
            lua_pushinteger(L, static_cast<lua_Integer>(i));
            lua_settable(L, -3);

            lua_pushstring(L, "name");
            lua_pushstring(L, pCaptureInfos[i].name);
            lua_settable(L, -3);

            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }
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
    {"start",        MicStart},
    {"stop",         MicStop},
    {"is_recording", MicIsRecording},
    {"is_connected", MicIsConnected},
    {"get_devices",  MicGetDevices},
    {"get_peak",     MicGetPeak},
    {"get_rms",      MicGetRMS},
    {"get_duration", MicGetDuration},
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
    if (ma_context_init(NULL, 0, NULL, &g_Mic.context) == MA_SUCCESS) {
        g_Mic.contextInitialized = true;
    } else {
        g_Mic.contextInitialized = false;
        dmLogError("Mic: Failed to initialize miniaudio context");
    }
    dmLogInfo("Mic: AppInitialize");
    return dmExtension::RESULT_OK;
}

static dmExtension::Result AppFinalizeMic(dmExtension::AppParams* params)
{
    if (g_Mic.isRecording) {
        g_Mic.isRecording = false;
        ma_device_stop(&g_Mic.device);
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