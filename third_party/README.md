# Vendor SDK headers

This directory contains selected **vendor-supplied SDK header files** used by OpenMEI to provide optional camera backends.

These files are **not part of OpenMEI** and are **not covered by the OpenMEI MIT license**. Each header remains the property of its respective vendor and is subject to that vendor's applicable license and redistribution terms.

OpenMEI does not include or link against vendor SDK binaries in its source repository. Camera SDK libraries are loaded dynamically at runtime when installed on the user's system.

| Vendor  | Header                  | Upstream SDK                                            |
| ------- | ----------------------- | ------------------------------------------------------- |
| ZWO     | `asi/ASICamera2.h`      | https://www.zwoastro.com/downloads/developers           |
| SVBONY  | `svbony/SVBCameraSDK.h` | https://www.svbony.com/downloads/software-driver        |
| ToupTek | `toupcam/toupcam.h`     | https://www.touptekphotonics.com/download/?category=SDK |

## SVBONY

`svbony/SVBCameraSDK.h` is distributed as part of the SVBONY Camera SDK.

The current SDK can be obtained directly from SVBONY:

https://www.svbony.com/downloads/software-driver

The upstream SDK package should be regarded as the authoritative source for the latest version of this header.

## ToupTek

`toupcam/toupcam.h` is distributed as part of the ToupTek **ToupCamSDK**.

The current SDK can be obtained directly from ToupTek Photonics:

https://www.touptekphotonics.com/download/?category=SDK

ToupTek's astronomy-specific SDK downloads are also available from:

https://www.touptek-astro.com/software-download

The upstream SDK package should be regarded as the authoritative source for the latest version of this header.

## ZWO

`asi/ASICamera2.h` is distributed as part of the ZWO ASI Camera SDK.

The current SDK can be obtained from ZWO's developer downloads:

https://www.zwoastro.com/downloads/developers

The upstream SDK package should be regarded as the authoritative source for the latest version of this header.

## Updating vendor headers

When updating a vendored header:

1. Obtain the header from the vendor's official SDK distribution.
2. Replace only the required header file unless additional files are necessary.
3. Do not commit vendor DLLs, shared libraries, static libraries, installers, or other SDK binaries.
4. Preserve any copyright, license, or attribution notices contained in the vendor's original file.
5. Update this document if the upstream SDK location or directory layout changes.

## Licensing

The presence of a vendor header in this repository does not imply that it is distributed under the OpenMEI license.

Copyright and licensing of:

* `ASICamera2.h` remain with ZWO.
* `SVBCameraSDK.h` remain with SVBONY.
* `toupcam.h` remain with ToupTek.

For authoritative licensing and redistribution terms, consult the SDK package and documentation supplied by the respective vendor.
