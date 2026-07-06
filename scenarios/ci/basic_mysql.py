# Basic CI scenario: randomized workload cycles against a single mysql
# server (no restarts).

import logging

from stormweaver import scenario

logger = logging.getLogger("scenario.basic_mysql")


def main(args):
    opts = scenario.parse(
        args, extend=lambda p: p.set_defaults(duration=30, workers=4, repeat=2)
    )

    with scenario.single_mysql(opts) as ctx:
        for cycle in range(opts.repeat):
            ctx.workload.run()
            logger.info("cycle %d/%d done", cycle + 1, opts.repeat)

        ctx.workload.print_report()

        ctx.validate_metadata_or_warn()

    print("Scenario completed successfully")
