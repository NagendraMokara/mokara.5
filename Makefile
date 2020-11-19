CFLAGS=-Wall -Werror -g
LDFLAGS=-lpthread

all: oss user_proc

oss: oss.c oss.h
	gcc $(CFLAGS) -o oss oss.c $(LDFLAGS)

user_proc: user_proc.c oss.h
	gcc $(CFLAGS) -o user_proc user_proc.c $(LDFLAGS)

clean:
	rm -f oss
	rm -f user_proc
