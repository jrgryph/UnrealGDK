FROM python:3.7.3-stretch

# Install Python dependencies.
RUN pip install --upgrade google-cloud-bigquery

WORKDIR /app
COPY . /app

ENV PATH="/app:${PATH}"

#chmod all the things.
RUN chmod +x /app/docker/run/upload-test-metrics.sh
