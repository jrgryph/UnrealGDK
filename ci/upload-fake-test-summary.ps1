# Define and upload test summary JSON artifact for longer-term test metric tracking (see upload-test-metrics.sh)
$test_summary = [pscustomobject]@{
    time = Get-Date -UFormat %s
    build_url = "https://google.com"
    platform = "Wondoos"
    passed_all_tests = $true
    tests_duration_seconds = 15.15
    num_tests = 1 + 2
    num_gdk_tests = 4
}
$test_summary | ConvertTo-Json | Set-Content -Path "./test_summary_$env:BUILDKITE_STEP_ID.json"

buildkite-agent "artifact" "upload" "./test_summary_$env:BUILDKITE_STEP_ID.json"
