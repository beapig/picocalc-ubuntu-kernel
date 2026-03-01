
# local kernel build dir
# KERN_DIR ?= /lib/modules/$(shell uname -r)/build

# users kernel dir
ARCH := arm

CROSS_COMPILE := /home/lyra/Lyra-sdk/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
KERN_DIR := /home/lyra/Lyra-sdk/kernel-6.1

MODULE_NAME = picocalc_snd_pwm

all:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERN_DIR) M=`pwd` modules
	$(CROSS_COMPILE)strip --strip-debug picocalc_snd_pwm.ko
clean:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERN_DIR) M=`pwd` modules clean

clena: clean

#CFLAGS_$(MODULE_NAME).o := -DDEBUG
obj-m += picocalc_snd_pwm.o
