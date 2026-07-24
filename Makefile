CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now

tabli: tabli.c
	$(CC) $(CFLAGS) tabli.c -o tabli $(LDFLAGS)
	strip tabli

test: tabli
	./test.sh

clean:
	rm -f tabli

.PHONY: test clean
