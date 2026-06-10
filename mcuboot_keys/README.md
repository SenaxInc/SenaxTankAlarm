# MCUboot Keys in SenaxTankAlarm

**Strategic Posture Statement:**

MCUboot keys in this repository are the public Arduino default keys. They exist solely so the Arduino MCUboot bootloader will accept our images and so we get **mechanical** benefits: image integrity (hash/magic) checks and **A/B rollback** on a bad boot. They provide **no firmware authenticity**: anyone can sign an image the bootloader accepts. 

This is an accepted trade-off — device firmware confidentiality/authenticity is explicitly out of scope for this product. Do **not** describe these signatures as a security control.

## What we get from MCUboot with default keys (all valuable, none security):
- **Corruption rejection:** a truncated/garbled image fails the image hash and is not booted.
- **A/B rollback:** a bad image that boots but fails its health gate is automatically reverted.
- **Atomic swap:** the live application is never partially overwritten (no mid-write brick).

## What we explicitly do not get (and accept):
- Protection against a malicious actor substituting a validly-"signed" image (they can use the same public default key).
