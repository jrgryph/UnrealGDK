import datetime
import sys
import os
import json
from google.cloud import bigquery

# Define target upload location
PROJECT = "venator-unsepulcher-3029873"
DATASET = "benchmarks"
TABLE = "records"

# Initialize bigquery client
client = bigquery.Client(project=PROJECT)
table_ref = client.dataset(DATASET).table(TABLE)
table = client.get_table(table_ref)

echo("json files:")
for json_file in os.listdir("ci/test_summaries"):
    json_obj = json.loads(json_file)
    print(json_obj)

# Read rows from files that have been generated per Buildkite step
rows_to_insert = [json.loads(summary_file) for summary_file in os.listdir("ci/test_summaries")]

# Upload rows
errors = client.insert_rows(table, rows_to_insert)

# Handle errors
for error in errors:
    print(f"Error inserting row at index {error['index']}: {error['errors']}", file=sys.stderr)

assert errors == []
