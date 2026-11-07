PYTHON := $(CURDIR)/.venv/bin/python
BASE ?= import-baseline

.PHONY: check hygiene affect-test agent-test firmware-build firmware-test firmware-p4-build \
	firmware-p4-store-build firmware-p4-net-build firmware-p4-tls-build firmware-p4-sd-build \
	firmware-p4-sd-detect-build \
	firmware-p4-i2s-build firmware-p4-i2s-candidate-build firmware-p4-i2s-candidate32-build \
	firmware-p4-i2s-stop-build firmware-p4-i2s-stop-candidate-build \
	firmware-p4-touch-id-build firmware-p4-touch-build firmware-p4-display-build \
	firmware-p4-boot-trace-build style

check: hygiene affect-test
	$(PYTHON) firmware/tests/safety_check/check_safety_contract.py
	$(PYTHON) -m pytest firmware/tests/pytest -q

hygiene:
	$(PYTHON) scripts/check_repo.py
	$(PYTHON) -m ruff check scripts lib/affect/tests/reference

affect-test:
	$(PYTHON) lib/affect/tests/reference/galatea.py --check lib/affect/tests/galatea_reference.h
	cmake -S lib/affect -B build/affect -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/affect
	ctest --test-dir build/affect --output-on-failure

agent-test:
	cd agent && ENTELEA_ROOT="$(CURDIR)" .venv/bin/python -m pytest tests -q
	cd agent && .venv/bin/python -m ruff check .
	cd agent && .venv/bin/python -m tessera_agent.skills.sync_check

firmware-build:
	bash scripts/firmware.sh build

firmware-test:
	bash scripts/firmware.sh test

firmware-p4-build:
	bash scripts/firmware.sh build-p4

firmware-p4-store-build:
	bash scripts/firmware.sh build-p4-store

firmware-p4-net-build:
	bash scripts/firmware.sh build-p4-net

firmware-p4-tls-build:
	bash scripts/firmware.sh build-p4-tls

firmware-p4-sd-build:
	bash scripts/firmware.sh build-p4-sd

firmware-p4-sd-detect-build:
	bash scripts/firmware.sh build-p4-sd-detect

firmware-p4-i2s-build:
	bash scripts/firmware.sh build-p4-i2s

firmware-p4-i2s-candidate-build:
	bash scripts/firmware.sh build-p4-i2s-candidate

firmware-p4-i2s-candidate32-build:
	bash scripts/firmware.sh build-p4-i2s-candidate32

firmware-p4-i2s-stop-build:
	bash scripts/firmware.sh build-p4-i2s-stop

firmware-p4-i2s-stop-candidate-build:
	bash scripts/firmware.sh build-p4-i2s-stop-candidate

firmware-p4-touch-id-build:
	bash scripts/firmware.sh build-p4-touch-id

firmware-p4-boot-trace-build:
	bash scripts/firmware.sh build-p4-boot-trace

firmware-p4-touch-build:
	bash scripts/firmware.sh build-p4-touch

firmware-p4-display-build:
	bash scripts/firmware.sh build-p4-display

style:
	$(PYTHON) scripts/check_style.py "$(BASE)"
