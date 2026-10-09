-- Static wrapper that every CEF C++ API consumer links. The 157 sources are the
-- distribution's libcef_dll/**.cc minus test/ (CEF's own unit-test handlers) and
-- views/ (the Views framework, unused by windowless rendering); the list is
-- cross-checked against the installed distribution below so a pin bump that adds
-- a source fails generation instead of silently dropping it.
--
-- Built with the same host compiler and flags as the Editor: a clang-built
-- consumer linked against a g++-built wrapper segfaulted in CefExecuteProcess.
-- The flags mirror cmake/cef_variables.cmake (Linux, Release) except -Werror,
-- and NDEBUG matches the Release libcef.so in every configuration.

local cef_wrapper_sources =
{
    "base/cef_atomic_flag.cc",
    "base/cef_callback_helpers.cc",
    "base/cef_callback_internal.cc",
    "base/cef_dump_without_crashing.cc",
    "base/cef_lock.cc",
    "base/cef_lock_impl.cc",
    "base/cef_logging.cc",
    "base/cef_ref_counted.cc",
    "base/cef_thread_checker_impl.cc",
    "base/cef_weak_ptr.cc",
    "cpptoc/accessibility_handler_cpptoc.cc",
    "cpptoc/app_cpptoc.cc",
    "cpptoc/audio_handler_cpptoc.cc",
    "cpptoc/base_ref_counted_cpptoc.cc",
    "cpptoc/base_scoped_cpptoc.cc",
    "cpptoc/browser_process_handler_cpptoc.cc",
    "cpptoc/client_cpptoc.cc",
    "cpptoc/command_handler_cpptoc.cc",
    "cpptoc/completion_callback_cpptoc.cc",
    "cpptoc/component_update_callback_cpptoc.cc",
    "cpptoc/context_menu_handler_cpptoc.cc",
    "cpptoc/cookie_access_filter_cpptoc.cc",
    "cpptoc/cookie_visitor_cpptoc.cc",
    "cpptoc/delete_cookies_callback_cpptoc.cc",
    "cpptoc/dev_tools_message_observer_cpptoc.cc",
    "cpptoc/dialog_handler_cpptoc.cc",
    "cpptoc/display_handler_cpptoc.cc",
    "cpptoc/domvisitor_cpptoc.cc",
    "cpptoc/download_handler_cpptoc.cc",
    "cpptoc/download_image_callback_cpptoc.cc",
    "cpptoc/drag_handler_cpptoc.cc",
    "cpptoc/end_tracing_callback_cpptoc.cc",
    "cpptoc/find_handler_cpptoc.cc",
    "cpptoc/focus_handler_cpptoc.cc",
    "cpptoc/frame_handler_cpptoc.cc",
    "cpptoc/jsdialog_handler_cpptoc.cc",
    "cpptoc/keyboard_handler_cpptoc.cc",
    "cpptoc/life_span_handler_cpptoc.cc",
    "cpptoc/load_handler_cpptoc.cc",
    "cpptoc/media_observer_cpptoc.cc",
    "cpptoc/media_route_create_callback_cpptoc.cc",
    "cpptoc/media_sink_device_info_callback_cpptoc.cc",
    "cpptoc/menu_model_delegate_cpptoc.cc",
    "cpptoc/navigation_entry_visitor_cpptoc.cc",
    "cpptoc/pdf_print_callback_cpptoc.cc",
    "cpptoc/permission_handler_cpptoc.cc",
    "cpptoc/preference_observer_cpptoc.cc",
    "cpptoc/print_handler_cpptoc.cc",
    "cpptoc/read_handler_cpptoc.cc",
    "cpptoc/render_handler_cpptoc.cc",
    "cpptoc/render_process_handler_cpptoc.cc",
    "cpptoc/request_context_handler_cpptoc.cc",
    "cpptoc/request_handler_cpptoc.cc",
    "cpptoc/resolve_callback_cpptoc.cc",
    "cpptoc/resource_bundle_handler_cpptoc.cc",
    "cpptoc/resource_handler_cpptoc.cc",
    "cpptoc/resource_request_handler_cpptoc.cc",
    "cpptoc/response_filter_cpptoc.cc",
    "cpptoc/run_file_dialog_callback_cpptoc.cc",
    "cpptoc/scheme_handler_factory_cpptoc.cc",
    "cpptoc/server_handler_cpptoc.cc",
    "cpptoc/set_cookie_callback_cpptoc.cc",
    "cpptoc/setting_observer_cpptoc.cc",
    "cpptoc/string_visitor_cpptoc.cc",
    "cpptoc/task_cpptoc.cc",
    "cpptoc/urlrequest_client_cpptoc.cc",
    "cpptoc/v8_accessor_cpptoc.cc",
    "cpptoc/v8_array_buffer_release_callback_cpptoc.cc",
    "cpptoc/v8_handler_cpptoc.cc",
    "cpptoc/v8_interceptor_cpptoc.cc",
    "cpptoc/write_handler_cpptoc.cc",
    "ctocpp/auth_callback_ctocpp.cc",
    "ctocpp/before_download_callback_ctocpp.cc",
    "ctocpp/binary_value_ctocpp.cc",
    "ctocpp/browser_ctocpp.cc",
    "ctocpp/browser_host_ctocpp.cc",
    "ctocpp/callback_ctocpp.cc",
    "ctocpp/command_line_ctocpp.cc",
    "ctocpp/component_ctocpp.cc",
    "ctocpp/component_updater_ctocpp.cc",
    "ctocpp/context_menu_params_ctocpp.cc",
    "ctocpp/cookie_manager_ctocpp.cc",
    "ctocpp/dictionary_value_ctocpp.cc",
    "ctocpp/domdocument_ctocpp.cc",
    "ctocpp/domnode_ctocpp.cc",
    "ctocpp/download_item_callback_ctocpp.cc",
    "ctocpp/download_item_ctocpp.cc",
    "ctocpp/drag_data_ctocpp.cc",
    "ctocpp/file_dialog_callback_ctocpp.cc",
    "ctocpp/frame_ctocpp.cc",
    "ctocpp/image_ctocpp.cc",
    "ctocpp/jsdialog_callback_ctocpp.cc",
    "ctocpp/list_value_ctocpp.cc",
    "ctocpp/media_access_callback_ctocpp.cc",
    "ctocpp/media_route_ctocpp.cc",
    "ctocpp/media_router_ctocpp.cc",
    "ctocpp/media_sink_ctocpp.cc",
    "ctocpp/media_source_ctocpp.cc",
    "ctocpp/menu_model_ctocpp.cc",
    "ctocpp/navigation_entry_ctocpp.cc",
    "ctocpp/permission_prompt_callback_ctocpp.cc",
    "ctocpp/post_data_ctocpp.cc",
    "ctocpp/post_data_element_ctocpp.cc",
    "ctocpp/preference_manager_ctocpp.cc",
    "ctocpp/preference_registrar_ctocpp.cc",
    "ctocpp/print_dialog_callback_ctocpp.cc",
    "ctocpp/print_job_callback_ctocpp.cc",
    "ctocpp/print_settings_ctocpp.cc",
    "ctocpp/process_message_ctocpp.cc",
    "ctocpp/registration_ctocpp.cc",
    "ctocpp/request_context_ctocpp.cc",
    "ctocpp/request_ctocpp.cc",
    "ctocpp/resource_bundle_ctocpp.cc",
    "ctocpp/resource_read_callback_ctocpp.cc",
    "ctocpp/resource_skip_callback_ctocpp.cc",
    "ctocpp/response_ctocpp.cc",
    "ctocpp/run_context_menu_callback_ctocpp.cc",
    "ctocpp/run_quick_menu_callback_ctocpp.cc",
    "ctocpp/scheme_registrar_ctocpp.cc",
    "ctocpp/select_client_certificate_callback_ctocpp.cc",
    "ctocpp/server_ctocpp.cc",
    "ctocpp/shared_memory_region_ctocpp.cc",
    "ctocpp/shared_process_message_builder_ctocpp.cc",
    "ctocpp/sslinfo_ctocpp.cc",
    "ctocpp/sslstatus_ctocpp.cc",
    "ctocpp/stream_reader_ctocpp.cc",
    "ctocpp/stream_writer_ctocpp.cc",
    "ctocpp/task_manager_ctocpp.cc",
    "ctocpp/task_runner_ctocpp.cc",
    "ctocpp/thread_ctocpp.cc",
    "ctocpp/unresponsive_process_callback_ctocpp.cc",
    "ctocpp/urlrequest_ctocpp.cc",
    "ctocpp/v8_backing_store_ctocpp.cc",
    "ctocpp/v8_context_ctocpp.cc",
    "ctocpp/v8_exception_ctocpp.cc",
    "ctocpp/v8_stack_frame_ctocpp.cc",
    "ctocpp/v8_stack_trace_ctocpp.cc",
    "ctocpp/v8_value_ctocpp.cc",
    "ctocpp/value_ctocpp.cc",
    "ctocpp/waitable_event_ctocpp.cc",
    "ctocpp/x509_certificate_ctocpp.cc",
    "ctocpp/x509_cert_principal_ctocpp.cc",
    "ctocpp/xml_reader_ctocpp.cc",
    "ctocpp/zip_reader_ctocpp.cc",
    "shutdown_checker.cc",
    "transfer_util.cc",
    "wrapper/cef_byte_read_handler.cc",
    "wrapper/cef_closure_task.cc",
    "wrapper/cef_message_router.cc",
    "wrapper/cef_message_router_utils.cc",
    "wrapper/cef_resource_manager.cc",
    "wrapper/cef_scoped_temp_dir.cc",
    "wrapper/cef_stream_resource_handler.cc",
    "wrapper/cef_xml_object.cc",
    "wrapper/cef_zip_archive.cc",
    "wrapper/libcef_dll_wrapper2.cc",
    "wrapper/libcef_dll_wrapper.cc"
}

