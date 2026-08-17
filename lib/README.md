# APEX reference implementation

A small, embeddable, portable C library implementing the [APEX](../spec/)
protocol — both **Host** and **Device** roles for Core (framing, discovery, the
provisional configuration phase, heartbeat, baud negotiation, re-enumeration)
and for three device classes: **Activation**, **Analog HMI**, and **Repeater**.

This build speaks **APEX wire protocol version 1** (`APEX_Core.md` §3.6). v1 is
a breaking revision of the earlier v0 wire: session frames carry
`protocol_version` 0x01, the unassigned device-id marker is 0x01, `0x00` is the
permanent legacy-v0 marker (illegal in v1), and `0xFF` marks a VERSION_BEACON.

## Goals

- **Pure C99**, no dynamic allocation, no `stdio`, no platform headers.
- **Transport-agnostic**: the caller pushes received bytes in and registers a
  TX callback. There is no built-in UART driver.
- **Drop-in source**: copy `src/` and `include/` into your project and add the
  `.c` files to your build. No submodules, no third-party deps.
- **Trivial standalone build**: a short `Makefile` produces `libapex.a`.

## Build

```sh
make            # produces build/libapex.a
make test       # builds and runs the unit tests (needs GoogleTest; see below)
make clean
```

The library compiles with any reasonable C99 toolchain. There is no `configure`,
no CMake, no autotools. To cross-compile, override `CC` and `AR`:

```sh
make CC=arm-none-eabi-gcc AR=arm-none-eabi-ar CFLAGS="-mcpu=cortex-m7 -mthumb -Os"
```

### Tests

The tests are GoogleTest-based (C++17). The library itself has no C++ and no
GoogleTest dependency; only the test harness does. `test/Makefile` expects a
GoogleTest source tree at `test/gtest`, user-provided, in the fused/full-source
layout — headers under `test/gtest/inc/` and the amalgamated sources at
`test/gtest/src/gtest-all.cc` and `test/gtest/src/gtest_main.cc`. Point it at
your own copy with `GTEST_DIR`:

```sh
make -C test GTEST_DIR=/path/to/googletest check
```

## Layout

```
include/apex/        public headers (this is what consumers #include)
  apex.h               umbrella header
  apex_core.h          v1 constants, enums, and outer-header layout (spec §3)
  apex_status.h        return codes + host-managed device lifecycle (§4)
  apex_cobs.h          COBS encode/decode
  apex_crc.h           CRC-16/CCITT-FALSE
  apex_framer.h        frame pack/unpack, byte-stream ingress, decode-free
                       header-prefix peek (§3.1.6), VERSION_BEACON codec (§3.6.2)
  apex_host.h          host core: device table, discovery, provisional phase,
                       heartbeat, baud negotiation, re-enumeration, dispatch
  apex_device.h        device core: discovery client, provisional phase,
                       heartbeat, baud negotiation, re-enumeration
  apex_activation.h    Activation class (traffic_type 2), class version 1
  apex_analog_hmi.h    Analog HMI class (traffic_type 3)
  apex_repeater.h      Repeater class (traffic_type 5)
src/                 one .c per non-header-only public header
test/                gtest-based unit tests; the Activation walkthrough replays
                     the byte-level worked example from
                     APEX_Device_Class_Activation.md §9
```

The type and constant names in the headers are version-neutral C identifiers
(`apex_hdr_t`, `apex_config_msg_id_t`, `apex_device_status_t`, …). The
`ApexHdr_t`-style names that appear in the specs are prose conventions, not the C
API.

## Device lifecycle

The host tracks each device slot through `apex_device_status_t` (`apex_status.h`):

```
UNKNOWN  ->  PROVISIONAL  ->  CONNECTED  ->  EXPENDED
                                        \->  FAULT
```

`PROVISIONAL` covers the whole provisional configuration phase — from the first
`CONFIG_REPLY` until the device latches its assigned id with `CONFIG_ACK`. The
device-side link state (`apex_device_link_state_t`) mirrors this as
`DISCOVERING -> PROVISIONAL -> CONNECTED`, with `REJECTED` (terminal reject) and
`INCOMPATIBLE` (VERSION_BEACON named a disjoint wire-version range) as off-ramps.

## Consumption patterns

### As a static library

```sh
make
gcc my_app.c -Iapex/include -Lapex/build -lapex
```

### As drop-in source (e.g. into flight-controller firmware)

Append the `.c` files to your source list:

```make
APEX_SRC = \
    apex/src/apex_cobs.c \
    apex/src/apex_crc.c \
    apex/src/apex_framer.c \
    apex/src/apex_host.c \
    apex/src/apex_device.c \
    apex/src/apex_activation.c \
    apex/src/apex_analog_hmi.c \
    apex/src/apex_repeater.c
```

