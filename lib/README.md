# APEX reference implementation

A small, embeddable, portable C library implementing the [APEX](../spec/)
protocol — both **Host** and **Device** roles for Core (framing, discovery,
heartbeat) and for the first device class, **Activation**.

## Goals

- **Pure C99**, no dynamic allocation, no `stdio`, no platform headers.
- **Transport-agnostic**: the caller pushes received bytes in and registers a
  TX callback. There is no built-in UART driver.
- **Drop-in source**: copy `src/` and `include/` into your project and add the
  `.c` files to your build. No submodules, no third-party deps.
- **Trivial standalone build**: a 20-line `Makefile` produces `libapex.a`.

## Build

```sh
make            # produces build/libapex.a
make test       # builds and runs unit tests (needs gtest, see test/README)
make clean
```

The library compiles with any reasonable C99 toolchain. There is no `configure`,
no CMake, no autotools. To cross-compile, override `CC` and `AR`:

```sh
make CC=arm-none-eabi-gcc AR=arm-none-eabi-ar CFLAGS="-mcpu=cortex-m7 -mthumb -Os"
```

## Layout

```
include/apex/        public headers (this is what consumers #include)
  apex.h               umbrella header
  apex_core.h          constants, types, and outer-header layout from spec §3.1
  apex_cobs.h          COBS encode/decode
  apex_crc.h           CRC-16/CCITT-FALSE
  apex_framer.h        decoded-frame pack/unpack + byte-stream ingress
  apex_host.h          host: device table, discovery, heartbeat
  apex_device.h        device: discovery client, heartbeat
  apex_activation.h    Activation class — both Host and Device sides
src/                 one .c per public header
test/                gtest-based unit tests; mirrors the byte-level worked
                     example from APEX_Activation_Class.md §9.4
```

## Consumption patterns

### As a static library

```sh
make
gcc my_app.c -Iapex/include -Lapex/build -lapex
```

### As drop-in source (e.g. in betaflight)

Append the `.c` files to your source list:

```make
APEX_SRC = \
    apex/src/apex_cobs.c \
    apex/src/apex_crc.c \
    apex/src/apex_framer.c \
    apex/src/apex_host.c \
    apex/src/apex_device.c \
    apex/src/apex_activation.c
```

Add `apex/include` to your include path. That's it.

## Scope of v1

| Feature | Status |
|---|---|
| COBS framing, CRC-16/CCITT-FALSE | ✅ |
| Outer header pack/unpack | ✅ |
| Host discovery + device table + lifecycle (NEW → CONNECTED → EXPENDED / FAULT) | ✅ |
| Device discovery + 1 Hz heartbeat + 5 s watchdog | ✅ |
| NAME_REQUEST / NAME_REPLY | ✅ |
| HOST_STATE broadcast | ✅ |
| Activation class — Device state machine + COMMAND/ACK/STATUS/CAPABILITY | ✅ |
| Activation class — Host driver | ✅ |
| Multi-class on a single link (host side) | ✅ |
| Passthrough / hub nodes | ⛔ — spec marks the area as not well defined |
| Baud-rate negotiation | ⛔ — spec does not define a CONFIG message for this yet |

## License

MIT. See [LICENSE](LICENSE).
