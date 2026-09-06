IMAGE := sendspin-soundtouch-build

.PHONY: build clean image

image:
	docker build -t $(IMAGE) .

build: image
	docker run --rm -v "$(PWD):/work" --platform linux/amd64 $(IMAGE) \
	    bash -c "cmake -B build \
	        -DCMAKE_TOOLCHAIN_FILE=/work/cmake/toolchain-cortex-a8.cmake \
	        -DCMAKE_BUILD_TYPE=Release -G Ninja && \
	        cmake --build build"

all: image build

deploy: all
ifndef DEVICE
	$(error DEVICE not set. Usage: make deploy DEVICE=<device-ip>)
endif
	scp -O -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa build/soundspin root@$(DEVICE):/tmp/
	@echo "Deployed to $(DEVICE):/tmp/"

clean:
	rm -rf build