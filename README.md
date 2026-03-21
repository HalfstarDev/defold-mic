![banner](/docs/images/banner.jpg)
# Defold-Mic

A Defold extension for microphone recording via [miniaudio](https://miniaud.io/).

## Installation

This asset can be added as a [library dependency](https://defold.com/manuals/libraries/#setting-up-library-dependencies) in your project.

Add this link to your dependencies: https://github.com/HalfstarDev/defold-mic/archive/master.zip

## Compatibility

This extension works without any restrictions on the following platforms:
* Windows
* Linux
* HTML5

### Android

For using the microphone on Android devices, you have to first get the permission `android.permission.RECORD_AUDIO` from the user, for example using [extension-permissions](https://github.com/defold/extension-permissions).

### macOS

On macOS, you have to first get the permission to use the microphone, for example using `mic.request_permission()`.

Currently the extension does not seem to work on debug builds from the Defold editor, but only in release bundles. This is likely because Defold is missing `NSMicrophoneUsageDescription` in its manifest. So for debugging on macOS, you need to use a release bundle, or a HTML5 build.

### iOS

This extension has not been tested on iOS yet. It should work with `mic.request_permission()`. If you use it on iOS, please tell me the results, positive or negaive.

## Usage

#### mic.start([options])
Begin audio capture. Accepts an optional Lua table with capture settings.

| Key | Type | Description |
|-----|------|-------------|
| `device_index` | number | Zero-based index of the capture device to use, as returned by `mic.get_devices()`. Defaults to the system default device. |
| `sample_rate`  | number | Sample rate in Hz. Usually `16000`, `22050`, `44100`, or `48000`. Defaults to `16000`. |
| `channels`     | number | Number of audio channels. Must be `1` (mono) or `2` (stereo). Defaults to `1`. |
| `max_seconds`  | number | Maximum number of seconds to buffer. Once reached, incoming audio is discarded. `0` means unlimited. Defaults to `60` seconds. |
| `noise_gate_threshold` | number | Amplitude threshold in range `[0.0, 1.0]`. Frames whose peak amplitude falls below are silenced. Defaults to `0`. |

```lua
mic.start()
```

```lua
mic.start({
    sample_rate = 16000,
    channels = 1,
    max_seconds = 10,
    noise_gate_threshold = 0.02,
})
```

#### mic.stop([options])
Stop audio capture and return the recorded audio as a WAV byte string. Optional table `options`, with only possible key `normalize_peak` to normalize recorded audio to, in (0.0, 1.0]. Returns `nil` if nothing was recorded.

```lua
mic.start()
-- ...
local wav = mic.stop()
if wav then
    local path = go.get("#voice", "sound")
    resource.set_sound(path, wav)
end
```

```lua
local wav = mic.stop({normalize_peak = 0.8})
local f = io.open("recording.wav", "wb")
if f then
    f:write(wav)
    f:close()
end
```

#### mic.is_recording()
Check whether the microphone is currently capturing audio. Returns `true` if recording, `false` otherwise.

```lua
if mic.is_recording() then
    print("Currently recording.")
end
```

#### mic.is_connected()
Check whether at least one capture device is available on the system. Returns `true` if one or more capture devices are available, `false` otherwise.

```lua
if not mic.is_connected() then
    print("No microphone found!")
end
```

#### mic.get_devices()
Enumerate all available capture devices. Returns a list of device info tables, each with keys `index` and `name`. (On HTML5, there will likely only be the microphone selected by the user for their browser.)

```lua
local devices = mic.get_devices()
pprint(devices)

mic.start({ device_index = devices[2].index })
```

#### mic.get_current_device()
Returns the name of the current capture device.

```lua
local device = mic.get_current_device()
```

#### mic.get_peak()
Get the peak amplitude of the current block. Returns a normalised value in the range `[0.0, 1.0]`, or `0.0` if not recording.

```lua
mic.start()

function update(self, dt)
    print("Peak amplitude:", mic.get_peak())
end
```

#### mic.get_rms()
Get the RMS (root mean square) volume of the current block. Returns a normalised value in the range `[0.0, 1.0]`, or `0.0` if not recording.

```lua
mic.start()

function update(self, dt)
    if mic.get_rms() > 0.1 then
        print("Somebody is speaking!")
    end
end
```

#### mic.get_duration()
Get the duration of audio buffered so far in the current recording. Returns duration in seconds, or `0.0` if not recording.

```lua
mic.start({ max_seconds = 30 })

function update(self, dt)
    print("Time left to record:", 30 - mic.get_duration())
end
```

#### mic.request_permission()
Request permission to use the microphone. (macOS and iOS only)

```lua
mic.request_permission()
```

#### mic.get_permission_status()
Get the current status of the permission to use the microphone. (macOS and iOS only)

```
APPLE_MIC_PERMISSION_UNAVAILABLE    = -1,
APPLE_MIC_PERMISSION_NOT_DETERMINED = 0,
APPLE_MIC_PERMISSION_RESTRICTED     = 1,
APPLE_MIC_PERMISSION_DENIED         = 2,
APPLE_MIC_PERMISSION_GRANTED        = 3,
```

```lua
local status = mic.get_permission_status()
```
