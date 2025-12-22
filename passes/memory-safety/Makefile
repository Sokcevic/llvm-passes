FINAL_RTLIB = build/libMemorySafetyRuntime.a

all: cmake-build rtlib

rtlib: rust

.PHONY clean:
	rm -rf build target

build_dir:
	mkdir -p build

rust: build_dir
	cargo build --release
	cp target/release/libmemory_safety_runtime.a $(FINAL_RTLIB)

cmake-build: build_dir
	cd build; cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
	ninja -Cbuild
