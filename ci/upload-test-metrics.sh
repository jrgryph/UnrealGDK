#!/bin/bash
set -euo pipefail

export GOOGLE_APPLICATION_CREDENTIALS="$(mktemp)"

# Fetch Google credentials so that our python script can upload the metrics to the GCS bucket.
imp-ci secrets read --environment=production --buildkite-org=improbable \
    --secret-type=gce-key-pair --secret-name=prod-research-gcp \
    --write-to=$GOOGLE_APPLICATION_CREDENTIALS

# TODO: fetch and pass all required values (see upload-test-metrics.py for which)
# TODO: set values in buildkite metadata (when tests are run)
# TODO: handle what to do with multiple builds running tests. The time it takes to complete tests may be different for a different OS...

if num_tests_run=$(buildkite-agent meta-data get "number-tests-run"); then
    echo "Extracted number of tests run from metadata: $num_tests_run"
    python "ci/upload-test-metrics.py" --num_tests_run "$num_tests_run"
else
    echo "Failed to extract number of tests run from metadata. This may be because no tests were run."
fi
