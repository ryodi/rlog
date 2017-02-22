all: rlog
static: rlog-static
clean:
	rm -fr rlog rlog-static *.o

rlog: rlog.o

rlog-static: rlog.o
	$(CC) -static $< -o $@

.PHONY: all static clean
