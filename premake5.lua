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

action_suffix = ""
if _ACTION ~= nil and _ACTION ~= "vs2022" then
    action_suffix = "-" .. _ACTION
end

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}" .. action_suffix
has_nvrhi = os.isdir("Vendor/NVRHI/include")
has_vulkan_headers = os.isdir("Vendor/Vulkan-Headers/include/vulkan")
has_directx_headers = os.isdir("Vendor/DirectX-Headers/include/directx")

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
