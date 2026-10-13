"""Opt-in test matrix dimensions, parametrized by fixture name."""

import itertools
from collections.abc import Callable
from dataclasses import dataclass, field

import pytest

_KEY = pytest.StashKey["dict[str, Dimension]"]()


@dataclass
class Dimension:
    name: str
    default: str
    available: Callable[[], list[str]]
    _values: list[str] | None = field(default=None, init=False, repr=False)

    def values(self, config: pytest.Config) -> list[str]:
        if self._values is None:
            self._values = self._resolve(config)
        return self._values

    def _resolve(self, config: pytest.Config) -> list[str]:
        explicit = _parse_matrix_option(config).get(self.name)
        if explicit is not None:
            avail = self.available()
            unknown = [v for v in explicit if v not in avail]
            if unknown:
                raise pytest.UsageError(
                    f"--matrix: unknown value(s) {', '.join(unknown)} for dimension "
                    f"{self.name!r}; available: {', '.join(avail)}"
                )
            return explicit
        if _mode(config) is not None:
            return self.available()
        return [self.default]


def _mode(config: pytest.Config) -> str | None:
    values = config.getoption("--all-values")
    combos = config.getoption("--all-combinations")
    if values and combos:
        raise pytest.UsageError(
            "--all-values and --all-combinations are mutually exclusive"
        )
    if values:
        return "values"
    if combos:
        return "combinations"
    return None


def _parse_matrix_option(config: pytest.Config) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for spec in config.getoption("--matrix") or []:
        if "=" not in spec:
            raise pytest.UsageError(f"--matrix expects name=v1,v2, got {spec!r}")
        name, raw = spec.split("=", 1)
        name = name.strip()
        values = list(dict.fromkeys(v.strip() for v in raw.split(",") if v.strip()))
        if not values:
            raise pytest.UsageError(f"--matrix: no values for dimension {name!r}")
        if name in out:
            raise pytest.UsageError(f"--matrix: dimension {name!r} given twice")
        out[name] = values
    return out


def dimensions(config: pytest.Config) -> dict[str, Dimension]:
    return config.stash.setdefault(_KEY, {})


def register_dimension(
    config: pytest.Config,
    name: str,
    *,
    default: str,
    available: Callable[[], list[str]],
) -> None:
    dimensions(config)[name] = Dimension(name, default, available)


def validate(config: pytest.Config) -> None:
    _mode(config)
    dims = dimensions(config)
    for name in _parse_matrix_option(config):
        if name not in dims:
            raise pytest.UsageError(
                f"--matrix: unknown dimension {name!r}; "
                f"registered: {', '.join(dims) or 'none'}"
            )
    for dim in dims.values():
        dim.values(config)


def _direct_params(metafunc: pytest.Metafunc) -> set[str]:
    # names the test parametrizes itself win over same-named dimensions
    out: set[str] = set()
    for mark in metafunc.definition.iter_markers("parametrize"):
        names = mark.args[0] if mark.args else mark.kwargs.get("argnames", "")
        if isinstance(names, str):
            names = [n.strip() for n in names.split(",")]
        out.update(names)
    return out


def combinations(config: pytest.Config, names: list[str]) -> list[tuple[str, ...]]:
    dims = dimensions(config)
    if _mode(config) != "values":
        return list(itertools.product(*(dims[n].values(config) for n in names)))
    # explicit --matrix dimensions stay cartesian, the others vary one at a time
    explicit = _parse_matrix_option(config)
    fixed = [n for n in names if n in explicit]
    free = [n for n in names if n not in explicit]
    out: list[tuple[str, ...]] = []
    for combo in itertools.product(*(dims[n].values(config) for n in fixed)):
        base = dict(zip(fixed, combo, strict=True)) | {n: dims[n].default for n in free}
        variants = [base]
        variants += [
            base | {n: v}
            for n in free
            for v in dims[n].values(config)
            if v != dims[n].default
        ]
        for var in variants:
            t = tuple(var[n] for n in names)
            if t not in out:
                out.append(t)
    return out


def generate(metafunc: pytest.Metafunc) -> None:
    direct = _direct_params(metafunc)
    names = [
        n
        for n in dimensions(metafunc.config)
        if n in metafunc.fixturenames and n not in direct
    ]
    if not names:
        return
    combos = combinations(metafunc.config, names)
    ids = ["-".join(f"{n}-{v}" for n, v in zip(names, t, strict=True)) for t in combos]
    if len(names) == 1:
        metafunc.parametrize(names[0], [t[0] for t in combos], ids=ids)
    else:
        metafunc.parametrize(names, combos, ids=ids)
