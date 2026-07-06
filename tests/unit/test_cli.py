from stormweaver.cli import parse_args


def test_extra_args_collected():
    args = parse_args(["scen.py", "-i", "/pg", "--repeat", "7"])
    assert args.scenario == "scen.py"
    assert args.install_dir == "/pg"
    assert args.extra == ["--repeat", "7"]


def test_no_extra_args_is_empty_list():
    args = parse_args(["scen.py"])
    assert args.extra == []


def test_prefix_like_scenario_flag_not_swallowed():
    args = parse_args(["scen.py", "--conf", "x"])
    assert args.config == "config/stormweaver.toml"
    assert args.extra == ["--conf", "x"]
