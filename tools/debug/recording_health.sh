#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER_ID="${ASFW_DRIVER_ID:-com.chrisizatt.ASFWLocal.ASFWDriver}"
APP_PATH="${ASFW_LOCAL_APP:-/Applications/ASFWLocal.app}"
LOG_WINDOW="${ASFW_LOG_WINDOW:-20m}"
OUT_DIR="${ASFW_RECORDING_HEALTH_DIR:-/tmp/asfw-recording-health-$(date +%Y%m%d-%H%M%S)}"
WAV_PATH=""

usage() {
  cat <<USAGE
Usage: $0 [--out DIR] [--log-window 20m] [--wav PATH]

Captures Alesis/ASFW recording-health state for a Logic capture pass:
  - System Extension and ASFW process state
  - installed/staged bundle identifiers, versions, and CDHashes
  - IORegistry and CoreAudio visibility
  - focused ASFW/CoreAudio/Logic logs
  - DICE register/timing snapshot extracted from the same log window
  - optional WAV metadata and dropout scan for exported PCM WAV files
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      OUT_DIR="$2"
      shift 2
      ;;
    --log-window)
      LOG_WINDOW="$2"
      shift 2
      ;;
    --wav)
      WAV_PATH="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

mkdir -p "$OUT_DIR"

capture() {
  local name="$1"
  shift
  {
    printf 'Command:'
    printf ' %q' "$@"
    printf '\n\n'
    "$@"
  } >"$OUT_DIR/$name" 2>&1 || true
}

capture_shell() {
  local name="$1"
  shift
  {
    printf 'Command: %s\n\n' "$*"
    "$@"
  } >"$OUT_DIR/$name" 2>&1 || true
}

cat >"$OUT_DIR/README.txt" <<README
Alesis MultiMix recording-health capture
Created: $(date)
Driver ID: $DRIVER_ID
App path: $APP_PATH
Log window: $LOG_WINDOW

Recommended live test setup:
- Logic at 48 kHz
- Alesis MultiMix as input
- non-Alesis output
- software monitoring off
- 256-frame buffer
- 10-minute recording on inputs 1 and 2 with steady signal

Acceptance notes:
- no Logic sample-rate warning
- no ASFW driver crash/restart
- no DBC/CIP errors after startup
- no RX zero-fill after startup
- transport rate remains close to 48 kHz
- exported audio duration matches wall time
- no detected 10 ms dropout on active channels

Key files:
- dice_register_snapshot.txt: DICE section/global/stream registers plus host ZTS/RX queue/decoded-zero markers
- focused_logs.txt: raw ASFW/CoreAudio/Logic log window used for the snapshot
README

"$ROOT_DIR/tools/debug/probe_local_state.sh" >"$OUT_DIR/local_state.txt" 2>&1 || true

capture "systemextensions.txt" systemextensionsctl list
capture "asfw_processes.txt" pgrep -fl "ASFW|ASFWDriver|${DRIVER_ID}"
capture "coreaudio_audio.txt" system_profiler SPAudioDataType -detailLevel full
capture "firewire_topology.txt" system_profiler SPThunderboltDataType SPFireWireDataType SPPCIDataType -detailLevel full
capture "ioreg_asfw_audio_nub.txt" ioreg -p IOService -l -w0 -r -c ASFWAudioNub
capture "ioreg_asfw_audio_driver.txt" ioreg -p IOService -l -w0 -r -c ASFWAudioDriver
capture "ioreg_asfw_driver.txt" ioreg -p IOService -l -w0 -r -c ASFWDriver

