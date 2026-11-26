BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
OFFLINE_LIVE_TAGS := "[paper_exchange],[paper_trading],[quote_holds],[normalized_trade],[lighter_market_data],[ohlcv],[state_machines]"

.PHONY: help configure build test backtest clean

help:
	@echo "Targets:"
	@echo "  make build     Configure (if needed) and compile all C++/Rust binaries"
	@echo "  make test      Build and run trading_engine_tests + offline live_infra_tests"
	@echo "  make backtest  Build and run the example sma_crossover backtest"
	@echo "  make clean     Remove $(BUILD_DIR)/"

configure:
	@if [ ! -f "$(BUILD_DIR)/CMakeCache.txt" ]; then \
		ORT="$${ONNXRUNTIME_ROOT:-$$(./scripts/fetch_onnxruntime.sh)}"; \
		GEN=""; command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"; \
		TC=""; \
		if [ -n "$$VCPKG_ROOT" ] && [ -f "$$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then \
			TC="-DCMAKE_TOOLCHAIN_FILE=$$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"; \
		elif [ -f "./vcpkg/scripts/buildsystems/vcpkg.cmake" ]; then \
			TC="-DCMAKE_TOOLCHAIN_FILE=$$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake"; \
		fi; \
		STRAT=""; \
		[ -n "$(KERONIS_STRATEGY_DIRS)" ] && STRAT="-DKERONIS_STRATEGY_DIRS=$(KERONIS_STRATEGY_DIRS)"; \
		cmake -S . -B "$(BUILD_DIR)" $$GEN -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" $$TC -DONNXRUNTIME_ROOT="$$ORT" $$STRAT; \
	fi

build: configure
	cmake --build "$(BUILD_DIR)" -j"$(JOBS)"

test: build
	./$(BUILD_DIR)/bin/trading_engine_tests
	./$(BUILD_DIR)/bin/live_infra_tests $(OFFLINE_LIVE_TAGS)

backtest: build
	./examples/quickstart_backtest/run.sh ./$(BUILD_DIR)/bin/backtest_runner

clean:
	rm -rf "$(BUILD_DIR)"
