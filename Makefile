IMAGE := sendspin-soundtouch-build

# These devices only speak legacy SSH algorithms.
SSHOPTS := -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa
SSH     := ssh $(SSHOPTS)
SCP     := scp -O $(SSHOPTS)

.PHONY: build clean image all deploy install uninstall

image:
	docker build -t $(IMAGE) .

build: image
	docker run --rm -v "$(PWD):/work" --platform linux/amd64 $(IMAGE) \
	    bash -c "cmake -B build \
	        -DCMAKE_TOOLCHAIN_FILE=/work/cmake/toolchain-cortex-a8.cmake \
	        -DCMAKE_BUILD_TYPE=Release -G Ninja && \
	        cmake --build build"

all: image build

# Temporary deploy to /tmp for testing. /tmp is a tmpfs and is wiped on reboot;
# use `make install` for a permanent installation.
deploy: all
ifndef DEVICE
	$(error DEVICE not set. Usage: make deploy DEVICE=<device-ip>)
endif
	$(SCP) build/soundspin root@$(DEVICE):/tmp/
	@echo "Deployed to $(DEVICE):/tmp/ (lost on reboot)"

# Permanent installation: binary and config on NAND, started at boot by the
# device's own supervisor (shepherdd) via the override in /mnt/nv/shepherd/.
# Replaces the stock Bose software entirely. Reverse with `make uninstall`.
install: all
ifndef DEVICE
	$(error DEVICE not set. Usage: make install DEVICE=<device-ip>)
endif
	@set -e ; \
	variant=$$($(SSH) root@$(DEVICE) 'cat /proc/variant') ; \
	case "$$variant" in rhino|spotty) ;; \
	    *) echo "Untested variant '$$variant'; review files/Shepherd-soundspin.xml first"; exit 1 ;; \
	esac ; \
	echo "Installing to $(DEVICE) (variant: $$variant)" ; \
	: "Copy to .new then rename: the running binary cannot be written in place" ; \
	$(SCP) build/soundspin root@$(DEVICE):/mnt/nv/soundspin.new ; \
	$(SCP) files/netwatch.sh root@$(DEVICE):/mnt/nv/netwatch.sh.new ; \
	$(SCP) files/fallback-shepherd.xml root@$(DEVICE):/mnt/nv/good-shepherd.xml ; \
	$(SSH) root@$(DEVICE) 'chmod +x /mnt/nv/soundspin.new /mnt/nv/netwatch.sh.new && \
	    mv /mnt/nv/soundspin.new /mnt/nv/soundspin && \
	    mv /mnt/nv/netwatch.sh.new /mnt/nv/netwatch.sh' ; \
	if $(SSH) root@$(DEVICE) 'test -f /mnt/nv/soundspin.conf' ; then \
	    echo "  keeping existing /mnt/nv/soundspin.conf" ; \
	else \
	    echo "  seeding /mnt/nv/soundspin.conf -- EDIT server_url before rebooting" ; \
	    $(SCP) soundspin.conf root@$(DEVICE):/mnt/nv/soundspin.conf ; \
	fi ; \
	$(SSH) root@$(DEVICE) 'mkdir -p /mnt/nv/shepherd && rm -f /mnt/nv/shepherd/Shepherd-*.xml' ; \
	$(SCP) files/Shepherd-soundspin.xml root@$(DEVICE):/mnt/nv/shepherd/ ; \
	$(SSH) root@$(DEVICE) 'sync' ; \
	echo "Installed. Reboot to start it: $(SSH) root@$(DEVICE) reboot"

# Revert to the stock Bose software (removes only the override; the binary and
# config are left on NAND and are harmless without it).
uninstall:
ifndef DEVICE
	$(error DEVICE not set. Usage: make uninstall DEVICE=<device-ip>)
endif
	$(SSH) root@$(DEVICE) 'rm -rf /mnt/nv/shepherd && sync'
	@echo "Override removed. Reboot to return to stock: $(SSH) root@$(DEVICE) reboot"

clean:
	rm -rf build
