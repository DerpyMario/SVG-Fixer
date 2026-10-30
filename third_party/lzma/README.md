# LZMA SDK (7z decoder subset)

Unmodified files from the **LZMA SDK 23.01** by Igor Pavlov
(<https://www.7-zip.org/sdk.html>), `C/` directory.

Only the files needed to *read* `.7z` archives are included
(LZMA, LZMA2, PPMd, BCJ/BCJ2/ARM/… filters, Delta, CRC).
SVG-Fixer builds them with `Z7_PPMD_SUPPORT` and `Z7_EXTRACT_ONLY`.

License: the LZMA SDK is placed in the **public domain** (see `lzma-sdk.txt`).
