-include ../../mconfig

HOSTCXX ?= $(CXX)
HOSTCXXFLAGS ?= $(CXXFLAGS) $(CXXFLAGS_EXTRA)
HOSTCPPFLAGS ?= $(CPPFLAGS)
HOSTLDFLAGS ?= $(LDFLAGS) $(LDFLAGS_EXTRA)

mconfig-gen: mconfig-gen.cc
	$(HOSTCXX) $(HOSTCPPFLAGS) $(HOSTCXXFLAGS) -o mconfig-gen mconfig-gen.cc $(HOSTLDFLAGS)

clean:
	rm -f mconfig-gen
