#include "SoapySidekiq.hpp"

std::vector<std::string> SoapySidekiq::listSensors(void) const
{
    std::vector<std::string> sensors;
    SoapySDR_logf(SOAPY_SDR_TRACE, "listSensors");

    sensors.push_back("temperature");
    sensors.push_back("accelerometer");

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

    SoapySDR_log(SOAPY_SDR_DEBUG, "sensor didn't match");
    return SoapySDR::Device::readSensor(key);
}

