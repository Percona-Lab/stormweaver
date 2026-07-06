import inspect
import logging
from collections.abc import Callable

import stormweaver._stormweaver as _stormweaver

logger = logging.getLogger(__name__)


class Workload:
    def __init__(
        self,
        workers: int,
        duration: int,
        registry: _stormweaver.ActionRegistry,
        metadata: _stormweaver.Metadata,
        # zero-arg, or one-arg taking the worker name (for per-worker SQL logs)
        node_factory: Callable[..., _stormweaver.LoggedSQL],
        repeat: int = 1,
        max_reconnect_attempts: int = 5,
        action_config: _stormweaver.AllConfig | None = None,
        seed: int = 0,
        worker_name_prefix: str = "",
        worker_setup: Callable[[_stormweaver.RandomWorker, int], None] | None = None,
    ) -> None:
        if workers < 1:
            raise ValueError("workers must be >= 1")
        if duration <= 0:
            raise ValueError("duration must be > 0")
        if repeat < 1:
            raise ValueError("repeat must be >= 1")
        self.num_workers = workers
        self.duration = duration
        self.repeat = repeat
        self.registry = registry
        self.metadata = metadata
        self.node_factory = node_factory
        self.max_reconnect_attempts = max_reconnect_attempts
        self.action_config = action_config
        self.seed = seed
        # worker names become spdlog logger names on the C++ side, and spdlog
        # loggers are get-or-create in a process-wide registry: a name already
        # registered keeps its original log file forever. Give every Workload
        # in the same process a unique prefix or their workers will silently
        # share (append to) each other's log files.
        self.worker_name_prefix = worker_name_prefix
        self.worker_setup = worker_setup
        # persists across run()/start() calls: worker names double as spdlog
        # logger names, get-or-create per process, so they must never repeat
        self._cycle = 0
        self._live: list[_stormweaver.RandomWorker] = []
        self._worker_stats: list[_stormweaver.WorkerStatistics] = []
        # node_factory can take the worker name (for per-worker SQL logs), or
        # nothing, for backwards compat with existing zero-arg factories
        try:
            self._factory_wants_name = (
                len(inspect.signature(node_factory).parameters) >= 1
            )
        except (ValueError, TypeError):  # fmt: skip
            # some callables (builtins, C extensions) have no introspectable
            # signature, assume the zero-arg legacy form
            self._factory_wants_name = False
        self._reports: list[str] = []

    @property
    def workers(self) -> list[_stormweaver.RandomWorker]:
        """Live workers of the running cycle (between start() and wait())."""
        return list(self._live)

    def start(self) -> None:
        """Launch one cycle's worker threads and return immediately."""
        if self._live:
            raise RuntimeError("a workload cycle is already running")
        self._cycle += 1
        workers: list[_stormweaver.RandomWorker] = []
        started: list[_stormweaver.RandomWorker] = []

        try:
            for i in range(self.num_workers):
                # one params object per worker, allows per-worker settings
                params = _stormweaver.WorkloadParams()
                params.max_reconnect_attempts = self.max_reconnect_attempts
                if self.action_config:
                    params.action_config = self.action_config
                # same scenario seed everywhere, C++ derives a distinct
                # per-worker stream by mixing in the worker name
                params.seed = self.seed

                name = f"{self.worker_name_prefix}worker-{self._cycle}-{i + 1}"
                connector = (
                    (lambda n=name: self.node_factory(n))
                    if self._factory_wants_name
                    else self.node_factory
                )
                worker = _stormweaver.RandomWorker(
                    name,
                    connector,
                    params,
                    self.metadata,
                    self.registry,
                )
                workers.append(worker)

            if self.worker_setup:
                for i, worker in enumerate(workers):
                    self.worker_setup(worker, i)

            # run_thread spawns a C++ std::thread internally;
            # its duration argument is the single source of truth
            for w in workers:
                w.run_thread(self.duration)
                started.append(w)
        except BaseException:
            # workers cannot be cancelled, wait for them before unwinding
            if started:
                logger.warning(
                    "waiting for %d running workers to finish (up to %ss)",
                    len(started),
                    self.duration,
                )
                for w in started:
                    w.join()
            raise

        self._live = workers

    def wait(self) -> None:
        """Join the running cycle's workers and capture their statistics."""
        workers, self._live = self._live, []
        if not workers:
            raise RuntimeError("no workload cycle is running")

        # join waits for each C++ thread to finish
        for w in workers:
            w.join()

        # Capture reports as strings while workers are still alive.
        # statistics() ties the worker's lifetime to the returned
        # object (reference_internal), so keeping it here keeps the
        # worker's stats readable after the cycle ends.
        for w in workers:
            stats = w.statistics()
            self._reports.append(stats.report())
            self._worker_stats.append(stats)

    def run(self) -> None:
        self._reports = []
        self._worker_stats = []
        for cycle in range(self.repeat):
            logger.info("Workload cycle %d/%d", cycle + 1, self.repeat)
            self.start()
            self.wait()
            logger.info("Workload cycle %d complete", cycle + 1)

    def worker_statistics(self) -> list[_stormweaver.WorkerStatistics]:
        return self._worker_stats

    def print_report(self) -> None:
        for report in self._reports:
            print(report)
