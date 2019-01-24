#include "pal.hpp"

#if PLATFORM_WINDOWS
#include <Shlwapi.h> // PathIsDirectory, PathFileExists etc.
#include <PathCch.h> // PathCchCombine etc.
#include <strsafe.h> // StringCchLengthA
#endif

#if PLATFORM_LINUX
#include <sys/stat.h> // stat
#include <sys/types.h> // O_RDONLY
#include <unistd.h> // getcwd
#include <fcntl.h> // open
#include <dirent.h> // opendir
#include <libgen.h> // dirname
#include <dlfcn.h> // dlopen
#include <cassert> // assert

// GLOBALS

static const char* symlink_entrypoint_executable = "/proc/self/exe";
#endif

#include <regex>

// - Generic
BOOL pal_isdebuggerpresent()
{
#if PLATFORM_WINDOWS
    return ::IsDebuggerPresent() ? TRUE : FALSE;
#elif PLATFORM_LINUX
    // https://github.com/dotnet/coreclr/blob/4a6753dcacf44df6a8e91b91029e4b7a4f12d917/src/pal/src/init/pal.cpp#L821
    BOOL debugger_present = FALSE;
    char buf[2048];

    int status_fd = open("/proc/self/status", O_RDONLY);
    if (status_fd == -1)
    {
        return FALSE;
    }
    ssize_t num_read = read(status_fd, buf, sizeof(buf) - 1);

    if (num_read > 0)
    {
        static const char TracerPid[] = "TracerPid:";
        char *tracer_pid;

        buf[num_read] = '\0';
        tracer_pid = strstr(buf, TracerPid);
        if (tracer_pid)
        {
            debugger_present = !!atoi(tracer_pid + sizeof(TracerPid) - 1);
        }
    }

    close(status_fd);

    return debugger_present;
#endif
    return FALSE;
}

BOOL pal_load_library(const char * name_in, BOOL pinning_required, void** instance_out)
{
    if (name_in == nullptr)
    {
        return FALSE;
    }

#if PLATFORM_WINDOWS
    pal_utf16_string name_in_utf16_string(name_in);

    auto h_module = LoadLibraryEx(name_in_utf16_string.data(), nullptr, 0);
    if (!h_module)
    {
        return FALSE;
    }

    if (pinning_required)
    {
        HMODULE dummy_module;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name_in_utf16_string.data(), &dummy_module)) {
            pal_free_library(h_module);
            return FALSE;
        }
    }

    *instance_out = static_cast<void*>(h_module);

    return TRUE;

#elif PLATFORM_LINUX

    auto instance = dlopen(name_in, RTLD_NOW | RTLD_LOCAL);
    if (!instance)
    {
        return FALSE;
    }

    *instance_out = instance;
    return TRUE;
#endif
    return FALSE;
}

BOOL pal_free_library(void* instance_in)
{
    if (instance_in == nullptr)
    {
        return FALSE;
    }
#if PLATFORM_WINDOWS
    auto free_library_result = FreeLibrary(static_cast<HMODULE>(instance_in));
    if (!free_library_result)
    {
        LOG(WARNING) << "FreeLibrary failed. result: " << free_library_result;
    }
    return TRUE;
#elif PLATFORM_LINUX
    auto dlclose_result = dlclose(instance_in);
    if (dlclose_result != 0)
    {
        LOG(WARNING) << "dlclose failed. result: " << dlclose_result;
    }
    return TRUE;
#endif
    return FALSE;
}

BOOL pal_getprocaddress(void* instance_in, const char* name_in, void** ptr_out)
{
    if (instance_in == nullptr)
    {
        return FALSE;
    }

#if PLATFORM_WINDOWS

    auto h_module = static_cast<HMODULE>(instance_in);
    auto h_module_ptr_out = ::GetProcAddress(h_module, name_in);
    if (h_module_ptr_out == nullptr)
    {
        return FALSE;
    }

    *ptr_out = reinterpret_cast<void*>(h_module_ptr_out);
    return TRUE;
#elif PLATFORM_LINUX
    auto dlsym_ptr_out = dlsym(instance_in, name_in);
    if (dlerror() != nullptr)
    {
        return FALSE;
    }

    *ptr_out = dlsym_ptr_out;

    return TRUE;
#endif

    return 0;
}

