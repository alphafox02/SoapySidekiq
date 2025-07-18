# Soapy SDR module for Epiq Solutions Sidekiq

# need to set the LD_LIBRARY_PATH to point to the epiq solutions library
$ export LD_LIBRARY_PATH="/usr/local/lib:/usr/lib/epiq:/usr/lib/epiq:$LD_LIBRARY_PATH"

# Building using the install.sh script
$ ./install.sh

or if a platform 

$ ./install.sh PLATFORM="msiq-g20g40"  or "msiq-x40", "msiq-z3u"

# More detailed make and install info:

# Building 
Use the normal build process for cmake projects:

$ mkdir build

$ cd build

$ cmake ../ -DPLATFORM="<platform_name>"

$ make -j8

$ sudo make install

$ sudo ldconfig 

# Building for the X40:
**SoapySDR** must be installed from source if using it with the X40.

## Building SoapySDR
The x40 has two different versions of python, python3.8 is default and python3.9.

So when building the **SoapySDR** repo we need to explicitly call out the paths to the python3.8 libraries.

So we need to run:

```
$ cmake .. -DPython3_EXECUTABLE=/usr/bin/python3.8 -DPython3_INCLUDE_DIR=/usr/include/python3.8 -DPython3_LIBRARY=/usr/lib/aarch64-linux-gnu/libpython3.8.so

We also need to make sure PYTHONPATH is set correctly:

```

```
$ export PYTHONPATH="/usr/local/lib/python3.8/site-packages$PYTHONPATH"
```



# Licensing information
* https://github.com/pothosware/SoapySidekiq/blob/master/LICENSE
