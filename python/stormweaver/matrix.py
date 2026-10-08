"""Opt-in test matrix dimensions, parametrized by fixture name."""

from collections.abc import Callable
from dataclasses import dataclass

import pytest

_KEY = pytest.StashKey["dict[str, Dimension]"]()


@dataclass
class Dimension:
    name: str
    default: str
    available: Callable[[], list[str]]

    def check(self, values: list[str]) -> None:
        avail = self.available()
        unknown = [v for v in values if v not in avail]
        if unknown:
            raise pytest.UsageError(
                f"--matrix: unknown value(s) {', '.join(unknown)} for dimension "
                f"{self.name!r}; available: {', '.join(avail)}"
            )

    def selected(self, config: pytest.Config) -> list[str]:
        explicit = _parse_matrix_option(config).get(self.name)
        if explicit is not None:
            self.check(explicit)
            return explicit
        if config.getoption("--full"):
            return self.available()
        return [self.default]


def _parse_matrix_option(config: pytest.Config) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for spec in config.getoption("--matrix") or []:
        if "=" not in spec:
            raise pytest.UsageError(f"--matrix expects name=v1,v2, got {spec!r}")
        name, values = spec.split("=", 1)
        out[name.strip()] = [v.strip() for v in values.split(",") if v.strip()]
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
    for name, values in _parse_matrix_option(config).items():
        if name not in dims:
            raise pytest.UsageError(
                f"--matrix: unknown dimension {name!r}; "
                f"registered: {', '.join(dims) or 'none'}"
            )
        dims[name].check(values)


def generate(metafunc: pytest.Metafunc) -> None:
    for name, dim in dimensions(metafunc.config).items():
        if name in metafunc.fixturenames:
            values = dim.selected(metafunc.config)
            metafunc.parametrize(name, values, ids=[f"{name}={v}" for v in values])
