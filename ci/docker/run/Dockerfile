FROM python:3.7.3-stretch

# Install gcloud
RUN echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] http://packages.cloud.google.com/apt cloud-sdk main" | tee -a /etc/apt/sources.list.d/google-cloud-sdk.list && \
    curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | apt-key --keyring /usr/share/keyrings/cloud.google.gpg  add - && \
    apt-get update -y && \
    apt-get -qq install -y --no-install-recommends google-cloud-sdk && \
    apt-get clean

# Install Python dependencies.
RUN pip install --upgrade google-cloud-bigquery && \
    pip install --upgrade pandas

WORKDIR /app
COPY . /app

ENV PATH="/app:${PATH}"

#chmod all the things.
#RUN chmod +x run-test.sh
