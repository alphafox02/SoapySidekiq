#include "SoapySidekiq.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

std::string SoapySidekiq::gpsSysfsPath(const std::string &entry) const
{
    return "/sys/fs/skiq_gps/" + std::to_string(card) + "/" + entry;
}

bool SoapySidekiq::gpsSysfsAvailable(void) const
{
    struct stat info;
    return stat(gpsSysfsPath("ant_bias_en").c_str(), &info) == 0;
}

std::string SoapySidekiq::readGpsSysfs(const std::string &entry) const
{
    std::ifstream input(gpsSysfsPath(entry));
    std::string value;
    if (!input.is_open() || !std::getline(input, value))
    {
        throw std::runtime_error("cannot read " + gpsSysfsPath(entry) +
                                 " (is the sidekiq_gps kernel module loaded?)");
    }
    return value;
}

void SoapySidekiq::writeGpsSysfs(const std::string &entry, const std::string &value) const
{
    const std::string path = gpsSysfsPath(entry);
    std::ofstream output(path);
    if (output.is_open())
    {
        output << value;
        output.flush();
    }
    if (!output.is_open() || !output.good())
    {
        const int error = errno;
        std::string message = "cannot write " + path + ": " + std::strerror(error);
        if (error == EACCES || error == EPERM)
        {
            message += "; the sidekiq_gps sysfs entries are root-owned, so run as root "
                       "or grant write access (see the SoapySidekiq README)";
        }
        throw std::runtime_error(message);
    }
}

bool SoapySidekiq::gpsdoSupported(void) const
{
#if SOAPYSIDEKIQ_HAS_SDK_GPSDO
    skiq_gpsdo_support_t support = skiq_gpsdo_support_unknown;
    return skiq_is_gpsdo_supported(card, &support) == 0 &&
           support == skiq_gpsdo_support_is_supported;
#else
    return false;
#endif
}

bool SoapySidekiq::gpsdoEnabled(void) const
{
#if SOAPYSIDEKIQ_HAS_SDK_GPSDO
    bool enabled = false;
    return gpsdoSupported() && skiq_gpsdo_is_enabled(card, &enabled) == 0 && enabled;
#else
    return false;
#endif
}

std::vector<std::string> SoapySidekiq::listSensors(void) const
{
    std::vector<std::string> sensors;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listSensors");

    sensors.push_back("temperature");
    sensors.push_back("accelerometer");
    if (gpsSysfsAvailable())
    {
        sensors.push_back("gps_fix");
    }
    if (gpsdoSupported())
    {
#if SOAPYSIDEKIQ_HAS_SDK_GPSDO_LOCK
        sensors.push_back("gpsdo_locked");
#endif
        sensors.push_back("gpsdo_freq_accuracy");
    }

    return sensors;
}

SoapySDR::ArgInfo SoapySidekiq::getSensorInfo(const std::string &key) const
{
    SoapySDR::ArgInfo info;
    info.key = key;

    if (key == "temperature")
    {
        info.name = "Temperature";
        info.description = "Sidekiq board temperature";
        info.units = "C";
        info.type = SoapySDR::ArgInfo::INT;
    }
    else if (key == "accelerometer")
    {
        info.name = "Accelerometer";
        info.description = "Raw accelerometer axes as a JSON object {\"x\":..,\"y\":..,\"z\":..}";
        info.type = SoapySDR::ArgInfo::STRING;
    }
    else if (key == "gps_fix")
    {
        info.name = "GPS Fix";
        info.description = "On-board GPS receiver has a position fix";
        info.type = SoapySDR::ArgInfo::BOOL;
    }
    else if (key == "gpsdo_locked")
    {
        info.name = "GPSDO Locked";
        info.description = "Reference oscillator is locked to GPS (clock source \"gpsdo\")";
        info.type = SoapySDR::ArgInfo::BOOL;
    }
    else if (key == "gpsdo_freq_accuracy")
    {
        info.name = "GPSDO Accuracy";
        info.description = "Reference frequency accuracy while disciplined by GPS; "
                           "empty when the GPSDO is not enabled or not locked";
        info.units = "ppm";
        info.type = SoapySDR::ArgInfo::FLOAT;
    }
    else
    {
        return SoapySDR::Device::getSensorInfo(key);
    }

    return info;
}

std::string SoapySidekiq::readSensor(const std::string &key) const
{
    int status = 0;
    SoapySDR_logf(SOAPY_SDR_TRACE, "readSensor");

    if (key.compare("temperature") == 0)
    {
        int8_t temp = 0;

        status = skiq_read_temp(card, &temp);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "Failure: skiq_read_temp (card %i), status %d", card,
                          status);
            // no reading; do not report a made-up 0 C
            return "";
        }
        else
        {
            SoapySDR_logf(SOAPY_SDR_DEBUG, "Temp is %d", temp);
        }

        return std::to_string(temp);
    }
    bool supported = false;

    if (key.compare("accelerometer") == 0)
    {
        status = skiq_is_accel_supported(card, &supported);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq-is_accel_supported (card %u), status %d", card,
                status);
        }

        if (!supported)
        {
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "Accelerometer not supported by card %u, status %d",
                          card, status);
            return "{}";
        }

        /* enable accel for the card */
        status = skiq_write_accel_state(card, 1);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq_write_accel_state (card %i), status %d", card,
                status);
            return "{}";
        }

        int16_t x_data = 0;
        int16_t y_data = 0;
        int16_t z_data = 0;
        status         = skiq_read_accel(card, &x_data, &y_data, &z_data);
        if (status != 0)
        {
            SoapySDR_logf(SOAPY_SDR_ERROR,
                          "Failure: skiq_read_accel (card %i), status %d", card,
                          status);
            return "{}";
        }

        /* disable accel */
        status = skiq_write_accel_state(card, 0);
        if (status != 0)
        {
            SoapySDR_logf(
                SOAPY_SDR_ERROR,
                "Failure: skiq_write_accel_state (card %i), status %d", card,
                status);
            return "{}";
        };
        std::stringstream ss;
        ss << "{\"x\":" << x_data << ",\"y\":" << y_data << ",\"z\":" << z_data
           << "}";

        SoapySDR_logf(SOAPY_SDR_DEBUG, "accel data %s", (ss.str().c_str()));
        return ss.str();
    }

    if (key == "gps_fix")
    {
        return readGpsSysfs("has_fix") == "1" ? "true" : "false";
    }

#if SOAPYSIDEKIQ_HAS_SDK_GPSDO_LOCK
    if (key == "gpsdo_locked")
    {
        bool locked = false;
        status = skiq_gpsdo_is_locked(card, &locked);
        return (status == 0 && locked) ? "true" : "false";
    }
#endif

#if SOAPYSIDEKIQ_HAS_SDK_GPSDO
    if (key == "gpsdo_freq_accuracy")
    {
        double ppm = 0;
        status = skiq_gpsdo_read_freq_accuracy(card, &ppm);
        if (status != 0)
        {
            // not enabled or not locked yet: no measurement to report
            return "";
        }
        std::ostringstream stream;
        stream << ppm;
        return stream.str();
    }
#endif

    SoapySDR_log(SOAPY_SDR_DEBUG, "sensor didn't match");
    return SoapySDR::Device::readSensor(key);
}

