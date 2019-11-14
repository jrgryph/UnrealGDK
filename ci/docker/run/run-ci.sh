#!/usr/bin/env bash

set -e -u -o pipefail

cd "$(dirname "${0}")/../../"

set -e -u -o pipefail

pwd

docker build \
    -t run \
    -f docker/run/Dockerfile \
    .

docker run \
  --e="BUILDKITE_BUILD_ID=${BUILDKITE_BUILD_ID}" \
  --volume="/usr/local/bin/imp-ci:/usr/local/bin/imp-ci" \
  --volume="/usr/bin/buildkite-agent:/usr/bin/buildkite-agent" \
  -w="/app" \
  run \
  /app/docker/run/upload-test-metrics.sh
