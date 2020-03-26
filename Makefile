all: clean
	$(CROSS_COMPILE)gcc -Wall -static -o droid4-agps droid4-agps.c

clean:
	rm -f droid4-agps *~
