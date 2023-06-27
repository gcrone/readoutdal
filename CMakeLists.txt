cmake_minimum_required(VERSION 3.12)
project(readoutdal VERSION 0.0.0)

find_package(daq-cmake REQUIRED)

daq_setup_environment()

find_package(okssystem REQUIRED)
find_package(genconfig REQUIRED)
find_package(ers REQUIRED)
find_package(logging REQUIRED)
find_package(oksdbinterfaces REQUIRED)
find_package(coredal REQUIRED)


#find_package(Boost COMPONENTS unit_test_framework REQUIRED)


daq_oks_codegen(readout.schema.xml NAMESPACE dunedaq::readoutdal DEP_PKGS coredal)

daq_add_library(generate_modules.cpp
 LINK_LIBRARIES oksdbinterfaces::oksdbinterfaces okssystem::okssystem
  logging::logging coredal_oks ers::ers)


daq_add_python_bindings(*.cpp LINK_LIBRARIES 
  readoutdal readoutdal_oks coredal coredal_oks)


daq_add_application(gen_readout_modules gen_readout_modules.cxx TEST LINK_LIBRARIES
 readoutdal readoutdal_oks coredal coredal_oks
 oksdbinterfaces::oksdbinterfaces logging::logging)

##############################################################################


# See https://dune-daq-sw.readthedocs.io/en/latest/packages/daq-cmake/#daq_add_unit_test

#daq_add_unit_test(Placeholder_test LINK_LIBRARIES)  # Placeholder_test should be replaced with real unit tests

##############################################################################

daq_install()
