# CEF And Chromium Runtime Notice And Distribution Gate

The installed CEF distribution preserves its upstream `LICENSE.txt` (CEF,
BSD-3-Clause, "Copyright (c) 2008-2020 Marshall A. Greenblatt. Portions
Copyright (c) 2006-2009 Google Inc.") and `CREDITS.html` (the consolidated
Chromium and third-party license listing that `about:credits` renders). Runtime
staging copies them beside the browser runtime as `CEF-LICENSE.txt` and
`CEF-CREDITS.html`, together with this notice as `CEF-THIRD_PARTY_NOTICE.md`.

Chromium itself is BSD-3-Clause. `CREDITS.html` for the pinned build lists
several hundred third-party products under, among others, Apache-2.0, MPL,
GPL/LGPL (for example FFmpeg), BoringSSL, SwiftShader, and libpng terms. The
standard CEF build excludes proprietary codecs (H.264 and AAC do not play), so
no MPEG-LA codec license is implied; that exclusion was observed, not audited
against source.

Redistribution of a build that stages this runtime remains blocked until a
release owner has reviewed `CEF-CREDITS.html` for the shipped binaries, added
the required notices to the product's third-party notice bundle, and decided
the policy questions this repository cannot settle by itself (including whether
embedding Epic/Fab web properties is permitted). A generated build output is
not a cleared redistribution package. This is not legal advice.

The runtime is Editor-only and loaded by the Editor-private browser module and
the `SpiralBrowserHelper` executable; `Engine` and `Sandbox` never link it. The
Linux sandbox is Chromium's unprivileged user-namespace sandbox. A setuid-root
`chrome-sandbox` is never fetched, staged, or shipped.
