// nvml_wrapper.cpp - NVIDIA GPU temperature and telemetry monitoring
// Dynamically loads libnvidia-ml so StarMiner works on non-NVIDIA systems.

#include "nvml_wrapper.hpp"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
   using DlHandle = HMODULE;
   static DlHandle dl_open(const char* name)  { return LoadLibraryA(name); }
   static void*    dl_sym(DlHandle h, const char* s) { return reinterpret_cast<void*>(GetProcAddress(h, s)); }
   static void     dl_close(DlHandle h)        { FreeLibrary(h); }
#else
#  include <dlfcn.h>
   using DlHandle = void*;
   static DlHandle dl_open(const char* name)  { return dlopen(name, RTLD_LAZY); }
   static void*    dl_sym(DlHandle h, const char* s) { return dlsym(h, s); }
   static void     dl_close(DlHandle h)        { dlclose(h); }
#endif

namespace starminer {
namespace platform {

// NVML temperature sensor enum value (NVML_TEMPERATURE_GPU = 0)
static constexpr unsigned int NVML_TEMPERATURE_GPU = 0;

// NVML success code
static constexpr int NVML_SUCCESS = 0;

// Utilization rates struct (matches NVML's nvmlUtilization_t)
struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

NvmlWrapper::NvmlWrapper() = default;

NvmlWrapper::~NvmlWrapper() {
    if (initialized_ && fn_shutdown_) {
        fn_shutdown_();
    }
    if (nvml_handle_) {
        dl_close(static_cast<DlHandle>(nvml_handle_));
    }
}

bool NvmlWrapper::init() {
    if (initialized_) return true;

#ifdef _WIN32
    auto handle = dl_open("nvml.dll");
    if (!handle) handle = dl_open("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
#else
    auto handle = dl_open("libnvidia-ml.so.1");
#endif
    if (!handle) {
        return false;
    }
    nvml_handle_ = static_cast<void*>(handle);

    fn_init_ = reinterpret_cast<nvmlInit_t>(dl_sym(handle, "nvmlInit_v2"));
    if (!fn_init_) fn_init_ = reinterpret_cast<nvmlInit_t>(dl_sym(handle, "nvmlInit"));
    fn_shutdown_ = reinterpret_cast<nvmlShutdown_t>(dl_sym(handle, "nvmlShutdown"));
    fn_get_count_ = reinterpret_cast<nvmlDeviceGetCount_t>(dl_sym(handle, "nvmlDeviceGetCount"));
    fn_get_handle_ = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(dl_sym(handle, "nvmlDeviceGetHandleByIndex_v2"));
    if (!fn_get_handle_) fn_get_handle_ = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(dl_sym(handle, "nvmlDeviceGetHandleByIndex"));
    fn_get_name_ = reinterpret_cast<nvmlDeviceGetName_t>(dl_sym(handle, "nvmlDeviceGetName"));
    fn_get_temp_ = reinterpret_cast<nvmlDeviceGetTemperature_t>(dl_sym(handle, "nvmlDeviceGetTemperature"));
    fn_get_power_ = reinterpret_cast<nvmlDeviceGetPowerUsage_t>(dl_sym(handle, "nvmlDeviceGetPowerUsage"));
    fn_get_util_ = reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(dl_sym(handle, "nvmlDeviceGetUtilizationRates"));
    fn_get_fan_ = reinterpret_cast<nvmlDeviceGetFanSpeed_t>(dl_sym(handle, "nvmlDeviceGetFanSpeed"));

    if (!fn_init_ || !fn_shutdown_ || !fn_get_count_ || !fn_get_handle_ ||
        !fn_get_name_ || !fn_get_temp_) {
        dl_close(handle);
        nvml_handle_ = nullptr;
        return false;
    }

    if (fn_init_() != NVML_SUCCESS) {
        dl_close(handle);
        nvml_handle_ = nullptr;
        return false;
    }

    initialized_ = true;
    return true;
}

std::vector<GpuTelemetry> NvmlWrapper::get_all_telemetry() {
    std::vector<GpuTelemetry> result;
    if (!initialized_) return result;

    unsigned int count = 0;
    if (fn_get_count_(&count) != NVML_SUCCESS) return result;

    for (unsigned int i = 0; i < count; ++i) {
        void* device = nullptr;
        if (fn_get_handle_(i, &device) != NVML_SUCCESS) continue;

        GpuTelemetry gt;
        gt.index = static_cast<int>(i);

        char name[96] = {};
        if (fn_get_name_(device, name, sizeof(name)) == NVML_SUCCESS) {
            gt.name = name;
        } else {
            gt.name = "Unknown";
        }

        unsigned int temp = 0;
        if (fn_get_temp_(device, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
            gt.temperature_c = static_cast<int>(temp);
        } else {
            gt.temperature_c = -1;
        }

        unsigned int power = 0;
        if (fn_get_power_ && fn_get_power_(device, &power) == NVML_SUCCESS) {
            gt.power_mw = static_cast<int>(power);
        } else {
            gt.power_mw = -1;
        }

        NvmlUtilization util = {};
        if (fn_get_util_ && fn_get_util_(device, &util) == NVML_SUCCESS) {
            gt.utilization_pct = static_cast<int>(util.gpu);
        } else {
            gt.utilization_pct = -1;
        }

        unsigned int fan = 0;
        if (fn_get_fan_ && fn_get_fan_(device, &fan) == NVML_SUCCESS) {
            gt.fan_speed_pct = static_cast<int>(fan);
        } else {
            gt.fan_speed_pct = -1;
        }

        result.push_back(gt);
    }
    return result;
}

std::vector<GpuTelemetry> NvmlWrapper::get_telemetry(const std::vector<int>& gpu_indices) {
    auto all = get_all_telemetry();
    std::vector<GpuTelemetry> result;
    for (const auto& gt : all) {
        for (int idx : gpu_indices) {
            if (gt.index == idx) {
                result.push_back(gt);
                break;
            }
        }
    }
    return result;
}

std::map<int, int> NvmlWrapper::get_temperatures() {
    std::map<int, int> temps;
    auto all = get_all_telemetry();
    for (const auto& gt : all) {
        temps[gt.index] = gt.temperature_c;
    }
    return temps;
}

bool NvmlWrapper::check_thermal_protection(
    int warn_temp_c,
    int critical_temp_c,
    std::function<void(const GpuTelemetry&)> on_warn) {

    auto all = get_all_telemetry();
    bool critical = false;

    for (const auto& gt : all) {
        if (gt.temperature_c < 0) continue;

        if (gt.temperature_c >= critical_temp_c) {
            std::cerr << "[NVML] CRITICAL: GPU " << gt.index << " (" << gt.name
                      << ") temperature " << gt.temperature_c << "°C exceeds critical threshold "
                      << critical_temp_c << "°C. Shutting down to prevent damage." << std::endl;
            critical = true;
        } else if (gt.temperature_c >= warn_temp_c) {
            if (on_warn) {
                on_warn(gt);
            } else {
                std::cerr << "[NVML] WARNING: GPU " << gt.index << " (" << gt.name
                          << ") temperature " << gt.temperature_c << "°C exceeds warn threshold "
                          << warn_temp_c << "°C" << std::endl;
            }
        }
    }
    return critical;
}

} // namespace platform
} // namespace starminer
