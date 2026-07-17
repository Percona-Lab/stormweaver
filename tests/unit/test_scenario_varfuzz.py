import argparse

from stormweaver import scenario


def _parse(*argv):
    parser = argparse.ArgumentParser()
    scenario.add_common_arguments(parser)
    return parser.parse_args(list(argv))


def test_var_fuzz_defaults_off():
    opts = _parse()
    assert opts.var_fuzz == "off"
    assert opts.seed == 0


def test_var_fuzz_choices():
    assert _parse("--var-fuzz", "semantics").var_fuzz == "semantics"
    assert _parse("--seed", "42").seed == 42
