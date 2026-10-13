#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/pro40-sequoia
# Keep assertions and sanitizers in check profiles; optimization must not disable
# validation. The real-time packet path should not use the compiler's -O0 default.
extra=(-std=c++20 -O2)
longRun=0
wrapRun=0
enduranceRun=0
restartRun=0
sequenceRun=0
entryCheck=0
workerRun=0
workerStub=0
managedStub=0
packageStub=0
managedReal=0
eightSix=0
sustained=0
sustainedInput=0
livePlayback=0
duplexWorker=0
native=(tools/pro40-sequoia/NativeFireWire.cpp)
entry=tools/pro40-sequoia/DuplexCaptureProbe.cpp
if [[ "${1:-}" == --managed-duplex-worker-real-inspect || "${1:-}" == --managed-duplex-worker-stub || "${1:-}" == --reuse-queue-worker || "${1:-}" == --reuse-queue-managed-worker-real-inspect || "${1:-}" == --reuse-queue-worker-stub || "${1:-}" == --reuse-queue-managed-worker-package-stub || "${1:-}" == --reuse-queue-managed-worker-stub || "${1:-}" == --reuse-queue-worker-endurance || "${1:-}" == --reuse-queue-worker-eight-six || "${1:-}" == --reuse-queue-worker-eight-six-stub || "${1:-}" == --sustained-worker || "${1:-}" == --sustained-worker-stub || "${1:-}" == --sustained-input-worker || "${1:-}" == --sustained-input-worker-stub || "${1:-}" == --live-playback-worker || "${1:-}" == --live-playback-worker-stub ]]; then
  [[ $# == 1 ]] || exit 64
  workerRun=1
  entry=tools/pro40-sequoia/worker/NativeWorker.cpp
  if [[ "$1" == --managed-duplex-worker-real-inspect || "$1" == --managed-duplex-worker-stub ]]; then
    duplexWorker=1
    extra+=(-DPRO40_DUPLEX_WORKER -DPRO40_MANAGED_WORKER)
  fi
  if [[ "$1" == --live-playback-worker || "$1" == --live-playback-worker-stub || "$duplexWorker" == 1 ]]; then
    livePlayback=1
    extra+=(-DPRO40_LIVE_PLAYBACK)
  fi
  if [[ "$1" == --sustained-input-worker || "$1" == --sustained-input-worker-stub || "$livePlayback" == 1 ]]; then
    sustainedInput=1
    extra+=(-DPRO40_SUSTAINED_INPUT)
  fi
  if [[ "$1" == --sustained-worker || "$1" == --sustained-worker-stub || "$sustainedInput" == 1 ]]; then
    sustained=1
    extra+=(-DPRO40_DMA_SUSTAINED)
  fi
  if [[ "$1" == --reuse-queue-worker-eight-six || "$1" == --reuse-queue-worker-eight-six-stub ]]; then
    eightSix=1
    extra+=(-DPRO40_EIGHT_SIX_CHECK)
  fi
  # Build the managed hardware candidate; this script never executes it.
  if [[ "$1" == --reuse-queue-managed-worker-real-inspect ]]; then
    managedReal=1
    extra+=(-DPRO40_MANAGED_WORKER)
  fi
  if [[ "$1" == --reuse-queue-managed-worker-package-stub ]]; then packageStub=1; fi
  if [[ "$1" == --reuse-queue-managed-worker-stub || "$packageStub" == 1 ]]; then
    managedStub=1
    extra+=(-DPRO40_MANAGED_WORKER)
  fi
  if [[ "$1" == --managed-duplex-worker-stub || "$1" == --reuse-queue-worker-stub || "$1" == --reuse-queue-worker-eight-six-stub || "$1" == --sustained-worker-stub || "$1" == --sustained-input-worker-stub || "$1" == --live-playback-worker-stub || "$managedStub" == 1 ]]; then
    workerStub=1
    native=(tools/pro40-sequoia/worker/NativeStub.cpp)
    extra+=(-DPRO40_WORKER_TEST_LOCKS)
    if [[ "$packageStub" == 0 ]]; then extra+=(-g -fsanitize=address,undefined); fi
  fi
  if [[ "$sustained" == 1 ]]; then
    set -- --reuse-queue
  elif [[ "$1" == --reuse-queue-worker-endurance || "$eightSix" == 1 ]]; then
    set -- --reuse-queue-endurance
  else
    set -- --reuse-queue-wrap
  fi
elif [[ "${1:-}" == --reuse-queue-peer-check || "${1:-}" == --reuse-queue-peer-check-tsan ]]; then
  [[ $# == 1 ]] || exit 64
  entryCheck=2
  native=()
  entry=tools/pro40-sequoia/NativePeerRunnerChecks.cpp
  if [[ "$1" == --reuse-queue-peer-check-tsan ]]; then
    extra+=(-g -fsanitize=thread)
  else
    extra+=(-g -fsanitize=address,undefined)
  fi
  set -- --reuse-queue-wrap
elif [[ "${1:-}" == --sustained-entry-check || "${1:-}" == --sustained-input-entry-check || "${1:-}" == --live-playback-entry-check ]]; then
  [[ $# == 1 ]] || exit 64
  entryCheck=3
  native=()
  entry=tools/pro40-sequoia/SustainedEntryChecks.cpp
  extra+=(-DPRO40_DMA_SUSTAINED -g -fsanitize=address,undefined)
  if [[ "$1" == --sustained-input-entry-check || "$1" == --live-playback-entry-check ]]; then
    extra+=(-DPRO40_SUSTAINED_INPUT)
    entryCheck=4
    if [[ "$1" == --live-playback-entry-check ]]; then
      extra+=(-DPRO40_LIVE_PLAYBACK)
      entryCheck=5
    fi
  fi
  set -- --reuse-queue
elif [[ "${1:-}" == --reuse-queue-entry-check ]]; then
  [[ $# == 1 ]] || exit 64
  entryCheck=1
  native=()
  entry=tools/pro40-sequoia/NativeSessionEntryChecks.cpp
  extra+=(-g -fsanitize=address,undefined)
  set -- --reuse-queue-wrap
elif [[ "${1:-}" == --reuse-queue-sequence ]]; then
  [[ $# == 1 ]] || exit 64
  sequenceRun=1
  entry=tools/pro40-sequoia/NativeSessionSequenceProbe.cpp
  set -- --reuse-queue-wrap
elif [[ "${1:-}" == --reuse-queue-restart ]]; then
  [[ $# == 1 ]] || exit 64
  restartRun=1
  entry=tools/pro40-sequoia/NativeSessionRestartCheck.cpp
  set -- --reuse-queue-wrap
fi
if [[ "${1:-}" == --reuse-queue-endurance ]]; then
  [[ $# == 1 ]] || exit 64
  enduranceRun=1
  extra+=(-DPRO40_DMA_CYCLE_BUDGET=262144)
  set -- --reuse-queue
elif [[ "${1:-}" == --reuse-queue-wrap ]]; then
  [[ $# == 1 ]] || exit 64
  wrapRun=1
  extra+=(-DPRO40_DMA_CYCLE_BUDGET=65536)
  set -- --reuse-queue
elif [[ "${1:-}" == --reuse-queue-long ]]; then
  [[ $# == 1 ]] || exit 64
  longRun=1
  extra+=(-DPRO40_DMA_CYCLE_BUDGET=32768)
  set -- --reuse-queue
fi
name=pro40-transmit-return
if [[ "${1:-}" == --refresh ]]; then
  extra+=(-DPRO40_DMA_TX_REFRESH)
  name=pro40-transmit-refresh
elif [[ "${1:-}" == --measured ]]; then
  extra+=(-DPRO40_DMA_TX_REFRESH -DPRO40_DMA_TX_MEASURED)
  name=pro40-transmit-measured
elif [[ "${1:-}" == --handoffs ]]; then
  extra+=(-DPRO40_DMA_TX_HANDOFFS)
  name=pro40-transmit-handoffs
elif [[ "${1:-}" == --measured-handoffs ]]; then
  extra+=(-DPRO40_DMA_TX_HANDOFFS -DPRO40_DMA_TX_REFRESH -DPRO40_DMA_TX_MEASURED)
  name=pro40-transmit-measured-handoffs
elif [[ "${1:-}" == --full-head ]]; then
  extra+=(-DPRO40_DMA_TX_REFRESH -DPRO40_DMA_TX_MEASURED -DPRO40_DMA_TX_FULL_HEAD)
  name=pro40-transmit-full-head
elif [[ "${1:-}" == --refill ]]; then
  extra+=(-DPRO40_DMA_TX_REFRESH -DPRO40_DMA_TX_MEASURED -DPRO40_DMA_TX_FULL_HEAD -DPRO40_DMA_TX_HANDOFFS -DPRO40_DMA_TX_REFILL)
  name=pro40-transmit-refill
elif [[ "${1:-}" == --refill-long ]]; then
  extra+=(-DPRO40_DMA_TX_REFRESH -DPRO40_DMA_TX_MEASURED -DPRO40_DMA_TX_FULL_HEAD -DPRO40_DMA_TX_HANDOFFS -DPRO40_DMA_TX_REFILL -DPRO40_DMA_TX_LONG_REFILL)
  name=pro40-transmit-refill-long
elif [[ "${1:-}" == --refill-lock || "${1:-}" == --reuse-lock || "${1:-}" == --reuse-extended || "${1:-}" == --reuse-compact || "${1:-}" == --reuse-second || "${1:-}" == --reuse-queue ]]; then
  extra+=(-DPRO40_DMA_TX_REFRESH -DPRO40_DMA_TX_MEASURED -DPRO40_DMA_TX_FULL_HEAD -DPRO40_DMA_TX_HANDOFFS -DPRO40_DMA_TX_REFILL -DPRO40_DMA_TX_LONG_REFILL -DPRO40_DMA_TX_LOCK)
  name=pro40-transmit-refill-lock
  if [[ "$1" == --reuse-lock || "$1" == --reuse-extended || "$1" == --reuse-compact || "$1" == --reuse-second || "$1" == --reuse-queue ]]; then
    extra+=(-DPRO40_DMA_BIDIRECTIONAL_REUSE -DPRO40_DMA_RX_BRANCH_STOP -DPRO40_DMA_RX_BRANCH_EXTEND -DPRO40_DMA_RX_REUSE -DPRO40_DMA_RX_DESCRIPTOR_REUSE -DPRO40_DMA_RX_HANDOFFS)
    name=pro40-bidirectional-reuse
    if [[ "$1" == --reuse-extended || "$1" == --reuse-compact || "$1" == --reuse-second || "$1" == --reuse-queue ]]; then
      extra+=(-DPRO40_DMA_EXTENDED_REUSE)
      name=pro40-bidirectional-extended
      if [[ "$1" == --reuse-compact || "$1" == --reuse-second || "$1" == --reuse-queue ]]; then
        extra+=(-DPRO40_DMA_COMPACT_NATIVE)
        name=pro40-bidirectional-compact
        if [[ "$1" == --reuse-second || "$1" == --reuse-queue ]]; then
          extra+=(-DPRO40_DMA_ONE_SECOND)
          name=pro40-bidirectional-second
          if [[ "$1" == --reuse-queue ]]; then
            extra+=(-DPRO40_DMA_SOURCE_QUEUE)
            name=pro40-source-queue
          fi
        fi
      fi
    fi
  fi
elif [[ $# != 0 ]]; then
  exit 64
fi
if [[ $longRun == 1 ]]; then name=pro40-source-queue-long; fi
if [[ $wrapRun == 1 ]]; then name=pro40-source-queue-wrap; fi
if [[ $enduranceRun == 1 ]]; then name=pro40-source-queue-endurance; fi
if [[ $restartRun == 1 ]]; then name=pro40-session-restart; fi
if [[ $sequenceRun == 1 ]]; then name=pro40-session-sequence; fi
if [[ $entryCheck == 1 ]]; then name=pro40-session-entry-checks; fi
if [[ $entryCheck == 2 ]]; then name=pro40-peer-runner-checks; fi
if [[ $entryCheck == 3 ]]; then name=pro40-sustained-entry-checks; fi
if [[ $entryCheck == 4 ]]; then name=pro40-sustained-input-entry-checks; fi
if [[ $entryCheck == 5 ]]; then name=pro40-live-playback-entry-checks; fi
if [[ $workerRun == 1 ]]; then name=pro40-native-worker; fi
if [[ $workerStub == 1 ]]; then name=pro40-native-worker-stub; fi
if [[ $managedStub == 1 ]]; then name=pro40-managed-worker-stub; fi
if [[ $packageStub == 1 ]]; then name=pro40-managed-worker-package-stub; fi
if [[ $managedReal == 1 ]]; then name=pro40-managed-worker-real-inspect; fi
if [[ $workerRun == 1 && $enduranceRun == 1 ]]; then name=pro40-native-worker-endurance; fi
if [[ $eightSix == 1 ]]; then
  name=pro40-eight-six-worker
  if [[ $workerStub == 1 ]]; then name=pro40-eight-six-worker-stub; fi
fi
if [[ $sustained == 1 ]]; then
  name=pro40-sustained-worker
  if [[ $workerStub == 1 ]]; then name=pro40-sustained-worker-stub; fi
fi
if [[ $sustainedInput == 1 ]]; then
  name=pro40-sustained-input-worker
  if [[ $workerStub == 1 ]]; then name=pro40-sustained-input-worker-stub; fi
fi
if [[ $livePlayback == 1 ]]; then
  name=pro40-live-playback-worker
  if [[ $workerStub == 1 ]]; then name=pro40-live-playback-worker-stub; fi
fi
if [[ $duplexWorker == 1 ]]; then
  name=pro40-managed-duplex-worker-real-inspect
  if [[ $workerStub == 1 ]]; then name=pro40-managed-duplex-worker-stub; fi
fi
native+=("$entry")
xcrun clang++ "${extra[@]}" -Wall -Wextra -Werror \
  -DPRO40_RESOURCE_PROBE -DPRO40_DMA_PREPARE -DPRO40_DMA_LIFECYCLE \
  -DPRO40_DMA_TX_TIMING -DPRO40_DMA_TX_PRIME -DPRO40_DMA_DUPLEX_CAPTURE -DPRO40_DMA_TX_RETURN \
  -mmacosx-version-min=15.0 "${native[@]}" tools/pro40-sequoia/NativeSessionRunner.cpp \
  ASFWDriver/Audio/Wire/IEC61883/CipHeader.cpp ASFWDriver/Audio/Wire/IEC61883/DbcCounter.cpp \
  -framework IOKit -framework CoreFoundation -lbsm -o "build/pro40-sequoia/$name"