// - Environment
BOOL pal_env_get_variable(const char * environment_variable_in, char ** environment_variable_value_out)
{
    if (environment_variable_in == nullptr)
    {
        return FALSE;
    }

#if PLATFORM_WINDOWS
    pal_utf16_string environment_variable_in_utf16_string(environment_variable_in);

    wchar_t* buffer = nullptr;
    size_t buffer_len = 0;
    const auto error = _wdupenv_s(&buffer, &buffer_len, environment_variable_in_utf16_string.data());
    if (error || buffer_len <= 0)
    {
        return FALSE;
    }

    *environment_variable_value_out = pal_utf8_string(buffer).dup();
    delete buffer;

    return TRUE;
#else
    auto value = std::getenv(environment_variable_in);
    if (value == nullptr)
    {
        return FALSE;
    }

    *environment_variable_value_out = std::getenv(environment_variable_in);
    return TRUE;
#endif
}

BOOL pal_env_get_variable_bool(const char * environment_variable_in, BOOL* env_value_bool_out)
{
    char* environment_variable_value_out = nullptr;
    if (!pal_env_get_variable(environment_variable_in, &environment_variable_value_out))
    {
        return FALSE;
    }

    *env_value_bool_out = pal_str_iequals(environment_variable_value_out, "1") == 0
        || pal_str_iequals(environment_variable_value_out, "true") == 0 ? TRUE : FALSE;

    delete environment_variable_value_out;

    return TRUE;
}

BOOL pal_env_expand_str(const char * environment_in, char ** environment_out)
{
    if (environment_in == nullptr)
    {
        return FALSE;
    }

    std::string environment_in_str(environment_in);

#if PLATFORM_WINDOWS
    static std::regex expression(R"(%([0-9A-Za-z\\/\(\)]*)%)", std::regex_constants::icase);
#else
    static std::regex expression(R"(\$\{([^}]+)\})", std::regex_constants::icase);
#endif

    auto replacements = 0;
    std::smatch match;
    while (std::regex_search(environment_in_str, match, expression)) {
        const auto match_str = match[1].str();
        char* environment_variable_value = nullptr;
        if (!pal_env_get_variable(match_str.c_str(), &environment_variable_value))
        {
            continue;
        }

        const std::string environment_variable_value_s(environment_variable_value);
        delete environment_variable_value;

        environment_in_str.replace(match[0].first, match[0].second, environment_variable_value_s);

        replacements++;
    }

    if (replacements <= 0)
    {
        return FALSE;
    }

    *environment_out = strdup(environment_in_str.c_str());

    return TRUE;
}

// - Filesystem
BOOL pal_fs_get_directory_name_absolute_path(const char* path_in, char** path_out)
{
    if (path_in == nullptr)
    {
        return FALSE;
    }
#if PLATFORM_WINDOWS

    pal_utf16_string path_in_utf16_string(path_in);

    wchar_t path_in_without_filespec[PAL_MAX_PATH];
    wcscpy_s(path_in_without_filespec, PAL_MAX_PATH, path_in_utf16_string.data());

    if (PathIsDirectory(path_in_utf16_string.data()))
    {
        if (S_OK != PathCchCanonicalize(path_in_without_filespec, PAL_MAX_PATH, path_in_utf16_string.data()))
        {
            return FALSE;
        }
    }
    else if (S_OK != ::PathCchRemoveFileSpec(path_in_without_filespec, PAL_MAX_PATH))
    {
        return FALSE;
    }

    if (0 == wcscmp(path_in_without_filespec, L""))
    {
        return FALSE;
    }

    *path_out = pal_utf8_string(path_in_without_filespec).dup();

    return TRUE;
#else
    auto path_in_cpy = strdup(path_in);
    char* dir = dirname(path_in_cpy);
    if (dir)
    {
        //  Both dirname() and basename() return pointers to null-terminated
        // strings.  (Do not pass these pointers to free(3).)
        *path_out = strdup(dir);
        return TRUE;
    }
    delete path_in_cpy;
    return FALSE;
#endif
}

