-include ../../mconfig

HOSTCXX ?= $(CXX)
HOSTCXXOPTS ?= $(CXXOPTS)
HOSTLDFLAGS ?= $(LDFLAGS)

mconfig-gen: mconfig-gen.cc
	$(HOSTCXX) $(HOSTCXXOPTS) -o mconfig-gen mconfig-gen.cc $(HOSTLDFLAGS)

clean:
	rm -f mconfig-gen
