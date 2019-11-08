import argparse
import datetime
import sys
import os
from google.cloud import bigquery

# parse command line arguments
parser = argparse.ArgumentParser(description="Record benchmark results")
parser.add_argument('--num_tests_run', required=True, type=int, help='The number of tests that has been run.')
parser.add_argument('--num_tests_passed', required=True, type=int, help='The number of tests that were passed.')
parser.add_argument('--testing_duration_seconds', required=True, type=int, help='How long testing took in seconds.')
parser.add_argument('--test_report_url', required=True, type=int, help='URL for the generated test report.')
args = parser.parse_args()

# Define target upload location
PROJECT = "venator-unsepulcher-3029873"
DATASET = "benchmarks"
TABLE = "records"

# Initialize bigquery client
client = bigquery.Client(project=PROJECT)
table_ref = client.dataset(DATASET).table(TABLE)
table = client.get_table(table_ref)

# Define row and upload it
rows_to_insert = [
    {
        "time": datetime.datetime.now(),
        "num_tests_run": args.num_tests_run,
        "num_tests_passed": args.num_tests_passed,
        "testing_duration-seconds": args.testing_duration_seconds,
        "test_report_url": args.test_report_url,
        "build_url": os.environ['BUILDKITE_BUILD_URL']
    }
]

errors = client.insert_rows(table, rows_to_insert)

# Handle errors
def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

for error in errors:
    eprint(f"Error inserting row at index {error['index']}: {error['errors']}")

assert errors == []