BOOL pal_fs_get_directory_name(const char * path_in, char ** path_out)
{
    if (path_in == nullptr)
    {
        return FALSE;
    }

    std::string path_in_s(path_in);

    const auto directory_name_start_pos = path_in_s.find_last_of(PAL_DIRECTORY_SEPARATOR_C);
    if (directory_name_start_pos == std::string::npos)
    {
        return FALSE;
    }

    const auto directory_name = path_in_s.substr(directory_name_start_pos + 1);

    *path_out = strdup(directory_name.c_str());

    return TRUE;
}

BOOL pal_fs_path_combine(const char * path1, const char * path2, char ** path_out)
{
    if (path1 == nullptr
        || path2 == nullptr)
    {
        return FALSE;
    }
#if PLATFORM_WINDOWS

    pal_utf16_string path_combined_utf16_string(PAL_MAX_PATH);
    pal_utf16_string path_in_lhs_utf16_string(path1);
    pal_utf16_string path_in_rhs_utf16_string(path2);

    if (S_OK != PathCchCombine(path_combined_utf16_string.data(), PAL_MAX_PATH,
        path_in_lhs_utf16_string.data(), path_in_rhs_utf16_string.data()))
    {
        return FALSE;
    }

    *path_out = pal_utf8_string(path_combined_utf16_string.data()).dup();

    return TRUE;
#elif PLATFORM_LINUX
    /*

    Adapted from: https://github.com/qpalzmqaz123/path_combine/blob/master/path_combine.c

    typedef struct {
        const char *path1;
        const char *path2;
        const char *combined;
    } test_t;

    test_t test_data[] = {
        { "/a/b/c", "/c/d/e", "/c/d/e" },
        { "/a/b/c", "d", "/a/b/c/d" },
        { "/foo/bar", "./baz", "/foo/bar/baz" },
        { "/foo/bar", "./baz/", "/foo/bar/baz" },
        { "a", ".",  "a"},
        { "a.", ".",  "a."},
        { "a./b.", ".",  "a./b."},
        { "a/b", "..",  "a"},
        { "a", "..a",  "a/..a"},
        { "a", "../a",  NULL},
        { "a", "c../a",  "a/c../a"},
        { "a/b", "../",  "a"},
        { "a/b", ".././c/d/../../.",  "a"},
        { NULL, NULL, NULL }
    };

    */

    char buffer[PAL_MAX_PATH];

    std::function<int(char*)> path_combine_recursive;
    path_combine_recursive = [path_combine_recursive](char *path)
    {
        char *str;

        if (0 == strlen(path)) {
            return 0;
        }

        // Resolve parent dir
        while (true) {
            str = strstr(path, "/../");
            if (nullptr == str) {
                break;
            }

            *str = 0;
            if (nullptr == strchr(path, '/')) {
                return 1;
            }

            const auto parent_dir = strrchr(path, '/') + 1;
            const auto current_dir = str + 4;

            // Replace parent dir
            memcpy(parent_dir, current_dir, strlen(current_dir) + 1);
        }

        // Resolve current dir
        while (true) {
            str = strstr(path, "/./");
            if (nullptr == str) {
                break;
            }

            memcpy(str + 1, str + 3, strlen(str + 3) + 1);
        }

        // Remove tail '/' or '/.' 
        const auto tail = path + strlen(path) - 1;
        if ('/' == *tail) {
            *tail = 0;
        }
        else if (0 == strcmp(tail - 1, "/.")) {
            *(tail - 1) = 0;
        }
        else if (0 == strcmp(tail - 2, "/..")) {
            strcat(path, "/");
            path_combine_recursive(path);
        }

        return 0;
    };

    const auto path_combine = [path_combine_recursive, path_out](char* buffer)
    {
        if (0 == path_combine_recursive(buffer)) {
            *path_out = strdup(buffer);
            return TRUE;
        }
        return FALSE;
    };

    if (nullptr == path1 && nullptr == path2) {
        return FALSE;
    }
    if (nullptr == path1) {
        strcpy(buffer, path2);
        return path_combine(buffer);
    }
    if (nullptr == path2) {
        strcpy(buffer, path1);
        return path_combine(buffer);
    }

    if ('/' == path2[0]) {
        strcpy(buffer, path2);
        return path_combine(buffer);
    }

    strcpy(buffer, path1);

    if ('/' != path1[strlen(path1) - 1]) {
        strcat(buffer, "/");
    }

    strcat(buffer, path2);
    return path_combine(buffer);
#endif
    return FALSE;
}

