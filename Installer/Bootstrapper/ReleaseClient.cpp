#include "ReleaseClient.h"

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace {
using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

std::wstring wide(const std::string& text) {
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        throw std::runtime_error("Invalid UTF-8 from GitHub");
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

std::string field(const std::string& object, std::string_view key) {
    const auto marker = '"' + std::string(key) + '"';
    auto pos = object.find(marker);
    if (pos == std::string::npos)
        throw std::runtime_error("GitHub release metadata is missing a field");
    pos = object.find(':', pos + marker.size());
    if (pos == std::string::npos)
        throw std::runtime_error("Invalid GitHub release metadata");
    pos = object.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || object[pos] != '"')
        throw std::runtime_error("Invalid GitHub release metadata");
    std::string value;
    for (++pos; pos < object.size(); ++pos) {
        const char c = object[pos];
        if (c == '"')
            return value;
        if (c == '\\') {
            if (++pos == object.size())
                break;
            const char escaped = object[pos];
            if (escaped != '/' && escaped != '\\' && escaped != '"')
                throw std::runtime_error("Unsupported escape in GitHub metadata");
            value += escaped;
        } else
            value += c;
    }
    throw std::runtime_error("Invalid GitHub release metadata");
}

std::uint64_t numberField(const std::string& object, std::string_view key) {
    const auto marker = '"' + std::string(key) + '"';
    auto pos = object.find(marker);
    if (pos == std::string::npos)
        throw std::runtime_error("GitHub release metadata is missing a size");
    pos = object.find(':', pos + marker.size());
    pos = object.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || !std::isdigit(static_cast<unsigned char>(object[pos])))
        throw std::runtime_error("Invalid release asset size");
    return std::stoull(object.substr(pos));
}

std::vector<std::string> assetObjects(const std::string& json) {
    auto pos = json.find("\"assets\"");
    if (pos == std::string::npos)
        throw std::runtime_error("Release has no assets");
    pos = json.find('[', pos);
    if (pos == std::string::npos)
        throw std::runtime_error("Invalid release assets");
    std::vector<std::string> objects;
    int depth = 0;
    bool quoted = false, escaped = false;
    std::size_t start = 0;
    for (++pos; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            continue;
        }
        if (c == '{') {
            if (depth++ == 0)
                start = pos;
        } else if (c == '}' && --depth == 0)
            objects.push_back(json.substr(start, pos - start + 1));
        else if (c == ']' && depth == 0)
            return objects;
    }
    throw std::runtime_error("Invalid release assets");
}

HttpHandle openRequest(const std::wstring& url, HttpHandle& session, HttpHandle& connection) {
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("Release URL must use HTTPS");
    session.reset(WinHttpOpen(L"GamEngineSetup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        throw std::runtime_error("Cannot initialize HTTPS");
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 30000);
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    connection.reset(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection)
        throw std::runtime_error("Cannot connect to GitHub");
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                       WinHttpCloseHandle);
    if (!request)
        throw std::runtime_error("Cannot create HTTPS request");
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        throw std::runtime_error("GitHub download failed");
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        throw std::runtime_error("GitHub returned HTTP " + std::to_string(status));
    return request;
}

std::wstring quote(const std::wstring& value) {
    return L'"' + value + L'"';
}

std::string runTar(const std::wstring& arguments, const std::filesystem::path& output) {
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH))
        throw std::runtime_error("Cannot find Windows tools");
    const auto executable = std::filesystem::path(systemDirectory) / L"tar.exe";
    if (!std::filesystem::exists(executable))
        throw std::runtime_error("Windows tar.exe is unavailable");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create archive listing");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = file;
    startup.hStdError = file;
    startup.hStdInput = nullptr;
    std::wstring command = quote(executable.wstring()) + L" " + arguments;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(file);
    if (!started)
        throw std::runtime_error("Cannot start Windows archive tool");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 0;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::ifstream stream(output, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (result != 0)
        throw std::runtime_error("Archive extraction failed: " + text.substr(0, 300));
    return text;
}
} // namespace

