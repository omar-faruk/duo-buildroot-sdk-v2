SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
     PARAM_FILE:=../../Makefile.param
     include $(PARAM_FILE)
endif

SDIR = $(PWD)/src
SRCS = $(wildcard $(SDIR)/*.c)

ifeq ($(CHIP_ARCH), $(filter $(CHIP_ARCH), CV180X PHOBOS))
	SRCS := $(filter-out $(SDIR)/cvi_vo.c, $(SRCS))
endif

INCS = -I$(MW_INC) -I$(ISP_INC) -I$(SYS_INC) -I$(BIN_INC)\
	   -I$(KERNEL_INC) -I./include -I$(VI_INC) -I$(VPSS_INC) -I$(GDC_INC) -I$(RGN_INC)

OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)
TARGET_A = $(MW_LIB)/libgdc.a
TARGET_SO = $(MW_LIB)/libgdc.so

EXTRA_CFLAGS = $(INCS)


.PHONY : clean all
all : $(TARGET_A) $(TARGET_SO)

$(SDIR)/gdc_mesh.o: $(SDIR)/gdc_mesh.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -O3 -c $< -o $@
	@echo [$(notdir $(CC))] $(notdir $@)

$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@
	@echo [$(notdir $(CC))] $(notdir $@)

$(TARGET_A): $(OBJS)
	@$(AR) $(ARFLAGS) $(TARGET_A) $(OBJS)
	@echo -e $(YELLOW)[LINK]$(END)[$(notdir $(AR))] $(notdir $(TARGET_A))

$(TARGET_SO): $(OBJS)
	@$(LD) $(LDFLAGS) -o $(TARGET_SO) --start-group $(OBJS) --end-group
	@echo -e $(GREEN)[LINK]$(END)[$(notdir $(LD))] $(notdir $(TARGET_SO))

clean:
	@rm -f $(OBJS) $(DEPS) $(TARGET_A) $(TARGET_SO)

-include $(DEPS)
