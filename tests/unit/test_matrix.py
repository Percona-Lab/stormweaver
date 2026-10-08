import pytest

pytest_plugins = ["pytester"]

CONFTEST = """
from stormweaver.matrix import register_dimension

def pytest_configure(config):
    register_dimension(config, "flavor", default="plain",
                       available=lambda: ["plain", "spicy", "sweet"])
"""

TEST = """
def test_uses(flavor):
    assert flavor in ("plain", "spicy", "sweet")

def test_plain():
    assert True
"""


@pytest.fixture
def suite(pytester):
    pytester.makeconftest(CONFTEST)
    pytester.makepyfile(TEST)
    return pytester


def test_default_single_value(suite):
    r = suite.runpytest("-q")
    r.assert_outcomes(passed=2)
    ids = "\n".join(suite.runpytest("--collect-only", "-q").outlines)
    assert "test_uses[flavor=plain]" in ids


def test_matrix_option(suite):
    r = suite.runpytest("-q", "--matrix", "flavor=spicy,sweet")
    r.assert_outcomes(passed=3)


def test_full(suite):
    r = suite.runpytest("-q", "--full")
    r.assert_outcomes(passed=4)


def test_unknown_value_errors(suite):
    r = suite.runpytest("-q", "--matrix", "flavor=bitter")
    assert r.ret != 0
    out = "\n".join(r.outlines + r.errlines)
    assert "bitter" in out and "flavor" in out


def test_unknown_dimension_errors(suite):
    r = suite.runpytest("-q", "--matrix", "color=red")
    assert r.ret != 0
    out = "\n".join(r.outlines + r.errlines)
    assert "color" in out


def test_malformed_spec_errors(suite):
    r = suite.runpytest("-q", "--matrix", "flavor")
    assert r.ret != 0


def test_repeatable(suite):
    suite.makeconftest(
        CONFTEST
        + """
    register_dimension(config, "size", default="s", available=lambda: ["s", "l"])
"""
    )
    suite.makepyfile("def test_two(flavor, size):\n    pass\n")
    r = suite.runpytest("-q", "--matrix", "flavor=plain,spicy", "--matrix", "size=s,l")
    r.assert_outcomes(passed=4)