ReleaseAsset latestEditorRelease() {
    HttpHandle session(nullptr, WinHttpCloseHandle), connection(nullptr, WinHttpCloseHandle);
    auto request = openRequest(L"https://api.github.com/repos/B4rtekk1/GamEngine/releases/latest", session, connection);
    std::string json;
    std::array<char, 16384> buffer{};
    DWORD read = 0;
    while (WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read) && read) {
        json.append(buffer.data(), read);
        if (json.size() > 2 * 1024 * 1024)
            throw std::runtime_error("GitHub release metadata is too large");
    }
    const auto tag = field(json, "tag_name");
    static const std::regex versionPattern(R"(^v?([0-9]+\.[0-9]+\.[0-9]+)(-[A-Za-z0-9.-]+)?$)");
    if (!std::regex_match(tag, versionPattern))
        throw std::runtime_error("Latest release has an invalid version tag");
    for (const auto& object : assetObjects(json)) {
        if (field(object, "name") != "GamEngineEditor.zip")
            continue;
        const auto digest = field(object, "digest");
        if (!std::regex_match(digest, std::regex(R"(^sha256:[A-Fa-f0-9]{64}$)")))
            throw std::runtime_error("Release asset has no SHA-256 digest");
        const auto url = field(object, "browser_download_url");
        const std::string expected =
            "https://github.com/B4rtekk1/GamEngine/releases/download/" + tag + "/GamEngineEditor.zip";
        if (url != expected)
            throw std::runtime_error("Unexpected release asset URL");
        return {tag[0] == 'v' ? tag.substr(1) : tag, wide(url), digest.substr(7), numberField(object, "size")};
    }
    throw std::runtime_error("Latest release has no GamEngineEditor.zip asset");
}

void downloadEditor(const ReleaseAsset& asset, const std::filesystem::path& destination,
                    const std::function<void(int)>& progress) {
    HttpHandle session(nullptr, WinHttpCloseHandle), connection(nullptr, WinHttpCloseHandle);
    auto request = openRequest(asset.url, session, connection);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("Cannot initialize SHA-256");
    DWORD objectLength = 0, resultLength = 0;
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                      &resultLength, 0);
    std::vector<UCHAR> hashObject(objectLength);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Cannot initialize SHA-256");
    }
    try {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Cannot create download file");
        std::vector<char> buffer(1024 * 1024);
        DWORD read = 0;
        std::uint64_t received = 0;
        while (WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read) && read) {
            output.write(buffer.data(), read);
            if (!output)
                throw std::runtime_error("Cannot write downloaded file");
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0);
            received += read;
            if (received > asset.size)
                throw std::runtime_error("Downloaded file exceeds release asset size");
            progress(asset.size ? static_cast<int>(received * 70 / asset.size) : 0);
        }
        output.close();
        if (received != asset.size)
            throw std::runtime_error("Download is incomplete");
        std::array<UCHAR, 32> digest{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
            throw std::runtime_error("Cannot finish SHA-256 verification");
        char hex[65]{};
        for (std::size_t i = 0; i < digest.size(); ++i)
            std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        if (asset.sha256 != hex)
            throw std::runtime_error("Downloaded package failed SHA-256 verification");
    } catch (...) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
}

void validateArchive(const std::filesystem::path& archive) {
    const auto listing = archive.wstring() + L".list";
    std::string lines;
    try {
        lines = runTar(L"-tf " + quote(archive.wstring()), listing);
    } catch (...) {
        std::filesystem::remove(listing);
        throw;
    }
    std::filesystem::remove(listing);
    std::size_t count = 0;
    for (std::size_t start = 0; start < lines.size();) {
        auto end = lines.find('\n', start);
        if (end == std::string::npos)
            end = lines.size();
        std::string name = lines.substr(start, end - start);
        if (!name.empty() && name.back() == '\r')
            name.pop_back();
        if (!name.empty()) {
            if (name[0] == '/' || name.find(':') != std::string::npos || name.find('\\') != std::string::npos)
                throw std::runtime_error("Archive contains an unsafe path");
            std::size_t partStart = 0;
            for (std::size_t i = 0; i <= name.size(); ++i) {
                if (i == name.size() || name[i] == '/') {
                    if (name.substr(partStart, i - partStart) == "..")
                        throw std::runtime_error("Archive contains a parent path");
                    partStart = i + 1;
                }
            }
            ++count;
        }
        start = end + 1;
    }
    if (!count)
        throw std::runtime_error("Release archive is empty");
}

void extractArchive(const std::filesystem::path& archive, const std::filesystem::path& destination) {
    const auto listing = archive.wstring() + L".extract.log";
    try {
        runTar(L"-xf " + quote(archive.wstring()) + L" -C " + quote(destination.wstring()), listing);
    } catch (...) {
        std::filesystem::remove(listing);
        throw;
    }
    std::filesystem::remove(listing);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(destination))
        if (entry.is_symlink())
            throw std::runtime_error("Release archive contains a symbolic link");
}