BOOL pal_fs_file_exists(const char * file_path_in, BOOL *file_exists_bool_out)
{
    if (file_path_in == nullptr)
    {
        return FALSE;
    }

#if PLATFORM_WINDOWS
    *file_exists_bool_out = PathFileExists(pal_utf16_string(file_path_in).data());
    return TRUE;
#elif PLATFORM_LINUX
    *file_exists_bool_out = access(file_path_in, F_OK) != -1;
    return TRUE;
#endif
    return FALSE;
}

BOOL pal_fs_list_impl(const char * path_in, const pal_fs_list_filter_callback_t filter_callback_in,
    const char* filter_extension_in, char *** paths_out, size_t * paths_out_len, const int type)
{
    if (path_in == nullptr)
    {
        return false;
    }

    std::vector<char*> paths;
    auto paths_success = FALSE;

#if PLATFORM_WINDOWS

    const pal_utf16_string extension_filter_in_utf16_string(
        filter_extension_in != nullptr ? filter_extension_in : "*");

    pal_utf16_string path_root_utf16_string(path_in);
    if (!path_root_utf16_string.ends_with(PAL_DIRECTORY_SEPARATOR_WIDE_STR))
    {
        path_root_utf16_string.append(PAL_DIRECTORY_SEPARATOR_WIDE_STR);
    }

    path_root_utf16_string.append(extension_filter_in_utf16_string.str());

    WIN32_FIND_DATA file;
    const auto h_file = FindFirstFile(path_root_utf16_string.data(), &file);
    if (h_file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    do
    {
        switch (type)
        {
        case 0:
            if (!(file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                continue;
            }
            break;
        case 1:
            if (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            break;
        default:
            continue;
        }

        auto relative_path = pal_utf8_string(file.cFileName);
        if (pal_str_iequals(relative_path.data(), ".")
            || pal_str_iequals(relative_path.data(), ".."))
        {
            continue;
        }

        char* absolute_path = nullptr;
        if (!pal_fs_path_combine(path_in, relative_path.data(), &absolute_path))
        {
            delete[] absolute_path;
            continue;
        }

        const auto filter_callback_fn = filter_callback_in;
        if (filter_callback_fn != nullptr
            && !filter_callback_fn(absolute_path))
        {
            delete[] absolute_path;
            continue;
        }

        paths.emplace_back(absolute_path);

    } while (FindNextFile(h_file, &file));

    ::FindClose(h_file);

    paths_success = TRUE;

#elif PLATFORM_LINUX

    std::string filter_extension_s(filter_extension_in == nullptr ? std::string() : filter_extension_in);

    DIR* dir = opendir(path_in);
    paths_success = dir != nullptr;
    if (paths_success)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            std::string absolute_path_s;

            switch (type)
            {
            case 0:
                if (entry->d_type != DT_DIR)
                {
                    continue;
                }

                if (pal_str_iequals(entry->d_name, ".") ||
                    pal_str_iequals(entry->d_name, ".."))
                {
                    continue;
                }

                absolute_path_s.assign(path_in);
                absolute_path_s.append("/");
                absolute_path_s.append(entry->d_name);

                break;
            case 1:
                switch (entry->d_type)
                {
                    // Regular file
                case DT_REG:
                    if (filter_extension_in != nullptr && FALSE == pal_str_endswith(entry->d_name, filter_extension_in))
                    {
                        continue;
                    }

                    absolute_path_s.assign(path_in);
                    absolute_path_s.append("/");
                    absolute_path_s.append(entry->d_name);
                    break;

                    // Handle symlinks and file systems that do not support d_type
                case DT_LNK:
                case DT_UNKNOWN:
                    if (filter_extension_in != nullptr && FALSE == pal_str_endswith(entry->d_name, filter_extension_in))
                    {
                        continue;
                    }

                    absolute_path_s.assign(path_in);
                    absolute_path_s.append("/");
                    absolute_path_s.append(entry->d_name);

                    struct stat sb;
                    if (stat(absolute_path_s.c_str(), &sb) == -1)
                    {
                        absolute_path_s.clear();
                        continue;
                    }

                    // Must be a regular file.
                    if (!S_ISREG(sb.st_mode))
                    {
                        absolute_path_s.clear();
                        continue;
                    }

                    break;
                }
                break;
            default:
                // void
                break;
            }

            if (absolute_path_s.empty())
            {
                continue;
            }

            const auto filter_callback_fn = filter_callback_in;
            if (filter_callback_fn != nullptr
                && !filter_callback_fn(absolute_path_s.c_str()))
            {
                continue;
            }

            paths.emplace_back(strdup(absolute_path_s.data()));
        }

        closedir(dir);
    }
#endif

    if (!paths_success)
    {
        return FALSE;
    }

    *paths_out_len = paths.size();

    const auto paths_array = new char*[*paths_out_len];

    for (auto i = 0u; i < *paths_out_len; i++)
    {
        paths_array[i] = paths[i];
    }

    *paths_out = paths_array;

    return TRUE;
}

BOOL pal_fs_list_directories(const char * path_in, const pal_fs_list_filter_callback_t filter_callback_in,
    const char* filter_extension_in, char *** directories_out, size_t* directories_out_len)
{
    return pal_fs_list_impl(path_in, filter_callback_in, filter_extension_in, directories_out, directories_out_len, 0);
}

BOOL pal_fs_list_files(const char * path_in, const pal_fs_list_filter_callback_t filter_callback_in,
    const char* filter_extension_in, char *** files_out, size_t * files_out_len)
{
    return pal_fs_list_impl(path_in, filter_callback_in, filter_extension_in, files_out, files_out_len, 1);
}

BOOL pal_fs_get_cwd(char ** working_directory_out)
{
#if PLATFORM_WINDOWS
    pal_utf16_string cwd_utf16_string(PAL_MAX_PATH);
    GetModuleFileName(GetModuleHandle(nullptr), cwd_utf16_string.data(), PAL_MAX_PATH);

    const auto directory_separator_pos = cwd_utf16_string.str().find_last_of(PAL_DIRECTORY_SEPARATOR_C);
    if (std::string::npos == directory_separator_pos) {
        return FALSE;
    }

    const auto cwd = cwd_utf16_string.str().substr(0, directory_separator_pos);

    *working_directory_out = pal_utf8_string(cwd).dup();

    return TRUE;
#elif PLATFORM_LINUX
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != nullptr)
    {
        *working_directory_out = strdup(cwd);
        return TRUE;
    }
#endif
    return FALSE;
}

