# Basic CI scenario: randomized workload cycles against a single postgres
# primary with per-cycle restarts.

import logging

from stormweaver import scenario

logger = logging.getLogger("scenario.basic")


def add_arguments(parser):
    scenario.add_common_arguments(parser)
    parser.set_defaults(duration=30, workers=4, repeat=2, var_fuzz="safe")


def main(args):
    opts = scenario.finalize(args)

    with scenario.single_pg(opts) as ctx:
        for cycle in range(opts.repeat):
            ctx.workload.run()
            ctx.restart_and_wait()
            logger.info("cycle %d/%d done", cycle + 1, opts.repeat)

        ctx.validate_metadata_or_warn()

    print("Scenario completed successfully")
