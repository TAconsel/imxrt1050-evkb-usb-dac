# Host tools for the RT1050 room-correction DAC
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PKGS_GTK = gtk4
LIBUSB   = libusb-1.0

all: rcdac-cli rcdac-gtk

rcdac.o: rcdac.c rcdac.h ../usb_dac32/roomcorr_usbctl.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(LIBUSB)) -c $< -o $@

rcdac-cli: rcdac-cli.c rcdac.o
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(LIBUSB)) $^ -o $@ $(shell pkg-config --libs $(LIBUSB)) -lm

rcdac-gtk: rcdac-gtk.c rcdac.o
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKGS_GTK) $(LIBUSB)) $^ -o $@ \
	    $(shell pkg-config --libs $(PKGS_GTK) $(LIBUSB)) -lm

clean:
	rm -f rcdac-cli rcdac-gtk rcdac.o

.PHONY: all clean
