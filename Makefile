# Makefile for schwung-mk-va (Ableton Move Synth Module)

.PHONY: all build clean install

all: build

build:
	./scripts/build.sh

install:
	./scripts/install.sh

clean:
	rm -rf build dist
