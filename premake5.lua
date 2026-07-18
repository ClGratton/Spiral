workspace "Spiral"
    architecture "x86_64"
    startproject "Editor"

    configurations
    {
        "Debug",
        "Release",
        "Dist"
    }

    multiprocessorcompile "On"

newoption
{
    trigger = "sanitize",
    value = "MODE",
    description = "Enable an admitted sanitizer configuration",
    allowed = {
        { "address-undefined", "Clang AddressSanitizer plus UndefinedBehaviorSanitizer" }
    }
}

action_suffix = ""
if _ACTION ~= nil and _ACTION ~= "vs2022" then
    action_suffix = "-" .. _ACTION
end
if _OPTIONS["sanitize"] == "address-undefined" then
    action_suffix = action_suffix .. "-asan-ubsan"
end

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}" .. action_suffix
has_nvrhi = os.isdir("Vendor/NVRHI/include")
has_vulkan_headers = os.isdir("Vendor/Vulkan-Headers/include/vulkan")
has_directx_headers = os.isdir("Vendor/DirectX-Headers/include/directx")
workspace_root = path.getabsolute(_MAIN_SCRIPT_DIR)

function apply_spiral_sanitizers(enable_fuzzer)
    if _OPTIONS["sanitize"] ~= "address-undefined" then return end
    filter "toolset:clang"
        buildoptions { "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all" }
        if enable_fuzzer then
            buildoptions { "-fsanitize=fuzzer" }
            linkoptions { "-fsanitize=fuzzer,address,undefined", "-fno-sanitize-recover=all" }
            defines { "GE_LIBFUZZER=1" }
        else
            linkoptions { "-fsanitize=address,undefined", "-fno-sanitize-recover=all" }
        end
    filter {}
end

local pin_file = assert(io.open(path.join(_MAIN_SCRIPT_DIR, "Scripts/ShaderToolchainPins.env"), "r"))
shader_toolchain_pins = {}
for line in pin_file:lines() do
    local key, value = line:match("^([A-Z0-9_]+)=(.+)$")
    if key ~= nil then shader_toolchain_pins[key] = value end
end
pin_file:close()
assert(shader_toolchain_pins.SHADER_TOOLCHAIN_PIN_FORMAT == "1", "Unsupported shader toolchain pin format")
slang_version = "v" .. shader_toolchain_pins.SLANG_VERSION
slang_root = path.join("Vendor/Slang", slang_version)
dxc_version = "v" .. shader_toolchain_pins.DXC_VERSION
dxc_root = path.join("Vendor/DXC", dxc_version)

local slang_hash_key_by_host = {
    windows = "SLANG_WINDOWS_X86_64_SHA256",
    linux = "SLANG_LINUX_X86_64_SHA256",
    macosx = "SLANG_MACOS_X86_64_SHA256"
}
local slang_hash_key = slang_hash_key_by_host[os.host()]
assert(slang_hash_key ~= nil and shader_toolchain_pins[slang_hash_key] ~= nil,
    "No x86_64 Slang package hash is declared for this Premake host")
slang_package_sha256 = shader_toolchain_pins[slang_hash_key]
dxc_package_sha256 = os.host() == "windows" and shader_toolchain_pins.DXC_WINDOWS_X86_64_SHA256 or ""
shader_toolchain_defines = {
    'GE_SLANG_PACKAGE_SHA256="' .. slang_package_sha256 .. '"',
    'GE_DXC_PACKAGE_SHA256="' .. dxc_package_sha256 .. '"'
}

group "Core"
    include "Vendor/GLFW"
    include "Vendor/ImGui"
    if has_nvrhi then
        include "Vendor/NVRHI.premake.lua"
    end
    include "Engine"
group ""

group "Tools"
    include "Editor"
group ""

group "Examples"
    include "Sandbox"
group ""

group "Tests"
    include "Tests"
group ""
