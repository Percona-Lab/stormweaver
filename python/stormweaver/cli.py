import argparse
import importlib.util
import logging
import os
import sys
import traceback
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType

from stormweaver.entropy import EncryptionMismatchError
from stormweaver.events import emit_run_header
from stormweaver.log import init_run_logging, record_outcome

logger = logging.getLogger(__name__)

# scenario-facing failures raised on purpose, message is enough for these
EXPECTED_ERRORS = (RuntimeError, ValueError, EncryptionMismatchError)


def build_parser(
    *, add_help: bool = True, scenario_required: bool = True
) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="stormweaver", allow_abbrev=False, add_help=add_help
    )
    parser.add_argument(
        "scenario",
        nargs=None if scenario_required else "?",
        help="Scenario file to execute",
    )
    parser.add_argument(
        "-c",
        "--config",
        default="config/stormweaver.toml",
        help="Configuration file",
    )
    parser.add_argument(
        "-i", "--install-dir", default="", help="database installation directory"
    )
    parser.add_argument(
        "--log-mode",
        choices=["split", "unified"],
        default=None,
        help="log layout, overrides scenario LOG_MODE",
    )
    parser.add_argument(
        "--log-splits",
        action="store_true",
        default=None,
        help="also write per-connection/worker files in unified mode",
    )
    verbosity = parser.add_mutually_exclusive_group()
    verbosity.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable debug logging",
    )
    verbosity.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Only log warnings and errors",
    )
    return parser


def _load_scenario(path: str) -> tuple[ModuleType | None, str | None]:
    """Import a scenario file. Returns (module, error): one is always None."""
    spec = importlib.util.spec_from_file_location("scenario", path)
    if spec is None or spec.loader is None:
        return None, f"Error: cannot load scenario file: {path}"

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module

    # let a scenario import sibling modules (helpers, conftest) from its own
    # directory, matching how pytest puts the test dir on sys.path
    scenario_dir = str(Path(path).resolve().parent)
    if scenario_dir not in sys.path:
        sys.path.insert(0, scenario_dir)

    # module load happens before logging init so LOG_MODE can steer it;
    # import-time failures only reach stderr
    try:
        spec.loader.exec_module(module)
    except Exception:
        traceback.print_exc()
        return None, f"Error: failed to load scenario: {path}"

    return module, None


def main(argv: Sequence[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    # phase 1: grab the scenario path without triggering --help or choking on
    # scenario-specific flags, so we can import the scenario and let it add its
    # own options to the real parser below
    pre = build_parser(add_help=False, scenario_required=False)
    pre_args, _ = pre.parse_known_args(argv)

    module = None
    if pre_args.scenario is not None and os.path.isfile(pre_args.scenario):
        module, error = _load_scenario(pre_args.scenario)
        if error:
            print(error, file=sys.stderr)
            return 1

    # phase 2: full parse with the scenario's options folded in, so --help
    # lists everything and unknown/mistyped flags are rejected
    parser = build_parser()
    if module is not None and hasattr(module, "add_arguments"):
        module.add_arguments(parser)
    args = parser.parse_args(argv)

    if args.verbose:
        level = logging.DEBUG
    elif args.quiet:
        level = logging.WARNING
    else:
        level = logging.INFO

    if not os.path.isfile(args.scenario):
        print(f"Error: scenario file not found: {args.scenario}", file=sys.stderr)
        return 1

    # file exists, so phase 1 already imported it (or bailed out)
    assert module is not None

    if not hasattr(module, "main"):
        print(
            f"Error: scenario {args.scenario} has no main() function",
            file=sys.stderr,
        )
        return 1

    mode = (
        args.log_mode
        or os.environ.get("STORMWEAVER_LOG_MODE")
        or getattr(module, "LOG_MODE", "split")
    )
    if mode not in ("split", "unified"):
        print(f"Error: unknown log mode: {mode}", file=sys.stderr)
        return 1
    if args.log_splits is not None:
        splits = args.log_splits
    else:
        splits = os.environ.get("STORMWEAVER_LOG_SPLITS") == "1" or bool(
            getattr(module, "LOG_SPLITS", False)
        )

    init_run_logging(Path(args.scenario).stem, level, mode=mode, splits=splits)
    emit_run_header(scenario=Path(args.scenario).stem, mode=mode)

    try:
        result = module.main(args)
    except SystemExit as e:
        # scenarios use SystemExit('msg') for usage errors, argparse uses ints
        if e.code is None or isinstance(e.code, int):
            rc = e.code or 0
            record_outcome("scenario result=" + ("passed" if rc == 0 else "failed"))
            return rc
        logger.error("scenario failed: %s", e.code)
        record_outcome("scenario result=failed")
        return 1
    except EXPECTED_ERRORS as e:
        logger.error("scenario failed: %s", e)
        logger.debug("traceback:", exc_info=True)
        record_outcome("scenario result=failed")
        return 1
    except Exception:
        logger.exception("scenario failed")
        record_outcome("scenario result=failed")
        return 1

    rc = 0 if result is None else int(result)
    record_outcome("scenario result=" + ("passed" if rc == 0 else "failed"))
    return rc
