# Miracast receiving on Windows — how it actually works, and how to debug it

## The stack (corrected)

A physical Wi-Fi driver does **not** expose a "Wi-Fi Direct sink role" that
software can query and depend on. Windows layers the **Microsoft Wi-Fi Direct
Virtual Adapter** (a virtual miniport) over a **WDI / WiFiCx** physical
driver and negotiates peer-to-peer roles — including the Miracast receiver
(sink) role — **dynamically** at run time. Sink capability is therefore a
property of that whole stack (physical driver + virtual adapter + graphics
driver + OS feature state), not a hardware bit.

Consequences for this project:

- "Wired-only PC ⇒ no Miracast" is only true in the trivial sense that the
  virtual adapter needs *some* Wi-Fi miniport underneath it.
- A machine can have capable hardware and still fail to start a sink,
  because the blocker lives elsewhere in the stack (feature not installed,
  radio state, another owner of the sink role, policy).

## Diagnostics (run these before blaming hardware)

```powershell
# Does the whole stack claim Wireless Display support?
netsh wlan show driver | Select-String "Wireless Display"
#   -> Wireless Display Supported: Yes (Graphics Driver: Yes, Wi-Fi Driver: Yes)

# Is the virtual adapter present, and what is the physical radio doing?
Get-NetAdapter -IncludeHidden |
  Select-Object Name,InterfaceDescription,Status,AdminStatus

# Wi-Fi Direct / P2P capabilities of the driver
netsh wlan show wirelesscapabilities

# The "Wireless display" optional feature (the OS receiver app). Elevated:
Get-WindowsCapability -Online -Name "App.WirelessDisplay.Connect*"
```

## What we measured on CASPAR (2026-08-06)

| Check | Result |
|---|---|
| Physical adapter | Realtek 8852CE Wi-Fi 6E PCI-E NIC, `AdminStatus: Up`, `Status: Disconnected` |
| Wi-Fi Direct Virtual Adapter | present (×2) |
| `Wireless Display Supported` | **Yes** (Graphics Driver: Yes, Wi-Fi Driver: Yes) |
| `MiracastReceiver.GetStatus()` | listening status 0, max connections 1 |
| `MiracastReceiverSession::Start()` | **never returns** (documented synchronous; hangs indefinitely) |
| `App.WirelessDisplay.Connect~~~~0.0.1.0` | **NotPresent** → installed 2026-08-06 |
| `Start()` after installing that feature | **still never returns** (no reboot flag set) |
| Projection policy keys | absent (PolicyManager / Policies / HKCU WirelessDisplay) |

So the driver stack is capable, yet the sink never starts.

**Tested and ruled out: the missing optional feature.** It was `NotPresent`;
installing `App.WirelessDisplay.Connect~~~~0.0.1.0` did **not** change the
behaviour — `Start()` still never returns, no reboot was flagged, and no
projection policy keys exist to be blocking it. So the FOD was a genuine
prerequisite gap but not *the* cause.

> Note: this contradicts an assumption carried in this project since M16
> ("Windows' own Projecting to this PC must be off **or not installed**").
> The feature being installed is likely a *prerequisite*; what must not
> happen is the OS Connect app holding an **active** projection session
> while we try to own the sink (that path returns `AccessDenied`, which we
> handle separately).

Install it (elevated, downloads from Microsoft) and retest:

```powershell
Add-WindowsCapability -Online -Name "App.WirelessDisplay.Connect~~~~0.0.1.0"
```

Remaining candidates, in current order of suspicion:

1. **Package identity (now the leading hypothesis).** `Windows.Media.Miracast` is a WinRT API intended
   for a sink *app*; several WinRT surfaces behave differently (or block on
   UI/consent plumbing) when called from a plain unpackaged console EXE like
   `vkr_miracast.exe`. Testing this means packaging the receiver (MSIX) or
   giving it a CoreApplication view.
2. **Radio state.** The physical Wi-Fi adapter is enabled but **not
   associated with any network** (`Status: Disconnected`) — the cheapest
   remaining test is to join any Wi-Fi network and retry, since some drivers
   will not bring up a P2P group owner while the radio is idle.
3. **"Projecting to this PC" availability.** Now that the feature exists,
   Settings → System → Projecting to this PC has an availability dropdown
   that defaults to *Always off* on desktops; with it off there is no sink
   for anyone to own.
4. **Policy.** `AllowProjectionToPC` under the WirelessDisplay policy
   manager key can disable the receiver path machine-wide — measured absent
   here, so not the cause on this machine.

## What the receiver does about it

`vkr_miracast` runs `Start()` under a **15-second watchdog** (see
[client/miracast_receiver.cpp](../client/miracast_receiver.cpp)). On timeout
it prints the diagnostic commands above and exits cleanly, so the appliance
never displays a receiver that is silently doing nothing. `MiracastNotSupported`
and `AccessDenied` are reported with the specific remedy for each.

Set `miracast=0` in `vkrender.ini` to skip the attempt entirely. The AirPlay
and WebRTC receivers are independent processes and are unaffected either way.
