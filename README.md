# AnyPalbook

AnyPalbook is a UE4SS mod for Palworld that allows an Applied Handbook to grant its work suitability to a Pal even when that Pal starts with rank 0 in that work type.

Example: a Cattiva with no Kindling can use the Kindling Applied Handbook and gain Kindling.

## Target build

This build is retargeted from the exact `Palworld-Win64-Shipping.exe` supplied for analysis:

- SHA-256: `44b6295e70aa37b83d1c42ce1dcf865a7ffadcd300298b0e49a02bad8eb83443`
- File size: `161802312` bytes
- PE `SizeOfImage`: `0x0A011000`

## v1.1.0

The first v1.0 patch fixed the eligibility check, so the game accepted and consumed a handbook on a Pal whose innate rank was 0. Testing showed a second vanilla restriction: the handbook bonus is saved in `GotWorkSuitabilityAddRankList`, but the central work-suitability accessors return early when the Pal's species has no innate entry for that work type. That made the successful handbook use effectively invisible.

v1.1.0 fixes both halves:

- Allows the first Applied Handbook on a work type whose current rank is 0.
- Makes `GetWorkSuitabilityRankWithCharacterRank` recognize a handbook-only suitability.
- Makes `HasWorkSuitability` recognize a handbook-only suitability.
- Makes `HasWorkSuitabilityRank` recognize a handbook-only suitability.
- Keeps the vanilla handbook target/type checks and maximum-rank check intact.

The game already writes new handbook bonuses with `UPalIndividualCharacterParameter::SetWorkSuitabilityAddRank`, so AnyPalbook does not replace the save format. A handbook used under v1.0 may therefore become visible after installing v1.1.0 if that saved bonus persisted.

## Install

Copy the `AnyPalbook` folder into:

`Palworld\\Pal\\Binaries\\Win64\\ue4ss\\Mods\\`

The final layout should contain:

- `AnyPalbook\\enabled.txt`
- `AnyPalbook\\Scripts\\main.lua`
- `AnyPalbook\\Native\\AnyPalbook.dll`

Fully close and restart Palworld after replacing the DLL. Do not hot-reload this native patch into a running game process.

Check `AnyPalbook\\anypalbook.log` and `UE4SS.log` after launch if the mod does not load.

## Expected v1.1.0 log

A successful launch should include messages for the zero-rank gate plus the added-only rank/has hooks, ending with:

`SUCCESS: AnyPalbook v1.1.0 compatibility hooks active.`

When the game queries a newly added Kindling rank, the log may also show:

`ACTIVE: added-only work suitability detected (type=1 rank=1).`

## Safety

The native patch validates the exact target build and expected machine-code prefixes before installing its detours. If the executable no longer matches after a Palworld update, it refuses to install the compatibility hooks instead of patching unknown instructions.
