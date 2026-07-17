import sysconfig

if not sysconfig.get_config_var("Py_GIL_DISABLED"):
    raise ImportError(
        "stormweaver requires a free-threaded (3.14t) Python build; "
        "install one with: uv python install 3.14t"
    )

from stormweaver import scenario
from stormweaver._stormweaver import (
    ActionFactory,
    ActionRegistry,
    ActionStatistics,
    AllConfig,
    DdlConfig,
    DmlConfig,
    LoggedSQL,
    Metadata,
    QueryResult,
    Random,
    RandomWorker,
    SqlError,
    TimingStatistics,
    VariableConfig,
    VariableSpec,
    Worker,
    WorkerStatistics,
    WorkloadParams,
    __version__,
    connect_mysql,
    connect_pg,
    default_action_registry,
)
from stormweaver.actions import action
from stormweaver.backends import DatabaseBackend, MySQL, Postgres
from stormweaver.config import Config
from stormweaver.workload import Workload
from stormweaver.wrappers import (
    ExecPrefixWrapper,
    RRWrapper,
    ServerWrapper,
    ValgrindWrapper,
    WrapCtx,
)

__all__ = [
    "ActionFactory",
    "ActionRegistry",
    "ActionStatistics",
    "AllConfig",
    "Config",
    "DatabaseBackend",
    "DdlConfig",
    "DmlConfig",
    "ExecPrefixWrapper",
    "LoggedSQL",
    "Metadata",
    "MySQL",
    "Postgres",
    "QueryResult",
    "RRWrapper",
    "Random",
    "RandomWorker",
    "ServerWrapper",
    "SqlError",
    "TimingStatistics",
    "ValgrindWrapper",
    "VariableConfig",
    "VariableSpec",
    "Worker",
    "WorkerStatistics",
    "Workload",
    "WorkloadParams",
    "WrapCtx",
    "__version__",
    "action",
    "connect_mysql",
    "connect_pg",
    "default_action_registry",
    "scenario",
]
