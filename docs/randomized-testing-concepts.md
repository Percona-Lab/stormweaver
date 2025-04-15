# Randomized testing concepts

A main goal of StormWeaver is to allow scripted randomized testing.

This feature uses the following parts:

## Workload

A workload is a randomized test execution.

It is executed with *some timelimit*, using *some number of `Worker`s*.

## Worker

A Worker is a thread executing `Action`s.

A randomized thread usually runs one or more workers, and each worker repeatedly chooses a random action and executes it, until the `time limit` is reached.

## Action

An Action is something executed during the test, usually in the form of one or more SQL statements.

For example:

* Creating a table
* Dropping a table
* Altering a table
* Inserting some data
* ....

While there are some actions implemented in the C++ framework, it also makes it possible to define custom actions using an `ActionRegistry`

## ActionRegistry

An ActionRegistry is a collection of possible actions, and a definition of how these actions can be constructed.
Other than the action definition itself, each Action in a Registry also has a weight, which determines the chance of its execution.

For each random action selection, the sum of all weights is calculated, and then a random number is choosen in the (0, sumOfWeights) range.
Then the action is determined on where this number lands in the list of actions.