LOG_PREDICATE='eventMessage CONTAINS[c] "ASFW" OR eventMessage CONTAINS[c] "Alesis" OR eventMessage CONTAINS[c] "MultiMix" OR eventMessage CONTAINS[c] "DICE" OR eventMessage CONTAINS[c] "RxStats" OR eventMessage CONTAINS[c] "IR RX HEALTH" OR eventMessage CONTAINS[c] "IR SYT" OR eventMessage CONTAINS[c] "RX startup" OR eventMessage CONTAINS[c] "ZERO-FILL" OR eventMessage CONTAINS[c] "RX QUEUE" OR eventMessage CONTAINS[c] "rxq/producer-drop" OR eventMessage CONTAINS[c] "producer-drop" OR eventMessage CONTAINS[c] "isoch length clamp" OR eventMessage CONTAINS[c] "PCM slot payload fallback" OR eventMessage CONTAINS[c] "IO callback" OR eventMessage CONTAINS[c] "IO-RX" OR eventMessage CONTAINS[c] "HALS_Device" OR eventMessage CONTAINS[c] "Logic" OR eventMessage CONTAINS[c] "sample rate" OR eventMessage CONTAINS[c] "ZTS" OR eventMessage CONTAINS[c] "RX transport rebase" OR eventMessage CONTAINS[c] "RX high-water trim" OR eventMessage CONTAINS[c] "RX high-water slew" OR eventMessage CONTAINS[c] "RX high-water emergency-trim" OR eventMessage CONTAINS[c] "rx/high-water-trim" OR eventMessage CONTAINS[c] "rx/high-water-slew" OR eventMessage CONTAINS[c] "rx/high-water-emergency-trim" OR eventMessage CONTAINS[c] "RX QUEUE UNDERREAD" OR eventMessage CONTAINS[c] "decoded all-zero run" OR eventMessage CONTAINS[c] "IO-TX" OR eventMessage CONTAINS[c] "CLK" OR eventMessage CONTAINS[c] "DBC" OR eventMessage CONTAINS[c] "CIP"'

capture_shell "focused_logs.txt" /usr/bin/log show --last "$LOG_WINDOW" --style syslog --predicate "$LOG_PREDICATE"

{
  printf 'Alesis MultiMix DICE register and timing snapshot\n'
  printf 'Created: %s\n' "$(date)"
  printf 'Source log: %s\n\n' "$OUT_DIR/focused_logs.txt"

  printf '== DICE register reads ==\n'
  grep -E 'DICE register snapshot|activePcm|activeAm824Slots|disabledPcm|ReadGeneralSections|ReadGlobalState|ReadTxStreamConfig|ReadRxStreamConfig|Global:|TX Streams|RX Streams|TX\[[0-9]+\]|RX\[[0-9]+\]|PrepareDuplex48k: .*clock|ConfirmDuplex48kStart' \
    "$OUT_DIR/focused_logs.txt" || true

  printf '\n== Host transport / queue timing ==\n'
  grep -E 'ZTS|IR SYT|IR RX HEALTH|RX transport rebase|RX high-water (trim|slew|emergency-trim)|rx/high-water-(trim|slew|emergency-trim)|IO-RX|IO-TX|RxStats|RX startup|ZERO-FILL|RX QUEUE|RX QUEUE UNDERREAD|decoded all-zero run|rxq/producer-drop|producer-drop|DBC|CIP|cipDbs|queueCh|host queue larger' \
    "$OUT_DIR/focused_logs.txt" || true
} >"$OUT_DIR/dice_register_snapshot.txt"

if [[ -n "$WAV_PATH" ]]; then
  if [[ -f "$WAV_PATH" ]]; then
    capture "wav_afinfo.txt" afinfo "$WAV_PATH"
    if command -v python3 >/dev/null 2>&1; then
      python3 - "$WAV_PATH" >"$OUT_DIR/wav_dropouts.txt" 2>&1 <<'PY' || true
import sys
import wave

path = sys.argv[1]
dropout_ms = 10.0
active_threshold_ratio = 10 ** (-54 / 20)
silence_threshold_ratio = 10 ** (-72 / 20)

