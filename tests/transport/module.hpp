#pragma once
#include <netcode/transport_provider.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct Module {
#ifdef _WIN32
    HMODULE handle;
    explicit Module(const std::filesystem::path &path) : handle(LoadLibraryW(path.c_str())) {
        if (!handle) throw std::runtime_error("LoadLibraryW failed: " + std::to_string(GetLastError()));
    }
    ~Module() { FreeLibrary(handle); }
    nc_transport_get_api_fn entry() const {
        return reinterpret_cast<nc_transport_get_api_fn>(GetProcAddress(handle, NC_TRANSPORT_ENTRY));
    }
#else
    void *handle;
    explicit Module(const std::filesystem::path &path) : handle(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        if (!handle) throw std::runtime_error(dlerror());
    }
    ~Module() { dlclose(handle); }
    nc_transport_get_api_fn entry() const {
        return reinterpret_cast<nc_transport_get_api_fn>(dlsym(handle, NC_TRANSPORT_ENTRY));
    }
#endif
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
};
