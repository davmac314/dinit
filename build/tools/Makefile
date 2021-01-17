HOSTCXX ?= $(CXX)
HOSTCXXFLAGS ?= $(CXXFLAGS)
HOSTLDFLAGS ?= $(LDFLAGS)

mconfig-gen: mconfig-gen.cc ../../mconfig
	$(HOSTCXX) $(HOSTCXXOPTS) -o mconfig-gen mconfig-gen.cc $(HOSTLDFLAGS)
