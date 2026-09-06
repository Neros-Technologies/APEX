# APEX — Device Classes

**APEX — Adaptive Payload EXchange**

**Status:** Draft | **Scope:** Registry of device classes (traffic types)

---

<a id="1--overview" name="1--overview"></a>
## 1.  Overview

This document is the **registry** of APEX device classes. Each device class corresponds to a `traffic_type` value in the APEX v1 outer header (see [APEX — Core §3.1.1](APEX_Core.md#3-1-1--outer-header-apexhdr_t)) and is fully specified in its own companion document. The core spec is class-agnostic; the per-class docs define both the **Host role** and the **Device role** ([APEX — Core §1](APEX_Core.md#1--overview)) — including the messages each side sends, state machines, and any class-specific timing.

Class assignment happens during discovery: the device declares its desired class via `device_class_req` in DEVICE_INFO, and the host accepts or rejects via CONFIG_REPLY (Core [§3.2](APEX_Core.md#3-2--config-traffic-traffic_type--1)). Once accepted, frames carrying `traffic_type` equal to that class value are routed to the class-specific handler on both sides. Discovery also negotiates the **class version** in force for the session (DEVICE_INFO advertises the device's supported class-version range; CONFIG_REPLY returns `selected_class_version`) — see [APEX — Core](APEX_Core.md).

---

<a id="2--registry" name="2--registry"></a>
## 2.  Registry

| `traffic_type` | Name | Description | Class version | Spec |
| --- | --- | --- | --- | --- |
| `1` | **CONFIG** | Core control traffic: discovery, configuration, and the implicit heartbeat. Not a device class — defined by the core spec. | — | [APEX — Core](APEX_Core.md) |
| `2` | **ACTIVATION** | Devices that progress through enable and trigger phases. | 1 | [APEX Device Class — Activation](APEX_Device_Class_Activation.md) |
| `3` | **ANALOG_HMI** | Analog HMI: control packets (CRSF / MAVLink 2) in, CVBS video out. | 1 | [APEX Device Class — Analog HMI](APEX_Device_Class_Analog_HMI.md) |
| `4` | **WAYFINDING** | Devices that produce directional cues — "point me there" updates for the operator, from any source. | 1 | [APEX Device Class — Wayfinding](APEX_Device_Class_Wayfinding.md) |
| `5` | **REPEATER** | RF relay node: extends C2 and video links between ground and a distal drone. | 1 | [APEX Device Class — Repeater](APEX_Device_Class_Repeater.md) |
| `6` | **USB_FS_HUB** | Full-Speed USB passthrough traffic. | TBD | *(TBD)* |
| `7` | **MAVLINK** | Transparent bidirectional MAVLink byte tunnel between a payload MAVLink endpoint and the Host. | TBD | *(TBD)* |

`traffic_type = 0x00` is permanently invalid on the wire (see the core spec's non-zero header constraints). The class version listed is the current class version defined by each class's companion document; the version in force for a given session is negotiated at discovery.

---

<a id="3--reserved--custom-traffic-types" name="3--reserved--custom-traffic-types"></a>
## 3.  Reserved & Custom Traffic Types

`traffic_type` values not enumerated above and outside the vendor range are reserved for future centrally-registered device classes. Extensions of general-purpose value should be coordinated through this registry rather than reusing CONFIG message IDs or values from another class.

**Vendor / platform-specific range — `0xF0–0xFE`.** These values are allocated for vendor- or platform-specific device classes. They are **not centrally registered**; collisions between vendors are the integrator's concern. Vendors SHOULD nonetheless coordinate a class through this registry when it could be general-purpose, rather than burning a vendor value on something the wider ecosystem would benefit from.

`0xFF` is reserved by the core spec. `0x00` is permanently invalid on the wire (see the core spec's non-zero header constraints).

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
7. **Class version** — the class's current class version (each class's v1.0-era revision is class version 1; class version 0 retroactively denotes its v0-era doc). The version in force for a session is negotiated at discovery, not carried in class frames — see [APEX — Core](APEX_Core.md).
8. **Per-class-version changelog** — a record marking each change as **breaking** (version-consuming) or **compatible** (an appended optional tail or a new message, per the core forward-compatibility rules). A version increments only for a breaking change.

---