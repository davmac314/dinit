-include ../../mconfig

CXX_FOR_BUILD ?= $(CXX)
CXXFLAGS_FOR_BUILD ?= $(CXXFLAGS) $(CXXFLAGS_EXTRA)
CPPFLAGS_FOR_BUILD ?= $(CPPFLAGS)
LDFLAGS_FOR_BUILD ?= $(LDFLAGS) $(LDFLAGS_EXTRA)

mconfig-gen: mconfig-gen.cc
	$(CXX_FOR_BUILD) $(CPPFLAGS_FOR_BUILD) $(CXXFLAGS_FOR_BUILD) -o mconfig-gen mconfig-gen.cc $(LDFLAGS_FOR_BUILD)

clean:
	rm -f mconfig-gen
