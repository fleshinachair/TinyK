# Makefile for TinyK (Ableton Move Synth Module)

.PHONY: all build clean install

all: build

build:
	./scripts/build.sh

install:
	./scripts/install.sh

clean:
	rm -rf build dist TinyK.tar.gz
