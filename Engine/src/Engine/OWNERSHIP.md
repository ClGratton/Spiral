# Engine Module Ownership

The engine library owns reusable runtime systems. It must not depend on `Editor` or `Sandbox`.

Allowed dependencies:

- C++ standard library.
- Explicit vendor libraries added through `Vendor/` and declared in Premake.
- Platform code hidden behind engine interfaces.

Forbidden:

- Editor panels or workflow UI.
- Direct application-specific logic.
- Backend-specific renderer types leaking into public gameplay APIs.
