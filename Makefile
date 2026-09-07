# Top-level: builds every replacement component. Each is also independently buildable from its
# own directory (`cd components/camera && make`) -- see components/*/Makefile and
# build/mk/container.mk for how the container indirection works.
COMPONENTS := camera controllers tracking kernel

.PHONY: all clean base-images $(COMPONENTS) $(addsuffix -clean,$(COMPONENTS))

all: $(COMPONENTS)

$(COMPONENTS):
	$(MAKE) -C components/$@

clean: $(addsuffix -clean,$(COMPONENTS))

$(addsuffix -clean,$(COMPONENTS)):
	$(MAKE) -C components/$(@:%-clean=%) clean

base-images:
	$(MAKE) -C build/containers
