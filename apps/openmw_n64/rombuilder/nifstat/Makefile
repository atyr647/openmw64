# nifstat: a host (PC) build of OpenMW's NIF and BSA readers, for the census.
#   make                      # needs g++ with C++20, and the OSG headers
OPENMW_ROOT := ../../../..
OSG_SRC     ?= /opt/src/osg
C := $(OPENMW_ROOT)/components

SRCS := nifstat.cpp \
	$(C)/bsa/bsafile.cpp \
	$(C)/nif/niffile.cpp $(C)/nif/nifstream.cpp $(C)/nif/base.cpp $(C)/nif/controller.cpp \
	$(C)/nif/data.cpp $(C)/nif/effect.cpp $(C)/nif/extra.cpp $(C)/nif/node.cpp \
	$(C)/nif/particle.cpp $(C)/nif/physics.cpp $(C)/nif/property.cpp $(C)/nif/texture.cpp \
	$(C)/toutf8/toutf8.cpp \
	$(C)/files/constrainedfilestreambuf.cpp $(C)/files/conversion.cpp $(C)/files/openfile.cpp \
	$(C)/files/hash.cpp $(C)/platform/filestdio.cpp $(C)/debug/debuglog.cpp \
	$(OPENMW_ROOT)/extern/smhasher/MurmurHash3.cpp \
	$(OSG_SRC)/src/osg/Quat.cpp $(OSG_SRC)/src/osg/Matrixf.cpp $(OSG_SRC)/src/osg/Matrixd.cpp

CXXFLAGS := -std=c++20 -O2 -w -DOPENMW_N64 -I$(OPENMW_ROOT) -I$(C) -I$(OSG_SRC)/include -Ihost \
	-I$(OPENMW_ROOT)/extern -I$(OPENMW_ROOT)/extern/smhasher/include -I$(OPENMW_ROOT)/extern/smhasher/include/smhasher

nifstat: $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f nifstat
.PHONY: clean
