using Xunit;

// The hub tests use real (short) timeouts and the integration tests run a simulator in real time:
// running them one after the other keeps them reliable on busy machines.
[assembly: CollectionBehavior(DisableTestParallelization = true)]