BOOL pal_fs_get_own_executable_name(char ** own_executable_name_out)
{
#if PLATFORM_WINDOWS

    pal_utf16_string pal_current_directory_utf16_string(PAL_MAX_PATH);
    GetModuleFileName(GetModuleHandle(nullptr), pal_current_directory_utf16_string.data(), PAL_MAX_PATH);

    const auto directory_separator_pos = pal_current_directory_utf16_string.str().find_last_of(PAL_DIRECTORY_SEPARATOR_C);
    if (std::string::npos == directory_separator_pos) {
        return FALSE;
    }

    const auto executable_name = pal_current_directory_utf16_string.str().substr(directory_separator_pos + 1);

    *own_executable_name_out = pal_utf8_string(executable_name).dup();

    return TRUE;
#elif PLATFORM_LINUX
    char* real_path = nullptr;
    if (pal_fs_get_absolute_path(symlink_entrypoint_executable, &real_path))
    {
        const auto real_path_str = std::string(real_path);
        const auto real_path_directory_separator_pos = real_path_str.find_last_of(PAL_DIRECTORY_SEPARATOR_C);
        if (std::string::npos == real_path_directory_separator_pos)
        {
            return FALSE;
        }

        const auto executable_name = real_path_str.substr(real_path_directory_separator_pos + 1);

        *own_executable_name_out = strdup(executable_name.c_str());

        return TRUE;
    }
#endif
    return FALSE;
}

