#include "coreclr.hpp"
#include "vendor/semver/semver200.h"

#include <algorithm>
#include <iostream>

using snap::core_clr_directory;
using snap::core_clr_instance;
using snap::core_clr_instance_t;

static const wchar_t* server_gc_environment_var = L"CORECLR_SERVER_GC";
static const wchar_t* concurrent_gc_environment_var = L"CORECLR_CONCURRENT_GC";

static const wchar_t* core_clr_dll = L"coreclr.dll";
static const wchar_t* core_clr_program_files_directory_path = L"%programfiles%\\dotnet\\shared\\microsoft.netcore.app";

int snap::coreclr::run(const std::wstring & executable_path, const std::vector<std::wstring>& arguments,
    const version::Semver200_version & clr_minimum_version)
{
    auto executable_file_exists = FALSE;
    if (!pal_fs_file_exists(executable_path.c_str(), &executable_file_exists)
        || !executable_file_exists)
    {
        return -1;
    }

    wchar_t* executable_working_directory = nullptr;
    if (!pal_fs_get_directory_name_full_path(executable_path.c_str(), &executable_working_directory))
    {
        return -1;
    }

    const auto core_clr_instance = try_load_core_clr(executable_path, arguments, clr_minimum_version);
    if (core_clr_instance == nullptr)
    {
        std::wcerr << L"ERROR - " << core_clr_dll << " not found." << std::endl;
        return -1;
    }

    const auto clr_runtime_host = get_clr_runtime_host(core_clr_instance);
    if (clr_runtime_host == nullptr)
    {
        return -1;
    }

    auto hr = clr_runtime_host->SetStartupFlags(create_clr_startup_flags());
    if (FAILED(hr)) {
        std::wcerr << L"Failed to set startup flags. ERRORCODE: " << hr << std::endl;
        return false;
    }

    hr = clr_runtime_host->Start();
    if (FAILED(hr)) {
        std::wcerr << L"Failed to start CoreCLR. ERRORCODE: " << hr << std::endl;
        return false;
    }

    const auto trusted_platform_assemblies_str = build_trusted_platform_assemblies_str(executable_path, core_clr_instance);
    if (trusted_platform_assemblies_str.empty())
    {
        return -1;
    }

    auto app_domain_id = 0ul;
    if (!create_clr_appdomain(executable_path, executable_working_directory,
        trusted_platform_assemblies_str, core_clr_instance, &app_domain_id))
    {
        return -1;
    }

    auto argc = 0;
    std::wstring argw_buffer;
    auto argw = to_clr_arguments(arguments, &argc, argw_buffer);
    auto execute_assembly_exit_code = 0ul;

    core_clr_activation_ctx cxt{ executable_path.c_str() };
    hr = clr_runtime_host->ExecuteAssembly(app_domain_id, executable_path.c_str(), argc, &argw, &execute_assembly_exit_code);
    if (FAILED(hr))
    {
        std::wcerr << L"ERROR - Failed call to ExecuteAssembly. ERRORCODE: " << hr << std::endl;
        return false;
    }

    hr = clr_runtime_host->UnloadAppDomain(app_domain_id, TRUE /* wait until done */);
    if (FAILED(hr)) {
        std::wcerr << L"ERROR - Failed to unload the AppDomain. ERRORCODE: " << hr << std::endl;
        return false;
    }

    hr = clr_runtime_host->Stop();
    if (FAILED(hr)) {
        std::wcerr << "ERROR - Failed to stop the host. ERRORCODE: " << hr << std::endl;
        return false;
    }

    clr_runtime_host->Release();

    return execute_assembly_exit_code;
}

ICLRRuntimeHost2* snap::coreclr::get_clr_runtime_host(core_clr_instance* core_clr_instance)
{
    if (core_clr_instance == nullptr)
    {
        return nullptr;
    }

    ICLRRuntimeHost2* clr_runtime_host;
    const auto pfn_get_clr_runtime_host =
        reinterpret_cast<FnGetCLRRuntimeHost>(::GetProcAddress(core_clr_instance->to_native_ptr(), "GetCLRRuntimeHost"));

    if (!pfn_get_clr_runtime_host)
    {
        std::wcerr << L"ERROR - GetCLRRuntimeHost not found." << std::endl;
        return nullptr;
    }

    const auto hr = pfn_get_clr_runtime_host(IID_ICLRRuntimeHost2, reinterpret_cast<IUnknown**>(&clr_runtime_host));
    if (FAILED(hr))
    {
        std::wcerr << L"ERROR - Failed to get ICLRRuntimeHost2 instance. Error code: " << hr << std::endl;
        return nullptr;
    }

    return clr_runtime_host;
}

