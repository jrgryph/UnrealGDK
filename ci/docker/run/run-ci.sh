#!/usr/bin/env bash

set -e -u -o pipefail

cd "$(dirname "${0}")/../../"

set -e -u -o pipefail

pushd "$(dirname "${0}")/../../"
  docker build \
      -t run \
      -f docker/run/Dockerfile \
      .
popd

docker run \
  --volume="/usr/local/bin/imp-ci:/usr/local/bin/imp-ci" \
  -w="/app" \
  run \
  /app/docker/run/upload-test-metrics.sh
