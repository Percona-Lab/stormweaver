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
    assert "test_uses[flavor-plain]" in ids


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


def test_k_selects_dash_id(suite):
    r = suite.runpytest("-q", "--matrix", "flavor=spicy,sweet", "-k", "flavor-spicy")
    r.assert_outcomes(passed=1)


def test_empty_values_errors(suite):
    r = suite.runpytest("-q", "--matrix", "flavor=")
    assert r.ret != 0


def test_repeated_dimension_errors(suite):
    r = suite.runpytest("-q", "--matrix", "flavor=plain", "--matrix", "flavor=spicy")
    assert r.ret != 0
    assert "given twice" in "\n".join(r.outlines + r.errlines)


def test_duplicate_values_deduplicated(suite):
    r = suite.runpytest(
        "--collect-only", "-q", "--matrix", "flavor=spicy,spicy", "-k", "test_uses"
    )
    assert sum("test_uses[" in line for line in r.outlines) == 1


COUNTING_CONFTEST = """
from stormweaver.matrix import register_dimension

def _avail():
    with open(r"{counter}", "a") as f:
        f.write("x")
    return ["plain", "spicy", "sweet"]

def pytest_configure(config):
    register_dimension(config, "flavor", default="plain", available=_avail)
"""

THREE_TESTS = """
def test_a(flavor):
    pass

def test_b(flavor):
    pass

def test_c(flavor):
    pass
"""


@pytest.mark.parametrize(
    "args,calls",
    [
        ([], 0),
        (["--full"], 1),
        (["--matrix", "flavor=spicy,sweet"], 1),
    ],
)
def test_available_resolved_once(pytester, args, calls):
    counter = pytester.path / "counter"
    pytester.makeconftest(COUNTING_CONFTEST.format(counter=counter))
    pytester.makepyfile(THREE_TESTS)
    pytester.runpytest("-q", *args)
    got = counter.read_text() if counter.exists() else ""
    assert len(got) == calls
