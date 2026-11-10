"""Shared test setup for the v2 emitter tests."""
import pytest


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "code_cache: the test runs with SNESRECOMP_EMIT_CODE_CACHE as set")


@pytest.fixture(autouse=True)
def _code_cache_opt_in(request, monkeypatch):
    """SNESRECOMP_EMIT_CODE_CACHE renames every body to F__body and leaves F to
    the program-level dispatch file, so a test that compiles a lone function
    and calls it by name cannot run under it. Tests opt in with
    @pytest.mark.code_cache (test_emit_code_cache.py)."""
    if request.node.get_closest_marker("code_cache") is None:
        monkeypatch.delenv("SNESRECOMP_EMIT_CODE_CACHE", raising=False)
