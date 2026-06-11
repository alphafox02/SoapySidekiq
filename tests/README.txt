The provided test applications do the following:

cs16_validate: Puts the FPGA in ramp mode.  Sets Soapy to use CS16 samples.
Does readStream on 10 blocks, testing the received rf_timestamps and validates 
the ramp on each block.  Then it does it again with sys_timestamps.

cf32_validate: Puts the FPGA in ramp mode.  Sets Soapy to use CF32 samples.
Does readStream on 10 blocks.  Then it validates the samples to see if they 
are what the FPGA ramp sends.

rx_to_file: Receives a defined number of blocks then writes them to a file.

tx_from_file: Transmits the blocks in a file, repeating until Ctrl-c.

txtone: Generates the samples for a tone and transmits it until Ctrl-c.

txtone_1pps: Generates the samples for a tone and transmits it on 1pps.  
Continues until Ctrl-c.

test_api: The runs through all of the API commands without streaming to validate they give the right values and pass in invalid values and verify errors.

record_cs16: This receives samples and writes them to a file continuously until Ctrl-c.  This uses threading to have one receive thread and one write to file thread.  This is limited in sample rate due to speed of writing to file.

rx_dual_cs16_validate: Python counter-mode RX validator for one card with a configurable channel list.

rx_multi_cs16_validate.cpp: C++ counter-mode RX validator for one card with a configurable channel list.  Use this when SoapySDR Python bindings are not installed.

rx_multicard_cs16_validate.cpp: C++ counter-mode RX validator that opens multiple Sidekiq cards in one process.  On a G40 with two NV100 cards, use `--cards 0,1 --channels 0,1` to validate four logical RX streams.

antenna_smoke.cpp: C++ RF-port smoke test that lists Soapy antennas, reads the current antenna, and can set each channel back to its current antenna to validate the Sidekiq RF-port API path without intentionally changing ports.  Pass --exercise-switch to temporarily switch each channel through all listed ports and restore the original port.

range_smoke.cpp: C++ API smoke test that prints sample-rate ranges, concrete listSampleRates/listBandwidths values, gain elements/ranges, frequency ranges, and native full-scale values per direction/channel.  Pass --include-tx to include TX, and pass --expect-nonlisted-rate-reject on NV100/NVM2-style profile-limited hardware to confirm in-range but non-profile rates are rejected.
