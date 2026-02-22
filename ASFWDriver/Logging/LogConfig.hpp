//
// LogConfig.hpp
// ASFWDriver
//
// Runtime logging configuration singleton
// Reads verbosity levels from Info.plist properties and supports runtime updates
//

#ifndef ASFW_LOGGING_LOGCONFIG_HPP
#define ASFW_LOGGING_LOGCONFIG_HPP

#include <stdint.h>
#include <atomic>

// Forward declarations (actual includes in .cpp to avoid .iig header issues)
class IOService;

namespace ASFW {

/**
 * @brief Centralized logging configuration manager
 *
 * Reads verbosity settings from Info.plist properties:
 * - ASFWAsyncVerbosity (integer 0-4): Controls Async subsystem logging detail
 * - ASFWControllerVerbosity (integer 0-4): Controls Controller logging (future)
 * - ASFWHardwareVerbosity (integer 0-4): Controls Hardware logging (future)
 * - ASFWEnableHexDumps (boolean): Force enable/disable packet dumps
 * - ASFWLogStatistics (boolean): Enable aggregate statistics logging
 * - ASFWEnableIsochTxVerifier (boolean): Enable dev-only IT TX verifier (expensive)
 *
 * Thread-safe singleton with runtime update support via user client.
 */
class LogConfig {
public:
    /**
     * @brief Get singleton instance
     */
    static LogConfig& Shared();

    /**
     * @brief Initialize from IOService properties
     * @param service The IOService instance (typically ASFWDriver)
     *
     * Reads Info.plist properties and sets initial configuration.
     * Must be called once during driver Start().
     */
    void Initialize(IOService* service);

    // ========================================================================
    // Getters (thread-safe, const)
    // ========================================================================

    /**
     * @brief Get Async subsystem verbosity level (0-4)
     */
    uint8_t GetAsyncVerbosity() const;

    /**
     * @brief Get Controller subsystem verbosity level (0-4)
     */
    uint8_t GetControllerVerbosity() const;

    /**
     * @brief Get Hardware subsystem verbosity level (0-4)
     */
    uint8_t GetHardwareVerbosity() const;

    /**
     * @brief Get Discovery subsystem verbosity level (0-4)
     */
    uint8_t GetDiscoveryVerbosity() const;
    uint8_t GetConfigROMVerbosity() const;

    /**
     * @brief Get UserClient subsystem verbosity level (0-4)
     */
    uint8_t GetUserClientVerbosity() const;

    /**
     * @brief Get MusicSubunit subsystem verbosity level (0-4)
     */
    uint8_t GetMusicSubunitVerbosity() const;

    /**
     * @brief Get FCP subsystem verbosity level (0-4)
     */
    uint8_t GetFCPVerbosity() const;

    /**
     * @brief Get CMP subsystem verbosity level (0-4)
     */
    uint8_t GetCMPVerbosity() const;

    /**
     * @brief Get IRM subsystem verbosity level (0-4)
     */
    uint8_t GetIRMVerbosity() const;

    /**
     * @brief Get AVC subsystem verbosity level (0-4)
     */
    uint8_t GetAVCVerbosity() const;
    uint8_t GetIsochVerbosity() const;

    /**
     * @brief Check if hex dumps are enabled
     */
    bool IsHexDumpsEnabled() const;

    /**
     * @brief Check if aggregate statistics logging is enabled
     */
    bool IsStatisticsEnabled() const;

    /**
     * @brief Check if dev-only IT TX verifier is enabled
     */
    bool IsIsochTxVerifierEnabled() const;

    // ========================================================================
    // Runtime Setters (thread-safe, for user client control)
    // ========================================================================

    /**
     * @brief Set Async verbosity at runtime
     * @param level New verbosity level (0-4, clamped if out of range)
     */
    void SetAsyncVerbosity(uint8_t level);

    /**
     * @brief Set Controller verbosity at runtime
     */
    void SetControllerVerbosity(uint8_t level);

    /**
     * @brief Set Hardware verbosity at runtime
     */
    void SetHardwareVerbosity(uint8_t level);

    /**
     * @brief Set Discovery verbosity at runtime
     */
    void SetDiscoveryVerbosity(uint8_t level);
    void SetConfigROMVerbosity(uint8_t level);

    /**
     * @brief Set UserClient verbosity at runtime
     */
    void SetUserClientVerbosity(uint8_t level);

    /**
     * @brief Set MusicSubunit verbosity at runtime
     */
    void SetMusicSubunitVerbosity(uint8_t level);

    /**
     * @brief Set FCP verbosity at runtime
     */
    void SetFCPVerbosity(uint8_t level);

    /**
     * @brief Set CMP verbosity at runtime
     */
    void SetCMPVerbosity(uint8_t level);

    /**
     * @brief Set IRM verbosity at runtime
     */
    void SetIRMVerbosity(uint8_t level);

    /**
     * @brief Set AVC verbosity at runtime
     */
    void SetAVCVerbosity(uint8_t level);
    void SetIsochVerbosity(uint8_t level);
    void SetHexDumps(bool enable);

    /**
     * @brief Enable or disable dev-only IT TX verifier at runtime
     */
    void SetIsochTxVerifierEnabled(bool enable);

    /**
     * @brief Enable or disable statistics logging at runtime
     */
    void SetStatistics(bool enable);

private:
    LogConfig();
    ~LogConfig();

    // Non-copyable
    LogConfig(const LogConfig&) = delete;
    LogConfig& operator=(const LogConfig&) = delete;

    /**
     * @brief Helper to read uint8 property from IOService
     */
    uint8_t ReadUInt8Property(IOService* service, const char* key, uint8_t defaultValue);

    /**
     * @brief Helper to read bool property from IOService
     */
    bool ReadBoolProperty(IOService* service, const char* key, bool defaultValue);

    /**
     * @brief Clamp verbosity level to valid range [0, 4]
     */
    static uint8_t ClampLevel(uint8_t level);

    // Configuration state (lock-free atomic for DriverKit compatibility)
    std::atomic<uint8_t> asyncVerbosity_;
    std::atomic<uint8_t> controllerVerbosity_;
    std::atomic<uint8_t> hardwareVerbosity_;
    std::atomic<uint8_t> discoveryVerbosity_;
    std::atomic<uint8_t> configROMVerbosity_;
    std::atomic<uint8_t> userClientVerbosity_;
    std::atomic<uint8_t> musicSubunitVerbosity_;
    std::atomic<uint8_t> fcpVerbosity_;
    std::atomic<uint8_t> cmpVerbosity_;
    std::atomic<uint8_t> irmVerbosity_;
    std::atomic<uint8_t> avcVerbosity_;
    std::atomic<uint8_t> isochVerbosity_;
    std::atomic<bool> enableHexDumps_;
    std::atomic<bool> isochTxVerifierEnabled_;
    std::atomic<bool> logStatistics_;
    std::atomic<bool> initialized_;
};

} // namespace ASFW

#endif // ASFW_LOGGING_LOGCONFIG_HPP
