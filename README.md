# AnyPalbook

AnyPalbook is a UE4SS mod for Palworld that allows an Applied Handbook to grant its work suitability to a Pal even when that Pal starts with rank 0 in that work type.

Example: a Cattiva with no Kindling can use the Kindling Applied Handbook and gain Kindling.

## Target build

This initial build is retargeted from the exact `Palworld-Win64-Shipping.exe` supplied for analysis:

- SHA-256: `44b6295e70aa37b83d1c42ce1dcf865a7ffadcd300298b0e49a02bad8eb83443`
- File size: `161802312` bytes
- PE `SizeOfImage`: `0x0A011000`

## What the patch changes

The current game function `UPalUtility::CanUseTargetWorkSuitabilityRankUp` rejects a handbook when the Pal's current rank for that work type is zero. AnyPalbook removes only that zero-rank rejection. The game's existing validity, item-type, target and maximum-rank checks are left intact.

The game's existing `UPalIndividualCharacterParameter::SetWorkSuitabilityAddRank` routine already creates a new work-suitability bonus entry when one does not exist, so no save-format replacement is needed.

## Install

Copy the `AnyPalbook` folder into:

`Palworld\\Pal\\Binaries\\Win64\\ue4ss\\Mods\\`

The final layout should contain:

- `AnyPalbook\\enabled.txt`
- `AnyPalbook\\Scripts\\main.lua`
- `AnyPalbook\\Native\\AnyPalbook.dll`

Check `AnyPalbook\\anypalbook.log` and `UE4SS.log` after launch if the mod does not load.

## Safety

The native patch verifies the expected machine-code bytes before changing memory. If the target no longer matches after a Palworld update, it refuses to patch rather than modifying an unknown instruction.