local cef_package_directory = path.join(workspace_root, cef_root)
local cef_wrapper_directory = path.join(cef_package_directory, "libcef_dll")
local cef_wrapper_files = {}
local cef_listed = {}
for _, source in ipairs(cef_wrapper_sources) do
    local full = path.join(cef_wrapper_directory, source)
    assert(os.isfile(full), "CEF wrapper source is missing from the installed distribution: " .. source)
    cef_listed[source] = true
    table.insert(cef_wrapper_files, "%{wks.location}/" .. cef_root .. "/libcef_dll/" .. source)
end
for _, found in ipairs(os.matchfiles(path.join(cef_wrapper_directory, "**.cc"))) do
    local relative = path.getrelative(cef_wrapper_directory, found)
    if not relative:find("^test/") and not relative:find("/test/") and not relative:find("/views/") then
        assert(cef_listed[relative], "CEF wrapper source is not listed in Vendor/CEF.premake.lua: " .. relative)
    end
end

cef_toolchain_buildoptions =
{
    "-fno-strict-aliasing", "-fstack-protector", "-funwind-tables",
    "-fvisibility=hidden", "-fvisibility-inlines-hidden",
    "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
    "-fdata-sections", "-ffunction-sections", "-fno-ident",
    "-Wall", "-Wno-missing-field-initializers", "-Wno-unused-parameter", "-Wno-comment",
    "-Wno-deprecated-declarations", "-Wsign-compare",
    "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=2"
}
cef_toolchain_defines =
{
    "NDEBUG", "WRAPPING_CEF_SHARED", "_FILE_OFFSET_BITS=64",
    "__STDC_CONSTANT_MACROS", "__STDC_FORMAT_MACROS"
}

project "CEFWrapper"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    pic "On"
    optimize "On"
    symbols "Off"

    -- Keep the generated makefile out of Vendor/ itself: a second project there
    -- renames NVRHI's generated Vendor/Makefile.
    location (path.join(workspace_root, "Vendor/CEF"))
    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files(cef_wrapper_files)

    includedirs
    {
        "%{wks.location}/" .. cef_root,
        "%{wks.location}/" .. cef_root .. "/libcef_dll"
    }

    defines(cef_toolchain_defines)
    buildoptions(cef_toolchain_buildoptions)

-- The on-demand browser host library and its smoke driver reuse the flags above.
include "../Editor/browserhost"
