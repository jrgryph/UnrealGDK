#!/bin/bash
set -euo pipefail

export GOOGLE_APPLICATION_CREDENTIALS="$(mktemp)"

# Fetch Google credentials so that our python script can upload the metrics to the GCS bucket.
imp-ci secrets read --environment=production --buildkite-org=improbable \
    --secret-type=gce-key-pair --secret-name=prod-research-gcp \
    --write-to=$GOOGLE_APPLICATION_CREDENTIALS

# TODO: fetch and pass metrics via .json artifact upload (see upload-test-metrics.py for which)

mkdir "ci/test_summaries"

buildkite-agent artifact download "*test_summary.json" "ci/test_summaries"

python "ci/upload-test-metrics.py"
