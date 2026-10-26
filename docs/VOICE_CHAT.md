# C-board voice chat

Open **Apps → Chat**. Choose Low, Medium, or High, tap **Talk**, speak, and tap
**Send**. **Cancel** stops capture, requests, or playback. Recordings submit after
20 seconds. The mic level animates while listening; the bar shows recording time
or actual uploaded bytes. Requests with an unknown duration show a moving waiting
indicator, and playback uses byte progress when the server supplies a length.
REC / SEND / THINK / VOICE distinguish the stages without invented percentages.
Replies use larger text and scroll vertically.

**New chat** starts a separate conversation and resets its session meter while
keeping previous saved chats. **Recents** opens up to four conversations, newest
first. Tap a row to read and continue it. Each keeps the last two compact text
turns; long text is marked as a saved excerpt. **DEL → Delete** removes that
conversation and frees its slot; **Keep** cancels deletion. A full history asks
you to delete a chat before starting another, rather than silently evicting one.
Saved chats survive resets and firmware updates. This starts saving new chats;
the earlier RAM-only firmware did not retain conversations across an update.

**Audio** opens speaker volume, +/- buttons, mute/unmute, microphone sensitivity
(24 / 30 / 36 dB; Normal is 30), and a spoken-replies switch. Unmute restores the
previous level. These controls also work during a turn; opening the Audio page
keeps capture/playback running. Turning spoken replies off stops current speech
and makes subsequent answers text-only. Muted/text-only replies skip a new TTS
request. Leaving the Chat screen stack cancels the active turn. The side button
returns from these subpages to Chat.

The watch contacts OpenRouter directly over Wi-Fi; a PC is needed only to install
firmware and provision credentials. Speech recognition uses Whisper Large V3
Turbo and speech output uses Kokoro (`af_heart`). Audio and full session context
stay in RAM. No recording is saved. Compact text excerpts are saved separately
in the C board's NVS, as described below. Speech is
AI-generated. Audio and text are sent to OpenRouter when a turn is submitted.

## Cost tiers

| Tier | Chat model | Target, USD cents/minute | Maximum output tokens |
| --- | --- | ---: | ---: |
| Low | `openai/gpt-4o-mini` | 1¢ | 160 |
| Medium | `openai/gpt-5.4-mini` | 5¢ | 256 |
| High | `openai/gpt-5.4` | 10¢ | 384 |

These are usage targets, not flat-rate subscriptions. All tiers keep inexpensive
speech; increasing the tier spends on the language model. The Low tier is the
default after reboot. Targets use combined captured and played audio time, with
a one-minute starting allowance. Before a turn, a conservative reservation
checks the selected tier's allowance. Replies, context, recording length, and
playback length are bounded. Text requests include provider price ceilings and
disable additional reasoning tokens. There are no automatic billable retries.
Slow provider responses are read on the existing connection up to a bounded
deadline, without sending the request again.

The session meter uses API-reported speech-recognition and chat cost when
available. Kokoro's raw PCM response does not include a usage object, so its
charge is estimated at the more expensive published fallback-provider price;
`~` marks estimates. Failed requests may still be billed upstream. OpenRouter's
usage dashboard is authoritative for actual billing and account limits.

Pricing checked on 2026-10-22:

- Whisper Turbo: $0.00000333/second at the least expensive provider, or about
  **0.020¢ per input minute**. The observed Groq alternative is about 0.067¢/min.
- Kokoro: $0.62/million characters at DeepInfra; Together is $4/million. At
  900 spoken characters/minute, this is about **0.056–0.36¢ per output minute**.
- A Low-tier conversation with one minute in, one minute out, and short model
  replies therefore has room below the requested 1¢ target at these prices.
  Language, speaking speed, history, provider routing, fees, and pricing changes
  affect the result.

