"""Opt-in test matrix dimensions, parametrized by fixture name."""

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
        if config.getoption("--full"):
            return self.available()
        return [self.default]


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


def generate(metafunc: pytest.Metafunc) -> None:
    direct = _direct_params(metafunc)
    for name, dim in dimensions(metafunc.config).items():
        if name in metafunc.fixturenames and name not in direct:
            values = dim.values(metafunc.config)
            metafunc.parametrize(name, values, ids=[f"{name}-{v}" for v in values])
