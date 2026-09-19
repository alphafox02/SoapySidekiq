// Unit tests for the SidekiqUtils helpers.  No Sidekiq hardware is needed.

#include "SidekiqUtils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sidekiq;

namespace
{
int failures = 0;
int checks = 0;

void check(const bool condition, const std::string &what)
{
    checks++;
    if (!condition)
    {
        failures++;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

bool throws(const std::function<void()> &fn)
{
    try
    {
        fn();
    }
    catch (const std::exception &)
    {
        return true;
    }
    return false;
}

bool contains(const std::vector<double> &values, const double value)
{
    return std::any_of(values.begin(), values.end(),
                       [value](const double v) { return std::abs(v - value) < 0.5; });
}

void testStrings()
{
    check(equalsIgnoreCase("RF", "rf"), "equalsIgnoreCase ignores case");
    check(!equalsIgnoreCase("RF", "RFX"), "equalsIgnoreCase compares length");
    check(!equalsIgnoreCase("", "a"), "equalsIgnoreCase empty vs non-empty");
    check(isRxGainName("LNA") && isRxGainName("gain"), "RX gain element names");
    check(isTxAttenuationName("attenuation") && isTxAttenuationName("ATTN"),
          "TX attenuation element names");
    check(std::string(rxHandleName(skiq_rx_hdl_B2)) == "B2", "RX handle names");
    check(std::string(txHandleName(skiq_tx_hdl_A2)) == "A2", "TX handle names");
}

void testHzValidation()
{
    check(hzToUint32("rate", 20e6) == 20000000u, "hzToUint32 converts");
    check(throws([] { hzToUint32("rate", 0); }), "hzToUint32 rejects 0");
    check(throws([] { hzToUint32("rate", -1); }), "hzToUint32 rejects negative");
    check(throws([] { hzToUint32("rate", NAN); }), "hzToUint32 rejects NaN");
    check(throws([] { hzToUint32("rate", 5e9); }), "hzToUint32 rejects overflow");
    check(hzToUint64("freq", 5.8e9) == 5800000000ull, "hzToUint64 above 4 GHz");
    const SoapySDR::Range range(70e6, 6e9);
    check(!throws([&] { validateRangeValue("freq", 915e6, range); }), "value inside range");
    check(throws([&] { validateRangeValue("freq", 50e6, range); }), "value below range");
}

// skiq_param_t's rx_param[]/tx_param[] are indexed by channel; the lookup must
// go through rf_param's handle list, which a topology can reorder.
void testParamIndexing()
{
    skiq_param_t param{};
    param.rf_param.num_rx_channels = 2;
    param.rf_param.rx_handles[0] = skiq_rx_hdl_B1;
    param.rf_param.rx_handles[1] = skiq_rx_hdl_A1;
    param.rx_param[0].sample_rate_max = 111;
    param.rx_param[1].sample_rate_max = 222;
    check(rxParamIndexForHandle(param, skiq_rx_hdl_B1) == 0, "remapped B1 is channel 0");
    check(rxParamForHandle(param, skiq_rx_hdl_A1).sample_rate_max == 222,
          "A1 parameters come from channel 1");

    skiq_param_t identity{};
    check(rxParamIndexForHandle(identity, skiq_rx_hdl_A2) == 1,
          "no channel list falls back to the handle number");
    check(throws([&] { rxParamIndexForHandle(identity, skiq_rx_hdl_end); }),
          "invalid handle rejected");
}

void testRxGain()
{
    const RxGainIndexRange range{187, 255};   // NV100: 0-34 dB in 0.5 dB steps
    check(rxGainStepDb(skiq_nv100, skiq_rx_hdl_A1) == 0.5, "NV100 gain step 0.5 dB");
    check(rxGainStepDb(skiq_m2_2280, skiq_rx_hdl_A1) == 1.0, "Stretch gain step 1 dB");
    check(rxGainStepDb(skiq_x2, skiq_rx_hdl_B1) == 1.0, "X2 B1 gain step 1 dB");
    check(rxGainIndexFromDb(skiq_nv100, skiq_rx_hdl_A1, range, 10.0) == 207, "10 dB -> index 207");
    check(rxGainIndexFromDb(skiq_nv100, skiq_rx_hdl_A1, range, 99.0) == 255, "gain clamps high");
    check(rxGainIndexFromDb(skiq_nv100, skiq_rx_hdl_A1, range, -5.0) == 187, "gain clamps low");
    for (double db = 0; db <= 34; db += 0.5)
    {
        const uint8_t index = rxGainIndexFromDb(skiq_nv100, skiq_rx_hdl_A1, range, db);
        if (rxGainDbFromIndex(skiq_nv100, skiq_rx_hdl_A1, range, index) != db)
        {
            check(false, "RX gain round trip at " + std::to_string(db) + " dB");
        }
    }
    const SoapySDR::Range db_range = rxGainRangeDb(skiq_nv100, skiq_rx_hdl_A1, range);
    check(db_range.minimum() == 0 && db_range.maximum() == 34 && db_range.step() == 0.5,
          "NV100 gain range 0-34 dB step 0.5");
}

void testTxAttenuation()
{
    skiq_param_t param{};
    param.rf_param.num_tx_channels = 1;
    param.rf_param.tx_handles[0] = skiq_tx_hdl_A1;
    param.tx_param[0].atten_quarter_db_min = 0;
    param.tx_param[0].atten_quarter_db_max = 359;          // 89.75 dB
    const SoapySDR::Range atten = txAttenuationRangeDb(param, skiq_tx_hdl_A1);
    check(atten.maximum() == 89.75 && atten.step() == 0.25, "TX attenuation range");
    check(txAttenuationIndexFromOutputGainDb(param, skiq_tx_hdl_A1, 89.75) == 0,
          "full output gain = no attenuation");
    check(txAttenuationIndexFromOutputGainDb(param, skiq_tx_hdl_A1, 0) == 359,
          "zero output gain = full attenuation");
    check(txAttenuationIndexFromAttenuationDb(param, skiq_tx_hdl_A1, 10.0) == 40,
          "10 dB attenuation = 40 quarter-dB steps");
    check(txAttenuationDbFromAttenuationIndex(param, skiq_tx_hdl_A1, 40) == 10.0,
          "attenuation index round trip");
    check(txOutputGainDbFromAttenuationIndex(param, skiq_tx_hdl_A1, 9999) == 0.0,
          "attenuation index clamps");
}

// Rates and bandwidths from the v4.26 SDK manual, NV100/NVM2/G20/G40/Z4 tables.
void testNv100Tables()
{
    const std::vector<double> rx = sampleRatesFromProfiles(rxProfilesForPart(skiq_nv100),
                                                           rxHandleMask(skiq_rx_hdl_A1));
    const std::vector<double> tx = sampleRatesFromProfiles(txProfilesForPart(skiq_nv100),
                                                           txHandleMask(skiq_tx_hdl_A1));
    for (const double rate : {250000.0, 541667.0, 1e6, 1.92e6, 10e6, 30.72e6, 61.44e6})
    {
        check(contains(rx, rate) && contains(tx, rate),
              "NV100 rate " + std::to_string(rate) + " listed for RX and TX");
    }
#if defined(LIBSIDEKIQ_VERSION) && (LIBSIDEKIQ_VERSION >= 42600)
    check(contains(rx, 625000) && contains(tx, 625000), "NV100 625 kHz (v4.26 manual)");
#endif
    if (libsidekiqAtLeast(4, 17, 0))
    {
        check(contains(rx, 1.4e6), "NV100 RX 1.4 MS/s disparate rate");
        check(!contains(tx, 1.4e6), "1.4 MS/s is RX only");
    }
    check(std::is_sorted(rx.begin(), rx.end()), "rate list sorted");
    check(partRequiresExactBuiltInSampleRate(skiq_nv100), "NV100 needs built-in rates");
    check(!partRequiresExactBuiltInSampleRate(skiq_m2_2280), "Stretch rates are continuous");
    check(throws([&] { validateBuiltInSampleRateIfRequired("rate", skiq_nv100, 12345678, rx); }),
          "off-table NV100 rate rejected");
    check(!throws([&] { validateBuiltInSampleRateIfRequired("rate", skiq_m2_2280, 12345678, rx); }),
          "Stretch accepts any rate");

    // 3%, 5-80% in 0.5% steps, then 86, 89, 95, 96, 99%
    const std::vector<double> bw = nv100BandwidthsForRate(10000000);
    check(bw.size() == 1 + 151 + 5, "NV100 bandwidth count (" + std::to_string(bw.size()) + ")");
    check(contains(bw, 300000) && contains(bw, 500000) && contains(bw, 8000000) &&
              contains(bw, 9900000),
          "NV100 bandwidth percentages");
    check(!contains(bw, 8100000) && !contains(bw, 10000000), "no 81% or 100% bandwidth");
}

void testX4Tables()
{
    const std::vector<double> c1 = sampleRatesFromProfiles(rxProfilesForPart(skiq_x4),
                                                           rxHandleMask(skiq_rx_hdl_C1));
    const std::vector<double> a1 = sampleRatesFromProfiles(rxProfilesForPart(skiq_x4),
                                                           rxHandleMask(skiq_rx_hdl_A1));
    check(contains(c1, 491.52e6) && !contains(a1, 491.52e6), "X4 491.52 MS/s only on C1/D1");
    const std::vector<double> bw = bandwidthsFromProfilesForRate(
        rxProfilesForPart(skiq_x4), rxHandleMask(skiq_rx_hdl_A1), 122880000);
    check(bw.size() == 4 && contains(bw, 100e6), "X4 122.88 MS/s bandwidth options");
}

void testBandwidthChecks()
{
    check(!bandwidthDiffersSignificantly(18000000, 18124800), "0.7% difference is rounding");
    check(bandwidthDiffersSignificantly(500000, 600000), "20% difference is significant");
    check(throws([] { validateBandwidthAgainstSampleRate("bw", 11000000, 10000000); }),
          "bandwidth above rate rejected");
    check(!throws([] { validateBandwidthAgainstSampleRate("bw", 8000000, 10000000); }),
          "bandwidth below rate accepted");
}

void testRfPorts()
{
    check(rfPortName(skiq_rf_port_J1) == "J1" && rfPortName(skiq_rf_port_unknown) == "NONE",
          "RF port names");
    const skiq_rf_port_t fixed[] = {skiq_rf_port_J2};
    const skiq_rf_port_t trx[] = {skiq_rf_port_J1};
    check(rfPortFromAntennaName("j2", fixed, 1, trx, 1) == skiq_rf_port_J2,
          "antenna name match is case-insensitive");
    check(rfPortFromAntennaName("TRX", fixed, 1, trx, 1) == skiq_rf_port_J1, "TRX alias");
    check(rfPortFromAntennaName("J7", fixed, 1, trx, 1) == skiq_rf_port_unknown,
          "unavailable port rejected");
}

void testSteppedValues()
{
    const std::vector<double> values =
        steppedValuesForRanges({SoapySDR::Range(1e6, 2e6)}, 250000);
    check(values.size() == 5 && values.front() == 1e6 && values.back() == 2e6,
          "stepped values include both ends");
}
}

int main()
{
    testStrings();
    testHzValidation();
    testParamIndexing();
    testRxGain();
    testTxAttenuation();
    testNv100Tables();
    testX4Tables();
    testBandwidthChecks();
    testRfPorts();
    testSteppedValues();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