def decode_sample(data, offset, width):
    if width == 1:
        return data[offset] - 128
    if width == 2:
        return int.from_bytes(data[offset:offset + 2], "little", signed=True)
    if width == 3:
        raw = data[offset:offset + 3]
        sign = b"\xff" if raw[2] & 0x80 else b"\x00"
        return int.from_bytes(raw + sign, "little", signed=True)
    if width == 4:
        return int.from_bytes(data[offset:offset + 4], "little", signed=True)
    raise ValueError(f"unsupported sample width: {width}")

with wave.open(path, "rb") as wav:
    channels = wav.getnchannels()
    rate = wav.getframerate()
    frames = wav.getnframes()
    width = wav.getsampwidth()
    frame_bytes = channels * width
    peak = (1 << ((8 * width) - 1)) - 1 if width > 1 else 127
    active_threshold = max(1, int(peak * active_threshold_ratio))
    silence_threshold = max(1, int(peak * silence_threshold_ratio))
    dropout_frames = max(1, int(rate * dropout_ms / 1000.0))

    active = [False] * channels
    current_silent = [0] * channels
    longest_silent = [0] * channels
    position = 0

    while True:
        chunk = wav.readframes(8192)
        if not chunk:
            break
        frame_count = len(chunk) // frame_bytes
        for frame_index in range(frame_count):
            base = frame_index * frame_bytes
            for channel in range(channels):
                sample = abs(decode_sample(chunk, base + channel * width, width))
                if sample >= active_threshold:
                    active[channel] = True
                if sample <= silence_threshold:
                    current_silent[channel] += 1
                else:
                    if current_silent[channel] > longest_silent[channel]:
                        longest_silent[channel] = current_silent[channel]
                    current_silent[channel] = 0
        position += frame_count

    for channel in range(channels):
        if current_silent[channel] > longest_silent[channel]:
            longest_silent[channel] = current_silent[channel]

    print(f"path={path}")
    print(f"channels={channels} sampleRate={rate} frames={frames} durationSeconds={frames / rate:.3f}")
    print(f"dropoutThresholdFrames={dropout_frames} activeThreshold={active_threshold} silenceThreshold={silence_threshold}")
    any_dropouts = False
    for channel in range(channels):
        if not active[channel]:
            continue
        longest_ms = 1000.0 * longest_silent[channel] / rate
        failed = longest_silent[channel] >= dropout_frames
        any_dropouts = any_dropouts or failed
        status = "FAIL" if failed else "ok"
        print(f"channel={channel + 1} active=yes longestSilentMs={longest_ms:.3f} status={status}")
    print(f"summary={'FAIL' if any_dropouts else 'ok'}")
PY
      python3 - "$WAV_PATH" >"$OUT_DIR/wav_exact_zero_runs.txt" 2>&1 <<'PY' || true
import sys
import wave

path = sys.argv[1]
min_report_ms = 1.0
active_threshold_ratio = 10 ** (-54 / 20)

def decode_sample(data, offset, width):
    if width == 1:
        return data[offset] - 128
    if width == 2:
        return int.from_bytes(data[offset:offset + 2], "little", signed=True)
    if width == 3:
        raw = data[offset:offset + 3]
        sign = b"\xff" if raw[2] & 0x80 else b"\x00"
        return int.from_bytes(raw + sign, "little", signed=True)
    if width == 4:
        return int.from_bytes(data[offset:offset + 4], "little", signed=True)
    raise ValueError(f"unsupported sample width: {width}")

