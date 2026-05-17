// nvml_wrapper.hpp - NVIDIA GPU temperature and telemetry monitoring
// v1.5: thermal protection for StarMiner

#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

namespace collider {
namespace platform {

struct GpuTelemetry {
    int index;
    std::string name;
    int temperature_c;      // Current temperature in Celsius
    int power_mw;           // Current power draw in milliwatts
    int utilization_pct;    // GPU utilization percentage
    int fan_speed_pct;      // Fan speed percentage (if available)
};

class NvmlWrapper {
public:
    NvmlWrapper();
    ~NvmlWrapper();

    // Initialize NVML. Returns false if NVML is not available (non-NVIDIA system).
    bool init();

    // Check if NVML is initialized
    bool is_initialized() const { return initialized_; }

    // Get telemetry for all GPUs
    std::vector<GpuTelemetry> get_all_telemetry();

    // Get telemetry for specific GPU indices
    std::vector<GpuTelemetry> get_telemetry(const std::vector<int>& gpu_indices);

    // Convenience: get just temperatures
    std::map<int, int> get_temperatures();

    // Thermal protection check.
    //   warn_temp_c:   log warning if exceeded (default 75)
    //   critical_temp_c: return true (caller should stop) if exceeded (default 85)
    //   on_warn:       optional callback when warn threshold exceeded
    // Returns true if critical temperature exceeded.
    bool check_thermal_protection(
        int warn_temp_c = 75,
        int critical_temp_c = 85,
        std::function<void(const GpuTelemetry&)> on_warn = nullptr);

private:
    bool initialized_ = false;
    void* nvml_handle_ = nullptr;  // dlopen handle for dynamic loading

    // Function pointers for dynamic loading
    typedef int (*nvmlInit_t)(void);
    typedef int (*nvmlShutdown_t)(void);
    typedef int (*nvmlDeviceGetCount_t)(unsigned int*);
    typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int, void**);
    typedef int (*nvmlDeviceGetName_t)(void*, char*, unsigned int);
    typedef int (*nvmlDeviceGetTemperature_t)(void*, unsigned int, unsigned int*);
    typedef int (*nvmlDeviceGetPowerUsage_t)(void*, unsigned int*);
    typedef int (*nvmlDeviceGetUtilizationRates_t)(void*, void*);
    typedef int (*nvmlDeviceGetFanSpeed_t)(void*, unsigned int*);

    nvmlInit_t fn_init_ = nullptr;
    nvmlShutdown_t fn_shutdown_ = nullptr;
    nvmlDeviceGetCount_t fn_get_count_ = nullptr;
    nvmlDeviceGetHandleByIndex_t fn_get_handle_ = nullptr;
    nvmlDeviceGetName_t fn_get_name_ = nullptr;
    nvmlDeviceGetTemperature_t fn_get_temp_ = nullptr;
    nvmlDeviceGetPowerUsage_t fn_get_power_ = nullptr;
    nvmlDeviceGetUtilizationRates_t fn_get_util_ = nullptr;
    nvmlDeviceGetFanSpeed_t fn_get_fan_ = nullptr;
};

} // namespace platform
} // namespace collider