core_clr_instance* snap::coreclr::try_load_core_clr(const std::wstring & executable_path, const std::vector<std::wstring>& arguments,
    const version::Semver200_version & clr_minimum_version)
{
    wchar_t* executable_directory_path = nullptr;
    wchar_t* core_root_path_out = nullptr;
    if (!pal_fs_get_directory_name_full_path(executable_path.c_str(), &executable_directory_path))
    {
        return nullptr;
    }

    // 1. Try loading from executable working directory if the application is self-contained.
    auto core_clr_instance = try_load_core_clr(executable_directory_path, version::Semver200_version());

    // 2. Try loading from default coreclr well-known directory.
    if (core_clr_instance == nullptr)
    {
        wchar_t* core_clr_program_files_directory_expanded = nullptr;
        if (pal_env_expand_str(core_clr_program_files_directory_path, &core_clr_program_files_directory_expanded)
            && core_clr_program_files_directory_expanded != nullptr)
        {
            const auto core_clr_directories = get_core_directories_from_path(
                core_clr_program_files_directory_expanded, clr_minimum_version);

            for (const auto& clr_directory : core_clr_directories)
            {
                core_clr_instance = try_load_core_clr(clr_directory.get_root_path(), clr_directory.get_version());
                if (core_clr_instance != nullptr)
                {
                    break;
                }
            }

            delete[] core_clr_program_files_directory_expanded;
        }
    }

    delete[] executable_directory_path;
    delete[] core_root_path_out;

    return core_clr_instance;
}

std::vector<core_clr_directory> snap::coreclr::get_core_directories_from_path(const wchar_t* core_clr_root_path,
    const version::Semver200_version& clr_minimum_version)
{
    wchar_t** paths_out = nullptr;
    size_t paths_out_len = 0;

    std::vector<core_clr_directory> core_clr_directories;

    if (!pal_fs_list_directories(core_clr_root_path, &paths_out, &paths_out_len))
    {
        return core_clr_directories;
    }

    std::vector<wchar_t*> core_clr_paths(
        paths_out, paths_out + paths_out_len);

    for (const auto& core_clr_path : core_clr_paths)
    {
        wchar_t* clr_directory_core_clrversion = nullptr;
        if (!pal_fs_get_directory_name(core_clr_path, &clr_directory_core_clrversion))
        {
            delete[] core_clr_path;
            continue;
        }

        try
        {
            auto core_clr_version = version::Semver200_version(pal_str_narrow(clr_directory_core_clrversion));

            if (core_clr_version < clr_minimum_version) {
                continue;
            }

            wchar_t* core_clr_dll_path = nullptr;
            if (!pal_fs_path_combine(core_clr_path, core_clr_dll, &core_clr_dll_path))
            {
                continue;
            }

            auto core_clr_dll_exists = FALSE;
            if (!pal_fs_file_exists(core_clr_dll_path, &core_clr_dll_exists)
                || !core_clr_dll_exists)
            {
                continue;
            }

            core_clr_directories.emplace_back(
                core_clr_directory(core_clr_path, core_clr_dll_path, core_clr_version));

            delete[] core_clr_dll_path;
        }
        catch (const version::Parse_error& e)
        {
            std::wcerr << L"Error - Failed to parse semver version for path: " << core_clr_path << ". Why: " << e.what() << std::endl;
        }

        delete[] core_clr_path;
    }

    std::sort(core_clr_directories.begin(), core_clr_directories.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.get_version() < rhs.get_version();
        });

    return core_clr_directories;
}

