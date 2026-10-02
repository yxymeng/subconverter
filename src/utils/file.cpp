#include <string>
#include <fstream>
#include <sys/stat.h>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#ifdef __linux__
#include <sys/xattr.h>
#elif defined(__APPLE__)
#include <sys/acl.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntdef.h>
#include <io.h>
#endif

#include "utils/string.h"

bool isInScope(const std::string &path)
{
#ifdef _WIN32
    if(path.find(":\\") != path.npos || path.find("..") != path.npos)
        return false;
#else
    if(startsWith(path, "/") || path.find("..") != path.npos)
        return false;
#endif // _WIN32
    return true;
}

// TODO: Add preprocessor option to disable (open web service safety)
std::string fileGet(const std::string &path, bool scope_limit)
{
    std::string content;

    if(scope_limit && !isInScope(path))
        return "";

    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if(fp)
    {
        std::fseek(fp, 0, SEEK_END);
        long tot = std::ftell(fp);
        /*
        char *data = new char[tot + 1];
        data[tot] = '\0';
        std::rewind(fp);
        std::fread(&data[0], 1, tot, fp);
        std::fclose(fp);
        content.assign(data, tot);
        delete[] data;
        */
        content.resize(tot);
        std::rewind(fp);
        std::fread(&content[0], 1, tot, fp);
        std::fclose(fp);
    }

    /*
    std::stringstream sstream;
    std::ifstream infile;
    infile.open(path, std::ios::binary);
    if(infile)
    {
        sstream<<infile.rdbuf();
        infile.close();
        content = sstream.str();
    }
    */
    return content;
}

bool fileExist(const std::string &path, bool scope_limit)
{
    //using c++17 standard, but may cause problem on clang
    //return std::filesystem::exists(path);
    if(scope_limit && !isInScope(path))
        return false;
    struct stat st;
    return stat(path.data(), &st) == 0 && S_ISREG(st.st_mode);
}

bool fileCopy(const std::string &source, const std::string &dest)
{
    std::ifstream infile;
    std::ofstream outfile;
    infile.open(source, std::ios::binary);
    if(!infile)
        return false;
    outfile.open(dest, std::ios::binary);
    if(!outfile)
        return false;
    try
    {
        outfile<<infile.rdbuf();
    }
    catch (std::exception &e)
    {
        return false;
    }
    infile.close();
    outfile.close();
    return true;
}

int fileWrite(const std::string &path, const std::string &content, bool overwrite)
{
    /*
    std::fstream outfile;
    std::ios_base::openmode mode = overwrite ? std::ios_base::out : std::ios_base::app;
    mode |= std::ios_base::binary;
    outfile.open(path, mode);
    outfile << content;
    outfile.close();
    return 0;
    */
    const char *mode = overwrite ? "wb" : "ab";
    std::FILE *fp = std::fopen(path.c_str(), mode);
    if(!fp) return -1;
    const bool complete = std::fwrite(content.c_str(), 1, content.size(), fp) == content.size();
    const int closed = std::fclose(fp);
    return complete && closed == 0 ? 0 : -1;
}

#ifndef _WIN32
static bool preserveFilePermissions(const std::filesystem::path &path, int descriptor, const struct stat &original)
{
    if(fchmod(descriptor, 0) != 0 || fchown(descriptor, original.st_uid, original.st_gid) != 0)
        return false;
#ifdef __linux__
    const char *attribute = "system.posix_acl_access";
    const auto size = getxattr(path.c_str(), attribute, nullptr, 0);
    if(size < 0)
    {
        if(errno != ENODATA && errno != ENOTSUP) return false;
        if(fremovexattr(descriptor, attribute) != 0 && errno != ENODATA && errno != ENOTSUP)
            return false;
    }
    else
    {
        std::string acl(size, '\0');
        if(getxattr(path.c_str(), attribute, acl.data(), acl.size()) != size ||
            fsetxattr(descriptor, attribute, acl.data(), acl.size(), 0) != 0)
            return false;
    }
#elif defined(__APPLE__)
    acl_t acl = acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
    if(!acl && errno == ENOENT)
    {
        struct stat current;
        if(stat(path.c_str(), &current) != 0) return false;
        acl = acl_init(0);
    }
    if(!acl) return false;
    const bool copied = acl_set_fd(descriptor, acl) == 0;
    acl_free(acl);
    if(!copied) return false;
#endif
    return fchmod(descriptor, original.st_mode & 07777) == 0;
}
#endif

