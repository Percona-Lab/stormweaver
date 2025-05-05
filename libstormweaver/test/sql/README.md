SQL tests

The goal of these tests is to run simple single threaded deterministic threads againts a real SQL server, and make sure that we get no errors.

The metadata/action system can result in failing SQLs, but this should only happen with (a) a database server bug (b) as a sideeffect of concurrency.

A single threaded execution shouldn't result in any errors.

This test just executes a few hundred SQL statements of each kind, and reports success if everything went well.