core_clr_instance* snap::coreclr::try_load_core_clr(const wchar_t* directory_path, const version::Semver200_version& core_clr_version)
{
    wchar_t* core_clr_dll_path = nullptr;
    if (!pal_fs_path_combine(directory_path, core_clr_dll, &core_clr_dll_path))
    {
        return nullptr;
    }

    auto core_clr_dll_exists = FALSE;
    if (!pal_fs_file_exists(core_clr_dll_path, &core_clr_dll_exists)
        || !core_clr_dll_exists)
    {
        return nullptr;
    }

    core_clr_instance_t* core_clr_instance_ptr = nullptr;

#if PLATFORM_WINDOWS
    core_clr_instance_ptr = LoadLibraryEx(core_clr_dll_path, nullptr, 0);
    if (!core_clr_instance_ptr) {
        return nullptr;
    }

    // Pin the module since CoreCLR.dll does not support being unloaded.
    HMODULE core_clr_dummy_module;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, core_clr_dll_path, &core_clr_dummy_module)) {
        return nullptr;
    }
#else
#error TODO: IMPLEMENT ME
#endif

    const auto core_clr_instance_out = new core_clr_instance(core_clr_instance_ptr,
       directory_path, core_clr_dll_path, core_clr_version);

    delete[] core_clr_dll_path;

    return core_clr_instance_out;
}

STARTUP_FLAGS snap::coreclr::create_clr_startup_flags()
{
    auto initial_startup_flags =
        static_cast<STARTUP_FLAGS>(
            STARTUP_LOADER_OPTIMIZATION_SINGLE_DOMAIN |
            STARTUP_SINGLE_APPDOMAIN |
            STARTUP_CONCURRENT_GC);

    const auto read_flag_for_environment_variable = [&](const STARTUP_FLAGS startup_flag, const wchar_t *environment_variable) {
        auto is_flag_enabled = FALSE;
        if (!pal_env_get_variable_bool(environment_variable, &is_flag_enabled))
        {
            return;
        }

        if (is_flag_enabled)
        {
            initial_startup_flags = static_cast<STARTUP_FLAGS>(initial_startup_flags | startup_flag);
            return;
        }

        initial_startup_flags = static_cast<STARTUP_FLAGS>(initial_startup_flags & ~startup_flag);
    };

    read_flag_for_environment_variable(STARTUP_SERVER_GC, server_gc_environment_var);
    read_flag_for_environment_variable(STARTUP_CONCURRENT_GC, concurrent_gc_environment_var);

    return initial_startup_flags;
}

BOOL snap::coreclr::create_clr_appdomain(const std::wstring& executable_path, const wchar_t* executable_working_directory,
    const std::wstring& trusted_platform_assemblies, core_clr_instance * const core_clr_instance, unsigned long* appdomain_id_out)
{
    if (executable_path.empty()
        || executable_working_directory == nullptr
        || trusted_platform_assemblies.empty()
        || core_clr_instance == nullptr
        || !core_clr_instance->is_instance_loaded()
        || !core_clr_instance->is_host_created())
    {
        return FALSE;
    }

    std::wstring executable_paths(executable_working_directory);
    executable_paths.append(L";");
    executable_paths.append(core_clr_instance->get_directory().get_root_path());
    executable_paths.append(L";");
    executable_paths.append(L"NI");
    executable_paths.append(L";");
    executable_paths.append(executable_working_directory);

    const wchar_t *property_keys[] = {
       L"TRUSTED_PLATFORM_ASSEMBLIES",
       L"APP_PATHS",
       L"APP_NI_PATHS",
       L"NATIVE_DLL_SEARCH_DIRECTORIES",
       L"APP_LOCAL_WINMETADATA"
    };

    const wchar_t *property_values[] = {
        // TRUSTED_PLATFORM_ASSEMBLIES
        trusted_platform_assemblies.c_str(),
        // APP_PATHS
        executable_paths.c_str(),
        // APP_NI_PATHS
        executable_paths.c_str(),
        // NATIVE_DLL_SEARCH_DIRECTORIES
        executable_paths.c_str(),
        // APP_LOCAL_WINMETADATA
        executable_paths.c_str(),
    };

    const auto hr = core_clr_instance->get_host()->CreateAppDomainWithManager(
        // The friendly name of the AppDomain
        executable_path.c_str(),
        // Flags:
        // APPDOMAIN_ENABLE_PLATFORM_SPECIFIC_APPS
        // - By default CoreCLR only allows platform neutral assembly to be run. To allow
        //   assemblies marked as platform specific, include this flag
        //
        // APPDOMAIN_ENABLE_PINVOKE_AND_CLASSIC_COMINTEROP
        // - Allows sandboxed applications to make P/Invoke calls and use COM interop
        //
        // APPDOMAIN_SECURITY_SANDBOXED
        // - Enables sandboxing. If not set, the app is considered full trust
        //
        // APPDOMAIN_IGNORE_UNHANDLED_EXCEPTION
        // - Prevents the application from being torn down if a managed exception is unhandled
        //
        APPDOMAIN_ENABLE_PLATFORM_SPECIFIC_APPS |
        APPDOMAIN_ENABLE_PINVOKE_AND_CLASSIC_COMINTEROP |
        APPDOMAIN_DISABLE_TRANSPARENCY_ENFORCEMENT,
        // Name of the assembly that contains the AppDomainManager implementation
        nullptr,
        // The AppDomainManager implementation type name
        nullptr,
        // The number of properties
        sizeof property_keys / sizeof(wchar_t*),
        property_keys,
        property_values,
        appdomain_id_out);

    if (FAILED(hr))
    {
        std::wcerr << L"ERROR - Failed to create APPDOMAIN. Error code: " << hr << std::endl;
        return FALSE;
    }

    return TRUE;
}

