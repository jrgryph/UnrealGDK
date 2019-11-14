FROM python:3.7.3-stretch

# Install Python dependencies.
RUN pip install --upgrade google-cloud-bigquery

WORKDIR /app
COPY . /app

# extend path to get access to imp-ci
ENV PATH="/usr/local/bin:${PATH}"

RUN chmod +x /app/docker/run/upload-test-metrics.sh
