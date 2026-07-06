from collections.abc import Callable
from typing import Any

from stormweaver._stormweaver import ActionRegistry

ActionFn = Callable[[Any, Any, Any], None]


def action(
    registry: ActionRegistry, name: str, weight: int = 10
) -> Callable[[ActionFn], ActionFn]:
    """Register a python callable as a workload action.

    fn(metadata, rand, connection)

    The objects passed to the action are borrowed references owned by the
    worker, valid only during the workload run - do not stash them.
    Statistics accessors become valid only after the workload completes
    (post-join).
    """

    def decorator(fn: ActionFn) -> ActionFn:
        registry.register_python(name, weight, fn)
        return fn

    return decorator