References: [Whisper Turbo](https://openrouter.ai/openai/whisper-large-v3-turbo),
[Kokoro](https://openrouter.ai/hexgrad/kokoro-82m),
[GPT-4o mini](https://openrouter.ai/openai/gpt-4o-mini),
[GPT-5.4 mini](https://openrouter.ai/openai/gpt-5.4-mini),
[GPT-5.4](https://openrouter.ai/openai/gpt-5.4),
[speech API](https://openrouter.ai/docs/guides/overview/multimodal/tts),
[transcription API](https://openrouter.ai/docs/guides/overview/multimodal/stt).

## Credentials

The root `.env` is ignored by git and excluded from source archives. Use
`.env.example` for the expected fields. Set `OPENROUTER_API_KEY`, and optionally
`WIFI_SSID` / `WIFI_PASSWORD`, then run:

```powershell
& 'C:/Users/isaia/.espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe' tools/voice_setup.py --port COM4
```

The tool saves and reads back the key in the watch's NVS. It does not embed the
key in firmware, command-line arguments, or logs. Host diagnostics redact both
key and Wi-Fi commands. Model identifiers must match the priced firmware
profiles; unsupported overrides fail before provisioning.

Optional verification:

```powershell
python tools/voice_setup.py --port COM4 --test-audio
python tools/voice_setup.py --port COM4 --ask "Say hello in one short sentence."
python tools/watchctl.py --port COM4 chat status
```

The local audio test records three seconds and plays it through the speaker
without calling an API. The text test calls the selected chat model and TTS.
`chat selftest` runs a small billable acoustic test: the speaker reads a known
sentence, the physical microphone records it, and Whisper must recognize
"voice" and "watch" before the normal model and spoken reply run. A successful
test reports `selftest=1`. This also checks the path without a person speaking.
Normal Talk/Send exercises the entire microphone → Whisper → model → speaker
path. `chat cancel` and `chat clear` also work over USB. Status includes audio
sample counts, peak level, HTTP status, estimated spend, and worker stack margin;
it never exposes credentials or transcript contents.

## Hardware and builds

Saved history uses a fixed **3,892-byte payload** in a versioned, checksummed checkpoint
(`voice_hist`) with four slots and two recent turns per slot. Each saved turn
contains up to 127 input bytes and 319 reply bytes, truncated at UTF-8 boundaries;
the active session retains up to four full turns for model context. Opening a
saved conversation restores its compact context and cost meter. NVS adds its
normal transactional metadata. The payload stays bounded as conversations grow;
audio stays in RAM and saved conversations remain until explicitly deleted.

Only the UI owner commits NVS, after the audio/network worker finishes. Saves
require exact readback and checksum verification; failures remain dirty and retry
at five-second intervals. An unreadable or unsupported existing checkpoint is
not overwritten with defaults. Speaker-volume changes use the existing watch
preference; mic sensitivity and spoken-reply intent share the compact history
checkpoint. Preference writes are deferred while voice is active. Shutdown and
the checked flash checkpoint reject unsaved history. The existing paired SD
settings/checksum/fsync/lease/lock path remains intact.

This uses the [Waveshare 1.43C](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43C)
ES8311 speaker codec and ES7210 microphone ADC with Espressif's pinned
`esp_codec_dev` 1.5.10. I2S: MCLK 38, BCLK 39, WS 40, speaker data 41, mic data 42.
Audio LDO enable is GPIO18 and amplifier enable is GPIO46. Capture is 16 kHz,
16-bit mono; speaker PCM is resampled to 16 kHz while streaming. Codec power and
I2S resources are released after a turn, including cancellation and failure.
The C profile limits Wi-Fi to three static TX buffers and prefers PSRAM for
ordinary allocations larger than 1 KiB. This leaves internal DMA memory for
microphone/speaker I/O while Wi-Fi and Bluetooth are both enabled.

The C board has no IMU, RTC, or SD slot. Its saved preferences use the checked
NVS fallback. B-board SD checkpoint, lease, locking, and retry behavior stay in
place. The C panel uses its own init table and touch transform, with no B-board
software rotation. Required Solar/Orrery, home pager, sports, developer, and jobs
features remain linked. The B build exposes the Chat page's hardware requirement
without linking the C audio/network implementation.

Settings still detects external strap microphone/speaker modules independently
of the C board's built-in codecs. This UI update leaves that module detection
unchanged; the Chat Audio page controls the working built-in path directly.

USB diagnostics include `chat history` (IDs/counts only), `chat select ID`,
`chat delete ID`, `chat history flush`, `chat volume 0..100`, and
`chat audio SPEAK GAIN` (SPEAK 0/1; GAIN 24/30/36). `chat clear` starts a new
conversation while preserving saved ones. Status includes audio levels, real
progress counters, history count, pending saves, and save errors. It does not
print saved transcripts or credentials.

```powershell
tools/build_c143.ps1
# Subsequent checked, preference-preserving installation:
tools/build_c143.ps1 -Port COM4
```

The C layout preserves NVS at `0x9000..0xefff`, uses the factory app at `0x10000`
with a `0x7e0000`-byte limit, and reserves the final 64 KiB. Its old SPIFFS tail
was backed up before enlarging the application. The original legacy firmware is
saved under ignored `build/c143-before-20261022/`; the final pre-migration backup
is `build/c143-preserved-20261022/`. Migration compared the device MAC and its
boot/partition/NVS bytes in download mode before writing. No NVS erase was used.

## Validation evidence

- Base C installation: `build/c143-os-flash.log`, `build/c143-base-check.log`.
- Live OpenRouter transcription and model probes: `build/voice-api-probe.log`.
- Host budget, UTF-8, credential redaction/provisioning, preservation, and actual
  storage/OTA regression tests: `build/voice-focused-tests.log`.
- UI images: `build/voice-ui-shots/`. The simulator is offline and never reads
  the host API key. Cost-model timing is not a device frame-rate measurement.
- Separate current B build: `build/voice-b-build.log`.
- C voice build and device checks are recorded in the corresponding
  `build/c143-voice-*` logs.

The final C build passed 54 focused tests, 259 identical UI damage-reference
frames, all three live model/speaker tiers, key reload across firmware updates,
preference preservation, and cancellation with both Wi-Fi and BLE enabled.
The acoustic self-test captured 3.35 seconds at a peak of 5447, recognized the
expected words through Whisper, then generated and played the reply. Total
speaker output (reference plus reply) was 6.425 seconds, with an upper estimated
session cost of $0.00034056 (**0.0341¢**). See
`build/c143-voice-selftest.json` and `build/c143-voice-device-verification.json`.

### Chat UI and saved-history update — 2026-10-23

The update passed **54 focused tests**, the complete Audio/Recents/new/continue/
delete touch flow, and **1,704 identical incremental/full-repaint frames**. Both
board variants built from the same current source tree; the C image was flashed
through the checked preference-preserving helper with NVS excluded.

All **33 device checks passed**. Live controls changed mic sensitivity during
3 seconds of local capture and volume during its playback. One Low-tier test
request generated and played a 1.1-second response, with upper estimated cost
$0.00003615. The compact text checkpoint survived a ROM reset, was selected on
the watch, then deleted through the confirmation screen; a second reset verified
the deletion. The synthetic test conversation was removed, existing preference
bytes and the API key were preserved, and TE remains off. No display fault or
allocation failure was observed. This run checked codec capture/writes and a live
spoken response; it did not repeat the earlier acoustic speaker-to-mic loop test.

See [structured verification](performance/C143_CHAT_UI_20261023.json),
`build/c143-chat-ui-shots/` (including listening/working GIFs), and
`build/c143-chat-ui-device.json` for checks and evidence hashes.