Add `apex/include` to your include path. That's it.

## Scope of v1

| Feature | Status |
|---|---|
| COBS framing + CRC-16/CCITT-FALSE, non-zero header prefix enforced on encode | Yes |
| Byte-stream ingress state machine (one decoded frame per 0x00 delimiter) | Yes |
| Decode-free header-prefix peek — PV/TT/ID off an encoded frame (§3.1.6, §3.7.5) | Yes |
| Outer-header pack/unpack, single-frame encode/decode | Yes |
| VERSION_BEACON codec: build, validate, parse; framer beacon callback (§3.6.2) | Yes |
| Host discovery + device table + lifecycle (UNKNOWN → PROVISIONAL → CONNECTED → EXPENDED / FAULT) | Yes |
| Device discovery client (DEVICE_INFO retransmit) + link state machine | Yes |
| Provisional configuration phase: ACK_PROVISIONAL, PHYS query loop, terminal accept/reject | Yes |
| Class-version negotiation at discovery (contiguous host/device ranges, §3.6) | Yes |
| Mass declaration + host mass policy (ACK_REJECT_MASS) | Yes |
| PHYS family: PHYS_REQUEST / PHYS_INFO (33-byte) / PHYS_ACK, host phys policy | Yes |
| Unsolicited post-CONNECTED in-flight PHYS updates, latest-wins, advisory reject | Yes |
| Post-CONNECTED baud change with ACK_REJECT_BAUD counter-offer hint + ladder-down (§3.4) | Yes |
| RESET_REQUEST re-enumeration (msg 13): boot sweep, reactive reset, voluntary reset, deferral hook | Yes |
| Device eviction (arm ACK_REJECT_POLICY deny latch + re-enumerate) | Yes |
| Heartbeat 1 Hz floor + 5 s watchdog; pre-CONNECTED only addressed frames refresh liveness | Yes |
| HOST_STATE broadcast; NAME_REQUEST / NAME_REPLY | Yes |
| Multi-class registration on a single link (host side, up to APEX_HOST_MAX_CLASSES) | Yes |
| Activation class version 1 — device state machine + Host driver | Yes |
| Activation transient states ENABLING / DISABLING (instant vs non-instant transitions) | Yes |
| Activation REJECT_BUSY + mid-transition reversals; self-clearing TRANSITION_FAILED fault | Yes |
| Activation DISPLAY_TEXT / HOST_DISPLAY_INFO (advisory display channel, §6.7/§6.8) | Yes |
| Activation GPIO-backed preconditions and HARDWARE_INPUT triggers | Yes |
| Analog HMI class — device + host (CRSF / MAVLink2, CVBS mode negotiation, CONTROL_DATA) | Yes |
| Repeater class — device + host (TELEMETRY, CONFIG, antenna list, ANTENNA_CMD, DISTAL_TLM) | Yes |
| Wayfinding class (traffic_type 4) | No — traffic type reserved; no class implementation |
| Passthrough / hub routing nodes (USB FS Hub tt 6, MAVLink passthrough tt 7) | No — traffic types reserved only |
| Legacy-v0 dual-stack | No — v0 frames are dropped (optional dual-stack is out of scope, §3.6.4) |

## Tests

10 test binaries, 127 tests, all passing (`make -C test check`):

| Binary | Tests | Covers |
|---|---|---|
| `test_cobs` | 10 | COBS encode/decode against the standard vectors |
| `test_crc` | 4 | CRC-16/CCITT-FALSE known-answer vectors |
| `test_framer` | 15 | Header pack/unpack, encode/decode, ingress, prefix peek |
| `test_beacon` | 10 | VERSION_BEACON build / validate / parse (§3.2.12, §3.6.2) |
| `test_discovery` | 30 | Hotplug (device swap, idle-port), Discovery + provisional phase (PHYS accept/reject/timeout), class-version selection, mass policy, dedup + promotion, slot recycling, re-enumeration |
| `test_negotiation` | 17 | Mutual beaconing gates, receiver discipline, VERSION_BEACON exchange and the post-CONNECTED baud ladder + switch callbacks |
| `test_activation_walkthrough` | 15 | §9 byte-for-byte walkthrough plus v1 additions (ENABLING/DISABLING, REJECT_BUSY, TRANSITION_FAILED, HOST_DISPLAY_INFO, DISPLAY_TEXT) |
| `test_activation_gpio` | 5 | GPIO-backed preconditions and the "no trigger outside ENABLED" rule |
| `test_analog_hmi` | 9 | Analog HMI negotiation, bidirectional CONTROL_DATA, reject/fault paths |
| `test_repeater` | 12 | Repeater telemetry/config and the per-antenna rework |

## License

MIT. See [LICENSE](LICENSE).
</content>
</invoke>
