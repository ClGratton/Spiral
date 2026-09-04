#pragma once

namespace Spiral::Tests
{
    // Fast, deterministic public-contract test. EngineTests.cpp owns registry
    // integration and registers this exported function.
    bool TestFabImportReceiptAuthority();
}
