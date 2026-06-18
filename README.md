# APEX — Adaptive Payload EXchange

APEX is an open, plug-and-play protocol for connecting drone platforms (**hosts**)
and payloads (**devices**) over a standard connector. It is analogous to
USB: the bus protocol itself is small and class-agnostic, while device-specific
behavior is defined through separate **device classes**.

> **Status: Draft, v0.7.0.** These specifications are under active
> development and may change. Mechanical and electrical details are still being
> finalized. The specification documents and the C reference implementation are
> versioned together as one snapshot; the current version is recorded in
> [VERSION.txt](VERSION.txt) and stamped in the footer of every specification
> PDF.

## Specifications

The protocol is published as a set of PDF documents under [`spec/`](spec/).

| Document | Description |
| --- | --- |
| [APEX — Core](spec/APEX_Core.pdf) | Core protocol: framing, the outer header, baud negotiation, the discovery handshake, capability exchange, and device-level lifecycle status. Does not define device behavior. |
| [APEX — Device Classes](spec/APEX_Device_Classes.pdf) | The `traffic_type` registry mapping each device class to its companion specification. |
| [APEX Device Class — Activation](spec/APEX_Device_Class_Activation.pdf) | Devices that progress through enable and trigger phases. |
| [APEX Device Class — Analog HMI](spec/APEX_Device_Class_Analog_HMI.pdf) | Analog HMI: control packets (CRSF / MAVLink 2) in, CVBS video out. |
| [APEX Device Class — Wayfinding](spec/APEX_Device_Class_Wayfinding.pdf) | Devices that produce directional cues — "point me there" updates for the operator, from any source. |
| [APEX Device Class — Repeater](spec/APEX_Device_Class_Repeater.pdf) | RF relay node: extends C2 and video links between ground and a distal drone. |

Planned classes reserved in the registry: **USB_FS_HUB**.

Once a device is discovered and its class accepted, all further traffic is routed
by `traffic_type` to the relevant device-class specification.

## Mechanical

The mechanical standard package is provided under [`mechanical/`](mechanical/):
a 3D CAD model of the connector assembly in STEP format and the corresponding 2D
drawing in PDF.

## License

MIT License. See [LICENSE](LICENSE).

<!--
  This README is GENERATED. Edit the prose in spec/tools/README.template.md;
  the specification table is produced from the Device Classes registry by
  spec/tools/build.mjs on every `npm run build`. Do not edit build/README.md or
  public/README.md by hand — changes there are overwritten.
-->