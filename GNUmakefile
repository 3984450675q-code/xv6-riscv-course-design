include Makefile
include conf/lab.mk

ifneq ($(V),@)
GRADEFLAGS += -v
endif

.PHONY: grade
grade:
	@echo $(MAKE) clean
	@$(MAKE) clean || \
	  (echo "'make clean' failed. HINT: Is another xv6 instance running?" && exit 1)
	./grade-lab-$(LAB) $(GRADEFLAGS)
