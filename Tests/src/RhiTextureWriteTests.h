#pragma once

namespace SpiralTests
{
    bool TestRhiTextureWriteValidationMatchesIndependentOracle();
    bool TestRhiTextureWriteFakeDeviceContract();
    bool TestRhiTextureWriteDefaultsRejectWithoutBackendSupport();
}
