all: module eval

module:
	make -C module/

eval:
	make -C eval/

clean:
	make -C module/ clean
	make -C eval/ clean

.PHONY: module eval clean
