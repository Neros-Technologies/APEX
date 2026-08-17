# APEX — Adaptive Payload EXchange

APEX is an open, plug-and-play protocol for connecting drone platforms (**hosts**)
and payloads (**devices**) over a standard connector. It is analogous to
USB: the bus protocol itself is small and class-agnostic, while device-specific
behavior is defined through separate **device classes**.

> **Status: Draft.** These specifications are under active development and may
> change. Mechanical and electrical details are still being finalized. The
> specification documents and the C reference implementation are versioned
> together as one snapshot; the current version is recorded in
> [VERSION.txt](VERSION.txt).

## Specifications

The protocol is defined by a set of Markdown documents under [`spec/`](spec/).
Each device class corresponds to a `traffic_type` value in the APEX outer header
and is specified in its own companion document; the core protocol is
class-agnostic.

| Document | Description |
| --- | --- |
| [APEX — Core](spec/APEX_Core.md) | Core protocol: framing, the outer header, baud negotiation, the discovery handshake, capability exchange, and device-level lifecycle status. Does not define device behavior. |
| [APEX — Device Classes](spec/APEX_Device_Classes.md) | The `traffic_type` registry mapping each device class to its companion specification. |
| [APEX Device Class — Activation](spec/APEX_Device_Class_Activation.md) | Devices that progress through enable and trigger phases. |
| [APEX Device Class — Analog HMI](spec/APEX_Device_Class_Analog_HMI.md) | Analog HMI: control packets (CRSF / MAVLink 2) in, CVBS video out. |
| [APEX Device Class — Wayfinding](spec/APEX_Device_Class_Wayfinding.md) | Devices that produce directional cues — "point me there" updates for the operator, from any source. |
| [APEX Device Class — Repeater](spec/APEX_Device_Class_Repeater.md) | RF relay node: extends C2 and video links between ground and a distal drone. |

Planned classes reserved in the registry: **USB_FS_HUB** and **MAVLINK**.

Once a device is discovered and its class accepted, all further traffic is routed
by `traffic_type` to the relevant device-class specification.

The specifications can be rendered to PDF with the build tooling under
[`spec/tools/`](spec/tools/) (`cd spec && npm install && npm run build` writes
per-document PDFs to `spec/build/`).

## Reference implementation

A small, portable C99 reference library implementing the protocol — both the
**Host** and **Device** roles — lives under [`lib/`](lib/). It has no dynamic
allocation, no `stdio`, and no third-party dependencies. See
[`lib/README.md`](lib/README.md) for build and consumption details.

## Mechanical

The normative mechanical standard is provided under
[`spec/mechanical/`](spec/mechanical/): a 3D CAD model of the connector assembly
in STEP format (`APEX STANDARD V0.9.STEP`) and the corresponding 2D drawing in
PDF (`APEX STANDARD V0.9.pdf`).

Non-normative reference designs are provided under
[`spec/mechanical/reference/`](spec/mechanical/reference/) as an example of a
conforming implementation. For each of the **carrier** (host side) and the
**payload** (device side) there are two STEP models:

- `APEX <role> PCBA, REFERENCE IMPLEMENTATION.STEP` — the bare populated board.
- `APEX <role> REFERENCE IMPLEMENTATION.STEP` — the full mechanical assembly.

These reference designs are informative only; the connector standard above is
the authoritative geometry.

## License

MIT License. See [LICENSE](LICENSE).
