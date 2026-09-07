# container.mk — generic "re-run this Makefile inside the toolchain container" wrapper.
#
# Included by every components/*/Makefile after it sets CONTAINER_IMAGE. On the host
# (IN_CONTAINER=0, the default), every target is redirected into `podman run` against that image,
# mounting the whole repo at the SAME absolute path so relative paths inside each Makefile (e.g.
# ../../work/oculus-kernel) resolve identically in and out of the container. Real recipes live in
# the including Makefile, guarded by `ifeq ($(IN_CONTAINER),1)`.
CONTAINER    ?= podman
IN_CONTAINER ?= 0
REPO_ROOT    := $(shell git rev-parse --show-toplevel)

ifeq ($(IN_CONTAINER),0)
.DEFAULT_GOAL := all
%:
	$(CONTAINER) run --rm -v $(REPO_ROOT):$(REPO_ROOT):Z -w $(CURDIR) $(CONTAINER_IMAGE) \
	  $(MAKE) IN_CONTAINER=1 $@
endif
