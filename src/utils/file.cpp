#include <string>
#include <fstream>
#include <sys/stat.h>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

int fileWriteAtomic(const std::string &path, const std::string &content)
{
    std::filesystem::path destination_path(path);
    std::error_code error;
    for(unsigned int hops = 0; ; ++hops)
    {
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
        destination_path = target.is_absolute() ? target : destination_path.parent_path() / target;
    }
    const auto output_path = destination_path.string();
    static std::atomic<unsigned long long> sequence {0};
    const auto temporary = output_path + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(++sequence);
#ifdef _WIN32
    const bool complete = fileWrite(temporary, content, true) == 0;
#else
    struct stat destination;
    const bool existing = stat(output_path.c_str(), &destination) == 0;
    if(!existing && errno != ENOENT) return -1;
    const auto mode = existing ? destination.st_mode & 0777 : 0666;
    const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
    if(descriptor < 0) return -1;
    std::FILE *file = nullptr;
    if(!existing || fchmod(descriptor, mode) == 0)
        file = fdopen(descriptor, "wb");
    bool complete = false;
    if(file)
    {
        const bool written = std::fwrite(content.data(), 1, content.size(), file) == content.size();
        complete = std::fclose(file) == 0 && written;
    }
    else
        close(descriptor);
#endif
    if(!complete)
    {
        std::remove(temporary.c_str());
        return -1;
    }
    // Replace only after the complete temporary file has been closed successfully.
#ifdef _WIN32
    const bool existing = GetFileAttributesA(output_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool replaced = existing
        ? ReplaceFileA(output_path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)
        : MoveFileExA(temporary.c_str(), output_path.c_str(), MOVEFILE_WRITE_THROUGH);
#else
    const bool replaced = std::rename(temporary.c_str(), output_path.c_str()) == 0;
#endif
    if(!replaced)
    {
        std::remove(temporary.c_str());
        return -1;
    }
    return 0;
}
