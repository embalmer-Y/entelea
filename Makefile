PYTHON := $(CURDIR)/.venv/bin/python
BASE ?= import-baseline

.PHONY: check hygiene affect-test agent-test firmware-build firmware-test style

check: hygiene affect-test
	$(PYTHON) firmware/tests/safety_check/check_safety_contract.py
	$(PYTHON) -m pytest firmware/tests/pytest -q

hygiene:
	$(PYTHON) scripts/check_repo.py
	$(PYTHON) -m ruff check scripts

affect-test:
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

style:
	$(PYTHON) scripts/check_style.py "$(BASE)"
