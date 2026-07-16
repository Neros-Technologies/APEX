# APEX — Device Classes

**APEX — Adaptive Payload EXchange**

**Status:** Draft | **Scope:** Registry of device classes (traffic types)

---

<a id="1--overview" name="1--overview"></a>
## 1.  Overview

This document is the **registry** of APEX device classes. Each device class corresponds to a `traffic_type` value in the APEX V0 outer header (see [APEX — Core §3.1.1](APEX_Core.md#3-1-1--outer-header-apexv0hdr_t)) and is fully specified in its own companion document. The core spec is class-agnostic; the per-class docs define both the **Host role** and the **Device role** ([APEX — Core §1](APEX_Core.md#1--overview)) — including the messages each side sends, state machines, and any class-specific timing.

Class assignment happens during discovery: the device declares its desired class via `device_class_req` in DEVICE_INFO, and the host accepts or rejects via CONFIG_REPLY (Core [§3.2](APEX_Core.md#3-2--config-traffic-traffic_type--0)). Once accepted, frames carrying `traffic_type` equal to that class value are routed to the class-specific handler on both sides.

---

<a id="2--registry" name="2--registry"></a>
## 2.  Registry

| `traffic_type` | Name | Description | Spec |
| --- | --- | --- | --- |
| `0` | **CONFIG** | Core control traffic: discovery, configuration, and the implicit heartbeat. Not a device class — defined by the core spec. | [APEX — Core](APEX_Core.md) |
| `1` | **ACTIVATION** | Devices that progress through enable and trigger phases. | [APEX Device Class — Activation](APEX_Device_Class_Activation.md) |
| `2` | **ANALOG_HMI** | Analog HMI: control packets (CRSF / MAVLink 2) in, CVBS video out. | [APEX Device Class — Analog HMI](APEX_Device_Class_Analog_HMI.md) |
| `3` | **WAYFINDING** | Devices that produce directional cues — "point me there" updates for the operator, from any source. | [APEX Device Class — Wayfinding](APEX_Device_Class_Wayfinding.md) |
| `4` | **REPEATER** | RF relay node: extends C2 and video links between ground and a distal drone. | [APEX Device Class — Repeater](APEX_Device_Class_Repeater.md) |
| `5` | **USB_FS_HUB** | Full-Speed USB passthrough traffic. | *(TBD)* |
| `6` | **MAVLINK** | Transparent bidirectional MAVLink byte tunnel between a payload MAVLink endpoint and the Host. | [APEX Device Class — MAVLink](APEX_Device_Class_MAVLink.md) |

---

<a id="3--reserved--custom-traffic-types" name="3--reserved--custom-traffic-types"></a>
## 3.  Reserved & Custom Traffic Types

`traffic_type` values not enumerated above are reserved for future device classes. Vendor- or platform-specific extensions should be coordinated through this registry rather than reusing CONFIG message IDs or values from another class.

> **Open:** Whether to formally allocate a high range of `traffic_type` values (e.g., `0x80–0xFF`) for vendor-custom classes, analogous to the prior spec's `0x80–0xFF` MSG_ID range.

---

<a id="4--authoring-a-new-class-spec" name="4--authoring-a-new-class-spec"></a>
## 4.  Authoring a New Class Spec

A device-class spec should cover, at minimum:

1. **`traffic_type` value** assigned in the registry above.
2. **Roles** — what the Host and Device each do in this class.
3. **Message set** — the inner payload format(s) carried under this `traffic_type`, distinguished by an in-class message ID where applicable.
4. **State machines** — for both host and device, if any.
5. **Timing** — heartbeat cadence beyond the core implicit heartbeat, command-response timeouts, etc.
6. **Failure handling** specific to the class.

---