with wave.open(path, "rb") as wav:
    channels = wav.getnchannels()
    rate = wav.getframerate()
    frames = wav.getnframes()
    width = wav.getsampwidth()
    frame_bytes = channels * width
    peak = (1 << ((8 * width) - 1)) - 1 if width > 1 else 127
    active_threshold = max(1, int(peak * active_threshold_ratio))
    min_report_frames = max(1, int(rate * min_report_ms / 1000.0))
    dropout_frames = max(1, int(rate * 10.0 / 1000.0))

    active = [False] * channels
    zero_run = [0] * channels
    zero_run_start = [0] * channels
    zero_runs = [0] * channels
    zero_frames = [0] * channels
    zero_runs_1ms = [0] * channels
    zero_runs_10ms = [0] * channels
    longest = [(0, 0)] * channels
    all_zero_run = [0]
    all_zero_start = [0]
    all_zero_runs = [0]
    all_zero_frames = [0]
    all_zero_runs_1ms = [0]
    all_zero_runs_10ms = [0]
    all_zero_longest = [(0, 0)]
    position = 0

    def finish_channel_run(channel, end_position):
        del end_position
        run = zero_run[channel]
        if run <= 0:
            return
        zero_runs[channel] += 1
        zero_frames[channel] += run
        if run >= min_report_frames:
            zero_runs_1ms[channel] += 1
        if run >= dropout_frames:
            zero_runs_10ms[channel] += 1
        if run > longest[channel][0]:
            longest[channel] = (run, zero_run_start[channel])
        zero_run[channel] = 0

    def finish_all_zero_run():
        if all_zero_run[0] <= 0:
            return
        all_zero_runs[0] += 1
        all_zero_frames[0] += all_zero_run[0]
        if all_zero_run[0] >= min_report_frames:
            all_zero_runs_1ms[0] += 1
        if all_zero_run[0] >= dropout_frames:
            all_zero_runs_10ms[0] += 1
        if all_zero_run[0] > all_zero_longest[0][0]:
            all_zero_longest[0] = (all_zero_run[0], all_zero_start[0])
        all_zero_run[0] = 0

    while True:
        chunk = wav.readframes(8192)
        if not chunk:
            break
        frame_count = len(chunk) // frame_bytes
        for frame_index in range(frame_count):
            base = frame_index * frame_bytes
            frame_position = position + frame_index
            frame_all_zero = True
            for channel in range(channels):
                sample = decode_sample(chunk, base + channel * width, width)
                if abs(sample) >= active_threshold:
                    active[channel] = True
                if sample == 0:
                    if zero_run[channel] == 0:
                        zero_run_start[channel] = frame_position
                    zero_run[channel] += 1
                else:
                    finish_channel_run(channel, frame_position)
                    frame_all_zero = False

            if frame_all_zero:
                if all_zero_run[0] == 0:
                    all_zero_start[0] = frame_position
                all_zero_run[0] += 1
            else:
                finish_all_zero_run()
        position += frame_count

    for channel in range(channels):
        finish_channel_run(channel, position)
    finish_all_zero_run()

    print(f"path={path}")
    print(f"channels={channels} sampleRate={rate} frames={frames} durationSeconds={frames / rate:.3f}")
    print(f"minReportFrames={min_report_frames} dropoutThresholdFrames={dropout_frames} activeThreshold={active_threshold}")
    print(
        "allChannels "
        f"runs={all_zero_runs[0]} zeroFrames={all_zero_frames[0]} "
        f"runsAtLeast1ms={all_zero_runs_1ms[0]} runsAtLeast10ms={all_zero_runs_10ms[0]} "
        f"longestFrames={all_zero_longest[0][0]} longestMs={1000.0 * all_zero_longest[0][0] / rate:.3f} "
        f"longestStartSeconds={all_zero_longest[0][1] / rate:.6f}"
    )
    for channel in range(channels):
        if not active[channel]:
            continue
        run, start = longest[channel]
        print(
            f"channel={channel + 1} active=yes runs={zero_runs[channel]} "
            f"zeroFrames={zero_frames[channel]} runsAtLeast1ms={zero_runs_1ms[channel]} "
            f"runsAtLeast10ms={zero_runs_10ms[channel]} longestFrames={run} "
            f"longestMs={1000.0 * run / rate:.3f} longestStartSeconds={start / rate:.6f}"
        )
PY
    fi
  else
    echo "WAV path not found: $WAV_PATH" >"$OUT_DIR/wav_error.txt"
  fi
fi

echo "Recording health capture written to: $OUT_DIR"
