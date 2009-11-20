CFLAGS	:= -m32 -g -std=c99 -fno-stack-protector -Wall -Werror -D_GNU_SOURCE=1
LDFLAGS	:= -m32
PROGS	:= httpd-exstack httpd-nxstack

all:	$(PROGS) shellcode.bin

httpd:	httpd.o
	gcc $< -o $@ $(LDFLAGS)

%-exstack: %
	cp $< $@
	execstack -s $@

%-nxstack: %
	cp $< $@
	execstack -c $@

%.o:	%.c
	gcc $< -c -o $@ $(CFLAGS)

%.o:	%.S
	gcc $< -c -o $@ $(CFLAGS)

%.bin:	%.o
	objcopy -S -O binary -j .text $< $@

clean:
	rm -f *.o *.pyc *.bin httpd $(PROGS)

.PRECIOUS: %.o
