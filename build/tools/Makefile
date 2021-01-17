HOSTCXX ?= $(CXX)
HOSTCXXFLAGS ?= $(CXXFLAGS)
HOSTLDFLAGS ?= $(LDFLAGS)

mconfig-gen: mconfig-gen.cc
	$(HOSTCXX) $(HOSTCXXOPTS) -o mconfig-gen mconfig-gen.cc $(HOSTLDFLAGS)
