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
	: "The Firewall daemon exits on a missing interface, so watch only those present" ; \
	ifaces=$$($(SSH) root@$(DEVICE) 'for i in eth0 wlan0; do [ -e /sys/class/net/$$i ] && echo $$i; done') ; \
	[ -n "$$ifaces" ] || { echo "No eth0/wlan0 on device; refusing to install"; exit 1; } ; \
	fwargs="" ; flag="-4i" ; \
	for i in $$ifaces; do fwargs="$$fwargs<arg>$$flag</arg><arg>$$i</arg>"; flag="-i"; done ; \
	tmp=$$(mktemp -d) ; trap 'rm -rf "$$tmp"' EXIT ; \
	for f in Shepherd-soundspin.xml fallback-shepherd.xml; do \
	    sed "s|<!--FIREWALL_INTERFACES-->|$$fwargs|" "files/$$f" > "$$tmp/$$f" ; \
	done ; \
	echo "Installing to $(DEVICE) (variant: $$variant, firewall on: $$(echo $$ifaces))" ; \
	: "Copy to .new then rename: a running executable cannot be written in place" ; \
	$(SCP) build/soundspin root@$(DEVICE):/mnt/nv/soundspin.new ; \
	$(SCP) files/netwatch.sh root@$(DEVICE):/mnt/nv/netwatch.sh.new ; \
	$(SCP) files/firewall.sh root@$(DEVICE):/mnt/nv/firewall.sh.new ; \
	$(SCP) "$$tmp/fallback-shepherd.xml" root@$(DEVICE):/mnt/nv/good-shepherd.xml ; \
	$(SSH) root@$(DEVICE) 'cd /mnt/nv && \
	    chmod +x soundspin.new netwatch.sh.new firewall.sh.new && \
	    mv soundspin.new soundspin && mv netwatch.sh.new netwatch.sh && \
	    mv firewall.sh.new firewall.sh' ; \
	if $(SSH) root@$(DEVICE) 'test -f /mnt/nv/soundspin.conf' ; then \
	    echo "  keeping existing /mnt/nv/soundspin.conf" ; \
	else \
	    echo "  seeding /mnt/nv/soundspin.conf -- EDIT server_url before rebooting" ; \
	    $(SCP) soundspin.conf root@$(DEVICE):/mnt/nv/soundspin.conf ; \
	fi ; \
	$(SSH) root@$(DEVICE) 'mkdir -p /mnt/nv/shepherd && rm -f /mnt/nv/shepherd/Shepherd-*.xml' ; \
	$(SCP) "$$tmp/Shepherd-soundspin.xml" root@$(DEVICE):/mnt/nv/shepherd/ ; \
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
