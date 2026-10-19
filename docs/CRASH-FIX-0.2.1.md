# AIMP Companion Control 0.2.1 crash fix

Date: 2026-10-18 UTC. Companion module 1.0.0-beta.3 is unchanged.

## Reported failure

The supplied AIMP error dialog reports AIMP 5.4.0.2696 and an `EAccessViolation` in `AimpCompanionControl.dll`: instruction address `60D0E91D`, reading address `00000002`. The report appeared while the expanded plugin was connected to Companion. We do not have a full madExcept report or a confirmed DLL load base.

## Concrete SDK mismatch

The official stable AIMP 5.40 SDK archive contains these incompatible declarations of the same interface method:

- C++ `Sources/Cpp/apiPlayer.h`: `virtual BOOL WINAPI GetActive() = 0;`
- Delphi `Sources/Delphi/apiPlayer.pas`: `function GetActive: LongBool;`

The Delphi declaration omits `stdcall`, unlike the other methods in this interface. Delphi's default interface convention is `register`. On 32-bit Windows, the two conventions disagree about passing `Self` and cleaning the stack. Calling a register implementation through the C++ declaration can leave the stack displaced by four bytes. Windows x64 uses a unified convention and does not have this specific stack-cleanup mismatch.

Primary sources: the bundled official C++ SDK, the matching official Delphi SDK downloaded from https://aimp.ru/?cat=sdk&do=download&os=desktop, and Embarcadero's interface calling-convention documentation at https://docwiki.embarcadero.com/RADStudio/Florence/en/Object_Interfaces_%28Delphi%29 . The downloaded SDK archive SHA-256 is `aa449fed93b93af7880a5580d71265231d048115e837b8d79f0c32aec59537a7`.

The original 0.2.0 x86 DLL SHA-256 is `1a2b358a6230aa550c7042271fe9841986506f4fd632fd0e52c7a7f3fd2f07b0`. With the candidate loaded base `60CF0000`, the reported instruction maps to RVA `01E91D`, in `Snapshot()` cleanup of the equalizer service reference. The instruction dereferences a pointer loaded from the stack. This is consistent with the calling-convention defect, but the inferred base and exact runtime sequence are not confirmed by a full crash report.

## Change

Both the EQ state snapshot and EQ toggle previously called `GetActive`. They now read the documented `AIMP_MSG_PROPERTY_EQUALIZER` property through `IAIMPServiceMessageDispatcher::Send`, using `AIMP_MSG_PROPVALUE_GET` and a pointer to a 32-bit `BOOL`.

The snapshot returns a null enabled state plus a read error if the query fails. Toggle only calls `SetActive` after a successful query. No direct `GetActive()` call remains in the implementation. The SDK headers are unmodified. An independent audit found no further calling-convention omissions among the 15 SDK interfaces used by the new controls.

The original commands, expanded controls, protocol version, connection settings, plugin identity, and Companion beta.3 package are retained.

## Verification and limits

- Both Windows x86 and x64 DLLs compile successfully with the pinned LLVM-MinGW toolchain.
- Protocol/input tests: 73 assertions and 50,000 malformed-input cases pass under AddressSanitizer and UndefinedBehaviorSanitizer.
- Portable audio validation passes under the same sanitizers.
- Both PE files expose the undecorated `AIMPPluginGetHeader` entry point, enable ASLR and NX, and have no external GCC/C++ runtime DLL dependency.
- Both DLLs embedded in the AIMPPACK match the standalone binaries; archive integrity and SHA-256 manifests pass.
- Independent source review confirms both affected paths use the dispatcher and toggle does not write after a failed read.
- `tests/eq_calling_convention_probe.S` is an optional freestanding i386 ABI demonstration. It compiled, but this executor cannot run 32-bit ELF binaries. It was **not executed**, and is not a Windows/AIMP regression test.

No Windows or AIMP runtime is available here. These checks do not establish that the reported crash is resolved on the user's machine. If it persists after installing 0.2.1, save the full AIMP bug report, including the stack and loaded-module information.

## Install

Disable the AIMP connection in Companion, fully close AIMP, install `dist/AimpCompanionControl-0.2.1-preview.aimppack`, restart AIMP, and enable the same connection. Confirm AIMP shows plugin version 0.2.1. Keep Companion module beta.3 and existing buttons; no rebuild or reimport is required.