int fileWriteAtomic(const std::string &path, const std::string &content)
{
    std::filesystem::path destination_path(path);
    std::error_code error;
    for(unsigned int hops = 0; ; ++hops)
    {
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(destination_path.c_str());
        if(attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD failure = GetLastError();
            if(failure != ERROR_FILE_NOT_FOUND && failure != ERROR_PATH_NOT_FOUND) return -1;
            break;
        }
        if(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) break;
        if(hops == 40) return -1;
        HANDLE link = CreateFileW(destination_path.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if(link == INVALID_HANDLE_VALUE) return -1;
        alignas(REPARSE_DATA_BUFFER) char data[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
        DWORD returned = 0;
        const bool read = DeviceIoControl(link, FSCTL_GET_REPARSE_POINT, nullptr, 0,
            data, sizeof(data), &returned, nullptr);
        CloseHandle(link);
        if(!read) return -1;
        const auto reparse = reinterpret_cast<const REPARSE_DATA_BUFFER *>(data);
        if(reparse->ReparseTag != IO_REPARSE_TAG_SYMLINK) return -1;
        const auto &buffer = reparse->SymbolicLinkReparseBuffer;
        std::wstring name(buffer.PathBuffer + buffer.SubstituteNameOffset / sizeof(wchar_t),
            buffer.SubstituteNameLength / sizeof(wchar_t));
        if(name.rfind(L"\\??\\UNC\\", 0) == 0)
            name = L"\\\\" + name.substr(8);
        else if(name.rfind(L"\\??\\", 0) == 0)
            name.erase(0, 4);
        const std::filesystem::path target(name);
#else
        const auto status = std::filesystem::symlink_status(destination_path, error);
        if(error)
        {
            if(error != std::errc::no_such_file_or_directory) return -1;
            break;
        }
        if(!std::filesystem::is_symlink(status)) break;
        if(hops == 40) return -1;
        const auto target = std::filesystem::read_symlink(destination_path, error);
        if(error) return -1;
#endif
        destination_path = target.is_absolute() ? target : destination_path.parent_path() / target;
    }
    static std::atomic<unsigned long long> sequence {0};
    auto temporary = destination_path;
    temporary += ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(++sequence);
#ifdef _WIN32
    const int descriptor = _wopen(temporary.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if(descriptor < 0) return -1;
    std::FILE *file = _fdopen(descriptor, "wb");
    if(!file) _close(descriptor);
#else
    struct stat destination;
    const bool existing = stat(destination_path.c_str(), &destination) == 0;
    if(!existing && errno != ENOENT) return -1;
    if(existing && destination.st_nlink > 1) return -1;
    const auto mode = existing ? 0600 : 0666;
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
    if(descriptor < 0) return -1;
    std::FILE *file = fdopen(descriptor, "wb");
    if(!file) close(descriptor);
#endif
    bool complete = false;
    if(file)
    {
        bool written = std::fwrite(content.data(), 1, content.size(), file) == content.size() && std::fflush(file) == 0;
#ifndef _WIN32
        if(written && existing)
            written = preserveFilePermissions(destination_path, descriptor, destination);
#endif
        complete = std::fclose(file) == 0 && written;
    }
    if(!complete)
    {
        std::filesystem::remove(temporary, error);
        return -1;
    }
    // Replace only after the complete temporary file has been closed successfully.
#ifdef _WIN32
    const bool existing = GetFileAttributesW(destination_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool replaced = existing
        ? ReplaceFileW(destination_path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
        : MoveFileExW(temporary.c_str(), destination_path.c_str(), MOVEFILE_WRITE_THROUGH);
#else
    const bool replaced = std::rename(temporary.c_str(), destination_path.c_str()) == 0;
#endif
    if(!replaced)
    {
        std::filesystem::remove(temporary, error);
        return -1;
    }
    return 0;
}
