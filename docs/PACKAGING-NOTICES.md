# Native installer notices revision — 19 October 2026

The native 0.3.1 preview installer was repackaged to include the existing distribution notices for both architectures. The packager reads the verified original DLLs; it does not compile, sign, patch or otherwise rewrite them. Both original DLLs and both language-file copies are byte-for-byte identical to the original release. The Companion module is unchanged.

`scripts/package.py` now includes these two members:

```text
AimpCompanionControl/THIRD-PARTY-NOTICES.txt
AimpCompanionControl/x64/THIRD-PARTY-NOTICES.txt
```

Each copy matches the repository's `THIRD-PARTY-NOTICES.txt` exactly and sits beside the corresponding DLL. The notice file's introductory sentence about the original installer being unchanged was removed to avoid a false claim about the revised archive; all attribution and licence texts are retained. A matching notices sidecar remains downloadable. The packager also writes that sidecar to `dist/` and includes it in the generated distribution checksums, using UTF-8 and LF for the checksum file.

## Verified package identities

| File | SHA-256 |
| --- | --- |
| Original installer (historical) | `85d05860db83edce61053d3bf012bd15bcc454ec10bd29622716d8ab6bd3f33e` |
| Updated `AimpCompanionControl-0.3.1-preview.aimppack` | `8e3cd89fd907e35681355b1d3c3bdb6baf3f363375fa9cdb1af690736dab5a47` |
| Unchanged `AimpCompanionControl-i686.dll` | `9e518213845ba555a30c85187ebf578d3580c2a8f0191efff50eb9fdf9d9f92a` |
| Unchanged `AimpCompanionControl-x86_64.dll` | `fe57bf6a76ce510ccc777646f058e9d3c71dbba04e3390e10f0db4d479ff8452` |
| Updated notices, both embedded copies and sidecar | `97dbd08fed934ab733046f4d209420d4f6f31cde22e43bf51b3ae44706282ab0` |

## Verification performed

1. Downloaded and retained the original native prerelease assets, checked the installer against its published SHA-256, and extracted its two DLLs into the ignored `dist/` directory.
2. Ran the revised `scripts/package.py` with Python 3.13.9; no native compiler or build script was invoked.
3. Read and CRC-checked every ZIP member. The archive contains exactly the four original members plus the two notices members, with no duplicate paths.
4. Compared all original members directly as bytes and verified both DLL SHA-256 hashes against the original installer and supplied verification inventory.
5. Compared both embedded notices and the generated sidecar directly with the repository notices.
6. Verified every generated distribution checksum and the release checksum file. A second packaging run produced the identical installer SHA-256.

The original `v0.3.1-preview` tag and original archive inventory remain historical records. The packaging revision is committed on `main` and replaces the installer, checksum file and matching notices sidecar on the existing prerelease. Use the current release `SHA256SUMS.txt` for downloads; the original installer hash in earlier verification reports is superseded.

Windows AIMP installation/runtime acceptance, publisher signing and Defender scanning were not performed for this revision. The native DLLs remain unsigned, the existing runtime limitations remain applicable, and the release remains a prerelease.
