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


def test_all_values_single_dimension(suite):
    r = suite.runpytest("-q", "--all-values")
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
        (["--all-values"], 1),
        (["--all-combinations"], 1),
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


def test_direct_parametrize_wins_over_dimension(suite):
    suite.makepyfile(
        test_direct="""
import pytest

@pytest.mark.parametrize(("mode", "flavor"), [("a", "x"), ("b", "y")])
def test_direct(mode, flavor):
    assert (mode, flavor) in {("a", "x"), ("b", "y")}
"""
    )
    r = suite.runpytest("-q", "--all-values", "test_direct.py")
    r.assert_outcomes(passed=2)


DIMS = """
from stormweaver.matrix import register_dimension

def pytest_configure(config):
    register_dimension(config, "flavor", default="plain",
                       available=lambda: ["plain", "spicy", "sweet"])
    register_dimension(config, "size", default="s", available=lambda: ["s", "l"])
    register_dimension(config, "temp", default="hot", available=lambda: ["hot", "cold"])
"""


@pytest.fixture
def dims_suite(pytester):
    pytester.makeconftest(DIMS)
    pytester.makepyfile("def test_two(flavor, size):\n    pass\n")
    return pytester


def _ids(p, *args):
    r = p.runpytest("--collect-only", "-q", *args)
    return sorted(
        line.split("[", 1)[1].rstrip("]") for line in r.outlines if "::test_" in line
    )


def test_all_values_varies_one_dimension_at_a_time(dims_suite):
    assert _ids(dims_suite, "--all-values") == [
        "flavor-plain-size-l",
        "flavor-plain-size-s",
        "flavor-spicy-size-s",
        "flavor-sweet-size-s",
    ]


def test_all_combinations_is_cartesian(dims_suite):
    assert _ids(dims_suite, "--all-combinations") == [
        "flavor-plain-size-l",
        "flavor-plain-size-s",
        "flavor-spicy-size-l",
        "flavor-spicy-size-s",
        "flavor-sweet-size-l",
        "flavor-sweet-size-s",
    ]


def test_matrix_dims_cartesian_rest_one_at_a_time(pytester):
    pytester.makeconftest(DIMS)
    pytester.makepyfile("def test_three(flavor, size, temp):\n    pass\n")
    # per size value: base + 2 flavors + 1 temp
    assert _ids(pytester, "--matrix", "size=s,l", "--all-values") == sorted(
        f"flavor-{f}-size-{s}-temp-{t}"
        for s in ("s", "l")
        for f, t in [
            ("plain", "hot"),
            ("spicy", "hot"),
            ("sweet", "hot"),
            ("plain", "cold"),
        ]
    )
    r = pytester.runpytest("-q", "--matrix", "size=s,l", "--all-combinations")
    r.assert_outcomes(passed=12)


def test_all_values_and_all_combinations_conflict(dims_suite):
    r = dims_suite.runpytest("-q", "--all-values", "--all-combinations")
    assert r.ret != 0
    out = "\n".join(r.outlines + r.errlines)
    assert "--all-values" in out and "--all-combinations" in out


def test_default_run_unchanged_with_dims_suite(dims_suite):
    assert _ids(dims_suite) == ["flavor-plain-size-s"]
