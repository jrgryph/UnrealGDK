#!/bin/bash
#set -euo pipefail

# Fetch the test summary artifacts uploaded earlier
mkdir "./test_summaries"
buildkite-agent artifact download "*test_summary*.json" "./test_summaries"

export GOOGLE_APPLICATION_CREDENTIALS="$(mktemp)" 

ls /usr/local/bin/imp-ci

# Fetch Google credentials so that our python script can upload the metrics to the GCS bucket.
/usr/local/bin/imp-ci secrets read --environment=production --buildkite-org=improbable \
    --secret-type=gce-key-pair --secret-name=prod-research-gcp \
    --write-to=$GOOGLE_APPLICATION_CREDENTIALS

# Upload test summaries to GCS
python "/app/docker/run/upload-test-metrics.py"
