# Windows physical keyboard, wired mouse and precision touchpad QA

Scope: Windows 11 with an integrated keyboard, an external wired USB keyboard,
a wired USB mouse and (where available) an integrated Precision Touchpad.
Repeat on multiple laptop vendors and distinct USB HID keyboard manufacturers.

## Read-only retest procedure

1. Unplug the external keyboard. Start a matching HydraSeat GUI and canonical
   host from the same build. Note keyboard and mouse inventory counts.
2. Plug in the USB keyboard. Allow at least one normal host inventory refresh
   and confirm a distinct second keyboard appears. Repeat unplug/replug three
   times. Verify the second device does not disappear while connected.
3. In Seats, use "Identify by key press" once for the built-in keyboard and
   once for the USB keyboard. Their stable IDs MUST be different. If both
   select one device, mark FAIL and do not configure independent seats.
4. Disconnect the USB keyboard, confirm any saved assignment becomes
   unavailable, then reconnect and identify it again.
5. Verify the wired mouse appears under MICE. If Windows exposes a HID
   Digitizers page 0x0D / usage 0x05 collection, its Precision Touchpad
   sensor can appear under the read-only, NOT SEAT-ASSIGNABLE section.
   Never infer a separate routable mouse merely from a sensor record:
   Windows can aggregate pointer messages with hDevice == nullptr.
6. Move the wired mouse and integrated touchpad independently; check that
   activity is not falsely assigned to the other device.

## Diagnostic evidence

Collect the before/after Hardware screenshots, both keyboard identification
results and versions of GUI/host. Optionally run the read-only
hydra_hardware_detect CLI and attach the output privately after reviewing
and redacting hardware serials and instance IDs. Never post unique hardware
identifiers publicly without explicit consent.

## Acceptance boundaries

- This is about accurate inventory and device attribution, not proof that
  physical keyboard/mouse input is isolated in a two-seat runtime.
- Some host-confirmed provisional HID identities require a genuine Raw Input
  event before they become selectable. Persisted profiles alone may not
  override absent transport proof.
- Old profile IDs that used shared Windows ContainerIds may need to be
  reselected and saved after this identity change. Never guess a replacement.
- No security setting, driver install, test-signing mode or anti-cheat
  bypass is required for this enumeration/identification check.
