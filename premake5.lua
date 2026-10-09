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

-- CEF is an optional, Linux-only, Editor-private input. It is enabled only when
-- Scripts/FetchCEF.sh has installed the exact pinned package; the sanitizer
-- lane stays CEF-free because the wrapper must share the host compiler.
local browser_pin_file = assert(io.open(path.join(_MAIN_SCRIPT_DIR, "Scripts/BrowserRuntimePins.env"), "r"))
local browser_runtime_pins = {}
for line in browser_pin_file:lines() do
    local key, value = line:match("^([A-Z0-9_]+)=(.+)$")
    if key ~= nil then browser_runtime_pins[key] = value end
end
browser_pin_file:close()
assert(browser_runtime_pins.BROWSER_RUNTIME_PIN_FORMAT == "1", "Unsupported browser runtime pin format")
cef_root = path.join("Vendor/CEF", "v" .. browser_runtime_pins.CEF_VERSION, "linux-x86_64")

local function installed_cef_matches_pin()
    local package_root = path.join(_MAIN_SCRIPT_DIR, cef_root)
    local manifest = io.open(path.join(package_root, ".spiral-package-manifest"), "r")
    if manifest == nil then return false end
    local pinned = false
    for line in manifest:lines() do
        if line == "sha256=" .. browser_runtime_pins.CEF_LINUX_X86_64_SHA256 then pinned = true end
    end
    manifest:close()
    return pinned
        and os.isfile(path.join(package_root, "include/cef_app.h"))
        and os.isfile(path.join(package_root, "libcef_dll/wrapper/libcef_dll_wrapper.cc"))
        and os.isfile(path.join(package_root, "Release/libcef.so"))
end
has_cef = os.host() == "linux" and _OPTIONS["sanitize"] == nil and installed_cef_matches_pin()
if os.host() == "linux" and not has_cef and _OPTIONS["sanitize"] == nil and os.isdir(path.join(_MAIN_SCRIPT_DIR, cef_root)) then
    print("CEF at " .. cef_root .. " does not match Scripts/BrowserRuntimePins.env; generating without CEF. Run Scripts/FetchCEF.sh --force.")
end

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
    if has_cef then
        include "Vendor/CEF.premake.lua"
        include "Editor/helper"
    end
group ""

group "Examples"
    include "Sandbox"
group ""

group "Tests"
    include "Tests"
group ""