BOOL pal_fs_get_absolute_path(const char * path_in, char ** path_absolute_out)
{
    if (path_in == nullptr)
    {
        return FALSE;
    }

#if PLATFORM_WINDOWS

    pal_utf16_string path_in_utf16_string(path_in);
    pal_utf16_string path_absolute_out_utf16_string(PAL_MAX_PATH);
    const auto path_absolute_out_len = GetLongPathName(path_in_utf16_string.data(),
        path_absolute_out_utf16_string.data(), PAL_MAX_PATH);
    if (path_absolute_out_len == 0)
    {
        return FALSE;
    }

    *path_absolute_out = pal_utf8_string(path_absolute_out_utf16_string.data()).dup();

    return TRUE;
#elif PLATFORM_LINUX
    char real_path[PATH_MAX];
    if (realpath(path_in, real_path) != nullptr && real_path[0] != '\0')
    {
        std::string real_path_str(real_path);

        // realpath should return canonicalized path without the trailing slash
        assert(real_path_str.back() != '/');

        *path_absolute_out = strdup(real_path_str.c_str());

        return TRUE;
    }
#endif
    return FALSE;
}

BOOL pal_fs_directory_exists(const char * path_in, BOOL * directory_exists_out)
{
    if (path_in == nullptr)
    {
        return FALSE;
    }

#if PLATFORM_WINDOWS
    pal_utf16_string path_in_utf16_string(path_in);
    const auto attributes = GetFileAttributes(path_in_utf16_string.data());
    *directory_exists_out = attributes & FILE_ATTRIBUTE_DIRECTORY;
#elif PLATFORM_LINUX
    auto directory = opendir(path_in);
    if (directory)
    {
        *directory_exists_out = TRUE;
        closedir(directory);
        return TRUE;
    }
    *directory_exists_out = FALSE;
    return FALSE;
#endif
    return 0;
}

BOOL pal_str_endswith(const char * src, const char * str)
{
    if (src == nullptr || str == nullptr)
    {
        return FALSE;
    }

    const auto diff = strlen(src) - strlen(str);
    return diff > 0 && 0 == strcmp(&src[diff], str) ? TRUE : FALSE;
}

BOOL pal_str_startswith(const char * src, const char * str)
{
    if (src == nullptr || str == nullptr)
    {
        return FALSE;
    }

    const auto src_len = strlen(src);
    const auto suffix_len = strlen(str);
    const auto starts_with = src_len < suffix_len ? false : strncmp(src, str, src_len) == 0;

    return starts_with ? TRUE : FALSE;
}

BOOL pal_str_iequals(const char* lhs, const char* rhs)
{
    std::string str1(lhs);
    std::string str2(rhs);

    const auto iequals = str1.size() == str2.size()
        && std::equal(str1.begin(), str1.end(), str2.begin(), [](char & c1, char & c2) {
        return c1 == c2 || std::toupper(c1) == std::toupper(c2);
            });

    return iequals ? TRUE : FALSE;
}