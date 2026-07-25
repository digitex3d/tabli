CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now

tabbli: tabbli.c
	$(CC) $(CFLAGS) tabbli.c -o tabbli $(LDFLAGS)
	strip tabbli

test: tabbli
	./test.sh

# the skill ships a copy of the engine and its source: keep them in sync
sync-skill: tabbli
	cp tabbli.c skill/tabbli/src/tabbli.c
	cp tabbli skill/tabbli/bin/tabbli-x86_64
	cmp -s tabbli.c skill/tabbli/src/tabbli.c && echo "skill in sync"

clean:
	rm -f tabbli

.PHONY: test clean sync-skill