std::vector<std::wstring> snap::coreclr::get_trusted_platform_assemblies(const wchar_t* trusted_platform_assemblies_path)
{
    if (trusted_platform_assemblies_path == nullptr)
    {
        return std::vector<std::wstring>();
    }

    static const std::vector<const wchar_t*> trusted_platform_assemblies_extension_list{
        L"*.ni.dll", // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
        L"*.dll",
        L"*.ni.exe",
        L"*.exe",
        L"*.ni.winmd",
        L"*.winmd"
    };

    std::vector<std::wstring> trusted_platform_assemblies_list;

    for (const auto& extension : trusted_platform_assemblies_extension_list)
    {
        wchar_t** files_out = nullptr;
        size_t files_out_len = 0;
        if (!pal_fs_list_files(trusted_platform_assemblies_path, nullptr, extension, &files_out, &files_out_len))
        {
            continue;
        }

        std::vector<wchar_t*> files(files_out, files_out + files_out_len);

        for (auto const& file : files)
        {
            const auto is_previously_added = std::find(
                trusted_platform_assemblies_list.begin(),
                trusted_platform_assemblies_list.end(), file) != trusted_platform_assemblies_list.end();
            if (is_previously_added)
            {
                continue;
            }

            trusted_platform_assemblies_list.emplace_back(std::wstring(file));
        }
    }

    return trusted_platform_assemblies_list;
}

std::wstring snap::coreclr::build_trusted_platform_assemblies_str(const std::wstring& executable_path,
    core_clr_instance* const core_clr_instance)
{
    if (core_clr_instance == nullptr)
    {
        return std::wstring();
    }

    std::vector<std::wstring> trusted_platform_assemblies;
    auto trusted_platform_assemblies_str = std::wstring();

    for (auto const& tpa : get_trusted_platform_assemblies(core_clr_instance->get_directory().get_root_path()))
    {
        trusted_platform_assemblies.emplace_back(tpa);
    }

    const auto is_managed_assembly_added_to_tpa = std::find(
        trusted_platform_assemblies.begin(),
        trusted_platform_assemblies.end(), executable_path) != trusted_platform_assemblies.end();
    if (!is_managed_assembly_added_to_tpa)
    {
        trusted_platform_assemblies.emplace_back(executable_path);
    }

    for (auto i = 0u; i < trusted_platform_assemblies.size(); i++)
    {
        trusted_platform_assemblies_str.append(trusted_platform_assemblies[i]);

        if (i + 1 < trusted_platform_assemblies.size())
        {
            trusted_platform_assemblies_str.append(L";");
        }
    }

    return trusted_platform_assemblies_str;
}

LPCWSTR snap::coreclr::to_clr_arguments(const std::vector<std::wstring>& arguments, int* argc, std::wstring& argw_ws)
{
    *argc = static_cast<int>(arguments.size());

    for (auto i = 0; i < *argc; i++)
    {
        argw_ws.append(arguments[i]);
        if (i + 1 < *argc)
        {
            argw_ws.append(L" ");
        }
    }

    return const_cast<LPCWSTR>(argw_ws.data());
}
